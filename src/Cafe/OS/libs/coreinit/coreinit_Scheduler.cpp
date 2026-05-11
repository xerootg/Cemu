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

// Adaptive futex-based spinlock used as the global scheduler lock. Replaces a
// recursive pthread_mutex_t — under WW HD core 1's tight OSSendMessage/OSReceiveMessage
// loop, the pthread path took ~30% of CPU on lock/unlock futex syscalls. The
// PTHREAD_MUTEX_RECURSIVE attribute was defensive only; existing
// s_schedulerLockCount <= 1 asserts confirm callers never recurse.
//
// State: 0=unlocked, 1=locked-no-waiters, 2=locked-with-waiters.
namespace {
class SchedulerSpinLock
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
}

static SchedulerSpinLock s_ptmSchedulerLock;
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
	s_ptmSchedulerLock.lock();
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
	r = s_ptmSchedulerLock.tryLock();
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
	s_ptmSchedulerLock.unlock();
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
