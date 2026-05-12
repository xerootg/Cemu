#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"

#include <atomic>
#include <thread>

namespace coreinit
{
	void UpdateSystemMessageQueue();
	void HandleReceivedSystemMessage(OSMessage* msg);

	SysAllocator<OSMessageQueue> g_systemMessageQueue;
	SysAllocator<OSMessage, 16> _systemMessageQueueArray;

	// Per-queue spinlock pool. Hashed by queue address so cross-queue ops run in parallel.
	// Replaces taking the global scheduler lock for the message queue body — the scheduler
	// lock is now only acquired when we actually need to manipulate thread state
	// (block on full/empty or wake a waiter). Pre-fix, WW HD's tight Send/Receive loop
	// serialized every call through the global lock even when no thread state was changing.
	//
	// Lost-wakeup protection: the wait queue head pointers (threadQueueSend.head,
	// threadQueueReceive.head) are written by the scheduler-lock side and only become
	// visible to the queue-lock side after some indirect synchronization. To close the
	// race window where a fast-path peer can miss a slow-path waiter that is mid-enqueue
	// (released queue lock, has not yet written head), each pool slot maintains atomic
	// "pending waiter" counters that the slow path bumps under the queue lock before
	// releasing it. Counters are shared across queues that hash to the same slot — that
	// only ever causes a spurious scheduler-lock acquisition (which then no-ops via
	// isEmpty()), never a lost wakeup or correctness bug.
	//
	// QueueLockSlot's struct definition lives in the header — the AArch64 JIT emits an
	// inlined body for OSSend/Receive that loads the pool by address and indexes slots
	// directly. The 16-byte size lets the JIT use `add x_slot, x_pool, x_idx, lsl #4`.
	alignas(16) QueueLockSlot g_queueLockPool[QUEUE_LOCK_POOL_SIZE];

	namespace {
	// Lock acquisition uses fetch_or(LOCKED_BIT) so the returned prior value also
	// snapshots the upper-bit waiter counters in one atomic round-trip — matches
	// the JIT fast path's LDSETA pattern. Unlock uses fetch_and(~LOCKED_BIT) to
	// preserve waiter bits across the release.
	class QueueSpinLockGuard
	{
	public:
		explicit QueueSpinLockGuard(QueueLockSlot& slot) noexcept
			: m_slot(slot)
		{
			lockSlot(slot);
		}
		~QueueSpinLockGuard() noexcept
		{
			if (m_held)
				m_slot.packed.fetch_and(~QueueLockSlot::LOCKED_BIT, std::memory_order_release);
		}
		QueueSpinLockGuard(const QueueSpinLockGuard&) = delete;
		QueueSpinLockGuard& operator=(const QueueSpinLockGuard&) = delete;

		void unlock() noexcept
		{
			cemu_assert_debug(m_held);
			m_slot.packed.fetch_and(~QueueLockSlot::LOCKED_BIT, std::memory_order_release);
			m_held = false;
		}
		void relock() noexcept
		{
			cemu_assert_debug(!m_held);
			lockSlot(m_slot);
		}
		QueueLockSlot& slot() noexcept { return m_slot; }
	private:
		void lockSlot(QueueLockSlot& slot) noexcept
		{
			uint32_t prior = slot.packed.fetch_or(QueueLockSlot::LOCKED_BIT, std::memory_order_acquire);
			if (!(prior & QueueLockSlot::LOCKED_BIT))
			{
				m_held = true;
				return;
			}
			lockSlow(slot);
			m_held = true;
		}
		static void lockSlow(QueueLockSlot& slot) noexcept
		{
			for (;;)
			{
				for (int i = 0; i < 128; ++i)
				{
					// Peek-then-OR avoids needlessly writing the same bit when
					// already locked (LDSET is RMW even on a no-op clear) so the
					// cache line stays in shared state for the holder.
					uint32_t snap = slot.packed.load(std::memory_order_relaxed);
					if (!(snap & QueueLockSlot::LOCKED_BIT))
					{
						uint32_t prior = slot.packed.fetch_or(QueueLockSlot::LOCKED_BIT, std::memory_order_acquire);
						if (!(prior & QueueLockSlot::LOCKED_BIT))
							return;
					}
#if defined(__aarch64__) || defined(__arm__)
					__asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
					__asm__ __volatile__("pause" ::: "memory");
#endif
				}
				std::this_thread::yield();
			}
		}
		QueueLockSlot& m_slot;
		bool m_held{false};
	};

	inline QueueLockSlot& getQueueLockSlot(const void* p)
	{
		auto h = reinterpret_cast<uintptr_t>(p);
		h = (h >> 4) * 0x9E3779B97F4A7C15ULL;
		return g_queueLockPool[(h >> 56) & (QUEUE_LOCK_POOL_SIZE - 1)];
	}
	} // namespace

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData)
	{
		msgQueue->magic = 'mSgQ';
		msgQueue->userData = userData;
		msgQueue->msgArray = msgArray;
		msgQueue->msgCount = msgCount;
		msgQueue->firstIndex = 0;
		msgQueue->usedCount = 0;
		msgQueue->ukn08 = 0;
		OSInitThreadQueueEx(&msgQueue->threadQueueReceive, msgQueue);
		OSInitThreadQueueEx(&msgQueue->threadQueueSend, msgQueue);
	}

	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount)
	{
		OSInitMessageQueueEx(msgQueue, msgArray, msgCount, nullptr);
	}

	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		bool isSystemMessageQueue = (msgQueue == g_systemMessageQueue);
		if(isSystemMessageQueue)
			UpdateSystemMessageQueue();

		QueueSpinLockGuard qlock(getQueueLockSlot(msgQueue));
		while (msgQueue->usedCount == (uint32be)0)
		{
			if (!(flags & OS_MESSAGE_BLOCK))
				return false;
			// Publish our intent to wait before releasing the queue lock so any
			// concurrent sender's post-enqueue waiter check sees us. Counter is
			// in the upper bits of the packed slot word — fetch_add at the unit
			// increment moves only that counter, leaving the lock bit alone.
			qlock.slot().packed.fetch_add(QueueLockSlot::RECV_WAITER_INC, std::memory_order_seq_cst);
			qlock.unlock();
			__OSLockScheduler(msgQueue);
			if (msgQueue->usedCount != (uint32be)0)
			{
				// Raced with a sender. Bail out of the wait setup; the next loop
				// iteration under qlock will see the message and dequeue it.
				qlock.slot().packed.fetch_sub(QueueLockSlot::RECV_WAITER_INC, std::memory_order_seq_cst);
				__OSUnlockScheduler(msgQueue);
				qlock.relock();
				continue;
			}
			msgQueue->threadQueueReceive.queueAndWait(OSGetCurrentThread());
			qlock.slot().packed.fetch_sub(QueueLockSlot::RECV_WAITER_INC, std::memory_order_seq_cst);
			__OSUnlockScheduler(msgQueue);
			qlock.relock();
		}
		// Dequeue under queue lock
		sint32 messageIndex = msgQueue->firstIndex;
		OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
		memcpy(msg, readMsg, sizeof(OSMessage));
		msgQueue->firstIndex = ((uint32)msgQueue->firstIndex + 1) % (uint32)(msgQueue->msgCount);
		msgQueue->usedCount = (uint32)msgQueue->usedCount - 1;

		// Probe for waiters via the per-slot counter (cross-queue false positives are
		// harmless — see slot lookup comment). Counter is seq_cst, so any prior
		// queue-lock-held increment is visible here.
		bool maybeSendWaiters = (qlock.slot().packed.load(std::memory_order_seq_cst) & QueueLockSlot::SEND_WAITER_MASK) != 0;
		qlock.unlock();

		if (maybeSendWaiters)
		{
			__OSLockSchedulerShard(msgQueue);
			if (!msgQueue->threadQueueSend.isEmpty())
				msgQueue->threadQueueSend.wakeupSingleThreadWaitQueueShard();
			__OSUnlockSchedulerShard(msgQueue);
		}

		if(isSystemMessageQueue)
			HandleReceivedSystemMessage(msg);
		return true;
	}

	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg)
	{
		QueueSpinLockGuard qlock(getQueueLockSlot(msgQueue));
		if ((msgQueue->usedCount == (uint32be)0))
			return false;
		sint32 messageIndex = msgQueue->firstIndex;
		if (msg)
		{
			OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(msg, readMsg, sizeof(OSMessage));
		}
		return true;
	}

	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		QueueSpinLockGuard qlock(getQueueLockSlot(msgQueue));
		while (msgQueue->usedCount >= msgQueue->msgCount)
		{
			if (!(flags & OS_MESSAGE_BLOCK))
				return 0;
			qlock.slot().packed.fetch_add(QueueLockSlot::SEND_WAITER_INC, std::memory_order_seq_cst);
			qlock.unlock();
			__OSLockScheduler();
			if (msgQueue->usedCount < msgQueue->msgCount)
			{
				qlock.slot().packed.fetch_sub(QueueLockSlot::SEND_WAITER_INC, std::memory_order_seq_cst);
				__OSUnlockScheduler();
				qlock.relock();
				continue;
			}
			msgQueue->threadQueueSend.queueAndWait(OSGetCurrentThread());
			qlock.slot().packed.fetch_sub(QueueLockSlot::SEND_WAITER_INC, std::memory_order_seq_cst);
			__OSUnlockScheduler();
			qlock.relock();
		}
		// Enqueue under queue lock
		if ((flags & OS_MESSAGE_HIGH_PRIORITY))
		{
			sint32 newFirstIndex = (sint32)((sint32)msgQueue->firstIndex + (sint32)msgQueue->msgCount - 1) % (sint32)msgQueue->msgCount;
			msgQueue->firstIndex = newFirstIndex;
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[newFirstIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		else
		{
			sint32 messageIndex = (uint32)(msgQueue->firstIndex + msgQueue->usedCount) % (uint32)msgQueue->msgCount;
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}

		bool maybeReceiveWaiters = (qlock.slot().packed.load(std::memory_order_seq_cst) & QueueLockSlot::RECV_WAITER_MASK) != 0;
		qlock.unlock();

		if (maybeReceiveWaiters)
		{
			__OSLockSchedulerShard(msgQueue);
			if (!msgQueue->threadQueueReceive.isEmpty())
				msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueueShard();
			__OSUnlockSchedulerShard(msgQueue);
		}
		return 1;
	}

	OSMessageQueue* OSGetSystemMessageQueue()
	{
		return g_systemMessageQueue.GetPtr();
	}

	// The isEmptyAcquire-before-lock shortcut was tried and reverted: it races
	// with the slow path's pendingWaiters-bump-then-storeHeadRelease window. A
	// receiver that bumped pendingReceiveWaiters under qlock but hasn't yet
	// reached storeHeadRelease in queueAndWait (still in writer-lock acquire
	// path) is invisible to a lock-free head read, so the wake gets skipped
	// and the receiver sleeps with no waker. The JIT inline body gates on the
	// under-qlock pendingWaiters snapshot, which IS visible across qlock's
	// release-acquire; this helper is only ever called when that snapshot was
	// non-zero, so taking the shard lock is justified.
	void OSWakeOneSender(OSMessageQueue* msgQueue)
	{
		__OSLockSchedulerShard(msgQueue);
		if (!msgQueue->threadQueueSend.isEmpty())
			msgQueue->threadQueueSend.wakeupSingleThreadWaitQueueShard();
		__OSUnlockSchedulerShard(msgQueue);
	}

	void OSWakeOneReceiver(OSMessageQueue* msgQueue)
	{
		__OSLockSchedulerShard(msgQueue);
		if (!msgQueue->threadQueueReceive.isEmpty())
			msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueueShard();
		__OSUnlockSchedulerShard(msgQueue);
	}

	// HLE indices for the hot message-queue functions. Captured at registration time
	// so the AArch64 JIT can recognize PPCREC_IML_MACRO_HLE invocations of these
	// specific functions and emit an inlined fast path that bypasses
	// PPCRecompiler_virtualHLE + cafeExportCallWrapper. Profile showed
	// virtualHLE + children = 30.82% of core 1 on WW HD, of which 23.96% was
	// OSSend/Receive body — the wrapper overhead is the inlining target.
	sint32 g_hleIdx_OSSendMessage = -1;
	sint32 g_hleIdx_OSReceiveMessage = -1;

	// Captured host pointer to the system message queue. Set once during init so the
	// JIT-emitted inline body can compare msgQueue to it without touching SysAllocator.
	OSMessageQueue* g_systemMessageQueuePtr = nullptr;

	void InitializeMessageQueue()
	{
		OSInitMessageQueue(g_systemMessageQueue.GetPtr(), _systemMessageQueueArray.GetPtr(), _systemMessageQueueArray.GetCount());
		g_systemMessageQueuePtr = g_systemMessageQueue.GetPtr();

		cafeExportRegister("coreinit", OSInitMessageQueueEx, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitMessageQueue, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSReceiveMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSPeekMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSendMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMessageQueue, LogType::CoreinitThread);

		// Capture HLE indices for the JIT fast-path recognizer.
		g_hleIdx_OSSendMessage = osLib_getFunctionIndex("coreinit", "OSSendMessage");
		g_hleIdx_OSReceiveMessage = osLib_getFunctionIndex("coreinit", "OSReceiveMessage");
	}
};

