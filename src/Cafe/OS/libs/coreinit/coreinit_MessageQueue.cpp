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
	namespace {
	struct QueueLockSlot
	{
		std::atomic<uint32_t> lockState{0};
		std::atomic<uint32_t> pendingReceiveWaiters{0};
		std::atomic<uint32_t> pendingSendWaiters{0};
	};

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
				m_slot.lockState.store(0, std::memory_order_release);
		}
		QueueSpinLockGuard(const QueueSpinLockGuard&) = delete;
		QueueSpinLockGuard& operator=(const QueueSpinLockGuard&) = delete;

		void unlock() noexcept
		{
			cemu_assert_debug(m_held);
			m_slot.lockState.store(0, std::memory_order_release);
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
			uint32_t expected = 0;
			if (slot.lockState.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
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
					uint32_t expected = 0;
					if (slot.lockState.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
						return;
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

	constexpr size_t QUEUE_LOCK_POOL_SIZE = 256;
	QueueLockSlot g_queueLockPool[QUEUE_LOCK_POOL_SIZE];

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
			// concurrent sender's post-enqueue waiter check sees us.
			qlock.slot().pendingReceiveWaiters.fetch_add(1, std::memory_order_seq_cst);
			qlock.unlock();
			__OSLockScheduler(msgQueue);
			if (msgQueue->usedCount != (uint32be)0)
			{
				// Raced with a sender. Bail out of the wait setup; the next loop
				// iteration under qlock will see the message and dequeue it.
				qlock.slot().pendingReceiveWaiters.fetch_sub(1, std::memory_order_seq_cst);
				__OSUnlockScheduler(msgQueue);
				qlock.relock();
				continue;
			}
			msgQueue->threadQueueReceive.queueAndWait(OSGetCurrentThread());
			qlock.slot().pendingReceiveWaiters.fetch_sub(1, std::memory_order_seq_cst);
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
		bool maybeSendWaiters = qlock.slot().pendingSendWaiters.load(std::memory_order_seq_cst) != 0;
		qlock.unlock();

		if (maybeSendWaiters)
		{
			__OSLockScheduler(msgQueue);
			if (!msgQueue->threadQueueSend.isEmpty())
				msgQueue->threadQueueSend.wakeupSingleThreadWaitQueue(true);
			__OSUnlockScheduler(msgQueue);
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
			qlock.slot().pendingSendWaiters.fetch_add(1, std::memory_order_seq_cst);
			qlock.unlock();
			__OSLockScheduler();
			if (msgQueue->usedCount < msgQueue->msgCount)
			{
				qlock.slot().pendingSendWaiters.fetch_sub(1, std::memory_order_seq_cst);
				__OSUnlockScheduler();
				qlock.relock();
				continue;
			}
			msgQueue->threadQueueSend.queueAndWait(OSGetCurrentThread());
			qlock.slot().pendingSendWaiters.fetch_sub(1, std::memory_order_seq_cst);
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

		bool maybeReceiveWaiters = qlock.slot().pendingReceiveWaiters.load(std::memory_order_seq_cst) != 0;
		qlock.unlock();

		if (maybeReceiveWaiters)
		{
			__OSLockScheduler();
			if (!msgQueue->threadQueueReceive.isEmpty())
				msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueue(true);
			__OSUnlockScheduler();
		}
		return 1;
	}

	OSMessageQueue* OSGetSystemMessageQueue()
	{
		return g_systemMessageQueue.GetPtr();
	}

	// HLE indices for the hot message-queue functions. Captured at registration time
	// so the AArch64 JIT can recognize PPCREC_IML_MACRO_HLE invocations of these
	// specific functions and emit an inlined fast path that bypasses
	// PPCRecompiler_virtualHLE + cafeExportCallWrapper. Profile showed
	// virtualHLE + children = 30.82% of core 1 on WW HD, of which 23.96% was
	// OSSend/Receive body — the wrapper overhead is the inlining target.
	sint32 g_hleIdx_OSSendMessage = -1;
	sint32 g_hleIdx_OSReceiveMessage = -1;

	void InitializeMessageQueue()
	{
		OSInitMessageQueue(g_systemMessageQueue.GetPtr(), _systemMessageQueueArray.GetPtr(), _systemMessageQueueArray.GetCount());

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

