#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_Scheduler.h"

// __OSHasSchedulerLock() is only read inside cemu_assert_debug, which is a no-op in
// release. The TLS counter that backs it is therefore dead in release — but writes
// to it still cost a tlsdesc_resolver_dynamic call on Android's general-dynamic TLS
// model, which showed up at ~1.8% on core 1's hot path (most of it via the scheduler
// lock taken from OSSend/ReceiveMessage's wake/block paths). Conditionally compile
// it out and keep the lock funcs noinline so LTO can't propagate the simplified
// bodies into every caller (which is what caused the earlier regression when we
// tried this without an inline barrier).
#ifdef CEMU_DEBUG_ASSERT
thread_local sint32 s_schedulerLockCount = 0;
#endif

#if BOOST_OS_WINDOWS
#include <synchapi.h>
CRITICAL_SECTION s_csSchedulerLock;
#else
#include <atomic>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <thread>

// Sharded scheduler lock.
//
// Writer mode (taken by __OSLockScheduler with obj=null) excludes all other
// writers and all shard holders. Required for fiber switches, thread
// create/destroy, and any path that holds the lock across a context switch.
//
// Shard mode (taken by __OSLockSchedulerShard) excludes only writers and other
// shard holders on the same hash slot. Two shard holders on different slots
// run in parallel. Intended for wake helpers operating on a single sync
// primitive — under WW HD core 1's tight OSSend/ReceiveMessage loop those
// wakes are the bulk of the lock traffic (~5M ops/sec).
//
// Implementation: per-shard futex spinlock array. Writer mode acquires ALL
// shards in order (deadlock-free: shard mode only ever holds one shard, and
// writer mode's fixed acquisition order serializes writers). No global
// reader-count/writer-flag atomics — those were the source of cross-shard
// contention in the first pass at this design; removing them lets shard ops
// run independently on different cache lines.
//
// Tradeoff: writer mode now costs SCHED_SHARD_COUNT CAS ops (~32 cache lines)
// instead of one. Acceptable since writers are rare in the hot path (scheduler
// ticks, block paths) — most lock traffic is shard mode (wake helpers).
namespace {

constexpr uint32_t SCHED_SHARD_COUNT = 32;

class FutexSpinLock
{
public:
	void lock() noexcept
	{
		uint32_t expected = 0;
		if (m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
			return;
		lockSlow();
	}

	bool tryLock() noexcept
	{
		uint32_t expected = 0;
		return m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
	}

	void unlock() noexcept
	{
		if (m_state.exchange(0, std::memory_order_release) == 2)
			syscall(SYS_futex, &m_state, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
	}

private:
	void lockSlow() noexcept
	{
		for (int i = 0; i < 64; ++i)
		{
			uint32_t expected = 0;
			if (m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
				return;
#if defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
			__asm__ __volatile__("pause" ::: "memory");
#endif
		}
		uint32_t state = m_state.load(std::memory_order_relaxed);
		if (state != 2)
			state = m_state.exchange(2, std::memory_order_acquire);
		while (state != 0)
		{
			syscall(SYS_futex, &m_state, FUTEX_WAIT_PRIVATE, 2, nullptr, nullptr, 0);
			state = m_state.exchange(2, std::memory_order_acquire);
		}
	}

	std::atomic<uint32_t> m_state{0};
};

class ShardedSchedulerLock
{
public:
	// Writer: take all shards in order. Deadlock-free against shard mode
	// (shard mode only ever holds one shard, never tries to acquire another).
	// Writers serialize because they always acquire shard 0 first.
	void lockExclusive() noexcept
	{
		for (uint32_t i = 0; i < SCHED_SHARD_COUNT; ++i)
			m_shardLocks[i].sl.lock();
	}

	bool tryLockExclusive() noexcept
	{
		for (uint32_t i = 0; i < SCHED_SHARD_COUNT; ++i)
		{
			if (!m_shardLocks[i].sl.tryLock())
			{
				for (uint32_t j = i; j-- > 0;)
					m_shardLocks[j].sl.unlock();
				return false;
			}
		}
		return true;
	}

	void unlockExclusive() noexcept
	{
		for (uint32_t i = SCHED_SHARD_COUNT; i-- > 0;)
			m_shardLocks[i].sl.unlock();
	}

	void lockShard(uint32_t shardIdx) noexcept
	{
		m_shardLocks[shardIdx].sl.lock();
	}

	void unlockShard(uint32_t shardIdx) noexcept
	{
		m_shardLocks[shardIdx].sl.unlock();
	}

	static uint32_t hashShard(const void* obj) noexcept
	{
		uintptr_t h = reinterpret_cast<uintptr_t>(obj);
		h ^= (h >> 33);
		h *= 0xff51afd7ed558ccdULL;
		h ^= (h >> 33);
		return static_cast<uint32_t>(h) & (SCHED_SHARD_COUNT - 1);
	}

private:
	// Each shard is on its own cache line so unrelated shard ops don't cause
	// false sharing between cores.
	struct alignas(64) PaddedShard
	{
		FutexSpinLock sl;
		char _pad[64 - sizeof(FutexSpinLock)];
	};
	PaddedShard m_shardLocks[SCHED_SHARD_COUNT];
};

}

static ShardedSchedulerLock s_ptmSchedulerLock;
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SCHED_LOCK_NOINLINE __attribute__((noinline))
#else
#define SCHED_LOCK_NOINLINE
#endif

SCHED_LOCK_NOINLINE void __OSLockScheduler(void* obj)
{
#if BOOST_OS_WINDOWS
	EnterCriticalSection(&s_csSchedulerLock);
#else
	s_ptmSchedulerLock.lockExclusive();
#endif
#ifdef CEMU_DEBUG_ASSERT
	s_schedulerLockCount++;
	cemu_assert_debug(s_schedulerLockCount <= 1); // >= 2 should not happen. Scheduler lock does not allow recursion
#endif
}

bool __OSHasSchedulerLock()
{
#ifdef CEMU_DEBUG_ASSERT
	return s_schedulerLockCount > 0;
#else
	// Only consulted from cemu_assert_debug call sites; those compile to no-ops in
	// release. Return value is meaningless here.
	return true;
#endif
}

SCHED_LOCK_NOINLINE bool __OSTryLockScheduler(void* obj)
{
	bool r;
#if BOOST_OS_WINDOWS
	r = TryEnterCriticalSection(&s_csSchedulerLock);
#else
	r = s_ptmSchedulerLock.tryLockExclusive();
#endif
#ifdef CEMU_DEBUG_ASSERT
	if (r)
		s_schedulerLockCount++;
#endif
	return r;
}

SCHED_LOCK_NOINLINE void __OSUnlockScheduler(void* obj)
{
#ifdef CEMU_DEBUG_ASSERT
	s_schedulerLockCount--;
	cemu_assert_debug(s_schedulerLockCount >= 0);
#endif
#if BOOST_OS_WINDOWS
	LeaveCriticalSection(&s_csSchedulerLock);
#else
	s_ptmSchedulerLock.unlockExclusive();
#endif
}

SCHED_LOCK_NOINLINE void __OSLockSchedulerShard(void* obj)
{
#if BOOST_OS_WINDOWS
	// Windows path keeps the global critical section.
	EnterCriticalSection(&s_csSchedulerLock);
#else
	cemu_assert_debug(obj != nullptr);
	s_ptmSchedulerLock.lockShard(ShardedSchedulerLock::hashShard(obj));
#endif
#ifdef CEMU_DEBUG_ASSERT
	s_schedulerLockCount++;
	cemu_assert_debug(s_schedulerLockCount <= 1);
#endif
}

SCHED_LOCK_NOINLINE void __OSUnlockSchedulerShard(void* obj)
{
#ifdef CEMU_DEBUG_ASSERT
	s_schedulerLockCount--;
	cemu_assert_debug(s_schedulerLockCount >= 0);
#endif
#if BOOST_OS_WINDOWS
	LeaveCriticalSection(&s_csSchedulerLock);
#else
	cemu_assert_debug(obj != nullptr);
	s_ptmSchedulerLock.unlockShard(ShardedSchedulerLock::hashShard(obj));
#endif
}

namespace coreinit
{
	uint32 OSIsInterruptEnabled()
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		if (hCPU == nullptr)
			return 0;

		return hCPU->coreInterruptMask;
	}

	// disables interrupts and scheduling
	uint32 OSDisableInterrupts()
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		if (hCPU == nullptr)
			return 0;
		uint32 prevInterruptMask = hCPU->coreInterruptMask;
		if (hCPU->coreInterruptMask != 0)
		{
			// we have no efficient method to turn off scheduling completely, so instead we just increase the remaining cycles
			if (hCPU->remainingCycles >= 0x40000000)
				cemuLog_log(LogType::Force, "OSDisableInterrupts(): Warning - Interrupts already disabled but the mask was still set? remCycles {:08x} LR {:08x}", hCPU->remainingCycles, hCPU->spr.LR);
			hCPU->remainingCycles += 0x40000000;
		}
		hCPU->coreInterruptMask = 0;
		return prevInterruptMask;
	}

	uint32 OSRestoreInterrupts(uint32 interruptMask)
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		if (hCPU == nullptr)
			return 0;
		uint32 prevInterruptMask = hCPU->coreInterruptMask;
		if (hCPU->coreInterruptMask == 0 && interruptMask != 0)
		{
			hCPU->remainingCycles -= 0x40000000;
		}
		hCPU->coreInterruptMask = interruptMask;
		return prevInterruptMask;
	}

	uint32 OSEnableInterrupts()
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		uint32 prevInterruptMask = hCPU->coreInterruptMask;
		OSRestoreInterrupts(1);
		return prevInterruptMask;
	}

	void InitializeSchedulerLock()
	{
#if BOOST_OS_WINDOWS
		InitializeCriticalSection(&s_csSchedulerLock);
#endif
		// s_ptmSchedulerLock is statically zero-initialized on Linux; no runtime init needed.
		cafeExportRegister("coreinit", __OSLockScheduler, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSUnlockScheduler, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDisableInterrupts, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSEnableInterrupts, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSRestoreInterrupts, LogType::CoreinitThread);
	}
};
