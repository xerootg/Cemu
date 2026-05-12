#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

#include <atomic>

namespace coreinit
{
	enum class SysMessageId : uint32
	{
		MsgAcquireForeground = 0xFACEF000,
		MsgReleaseForeground = 0xFACEBACC,
		MsgExit = 0xD1E0D1E0,
		HomeButtonDenied = 0xCCC0FFEE,
		NetIoStartOrStop = 0xAAC0FFEE,
	};

	// Per-queue spinlock slot. Exposed in the header so the AArch64 JIT can emit an
	// inlined message-queue body that bypasses the C++ entrypoint entirely. Layout is
	// part of the JIT ABI — do not reorder fields without updating BackendAArch64.cpp.
	//
	// All state packed into a single 32-bit word so the JIT fast path observes both
	// the lock acquisition AND the opposite-side waiter snapshot from one atomic op
	// (LDSETA), removing the post-CAS dependent LDR that previously dominated cycle
	// attribution on the message-queue hot path:
	//   bit  0     : LOCKED_BIT
	//   bits 1..15 : pendingReceiveWaiters count (RECV_WAITER_MASK)
	//   bits 16..30: pendingSendWaiters count    (SEND_WAITER_MASK)
	//   bit  31    : reserved
	// Waiter counters are only ever mutated while the lock is held; the atomic
	// fetch_add/sub there is for cross-thread visibility, not concurrent RMW.
	struct alignas(16) QueueLockSlot
	{
		static constexpr uint32_t LOCKED_BIT       = 1u << 0;
		static constexpr uint32_t RECV_WAITER_INC  = 1u << 1;
		static constexpr uint32_t RECV_WAITER_MASK = 0xFFFEu;        // bits 1..15
		static constexpr uint32_t SEND_WAITER_INC  = 1u << 16;
		static constexpr uint32_t SEND_WAITER_MASK = 0x7FFF0000u;    // bits 16..30

		std::atomic<uint32_t> packed{0};                // offset 0
		uint32_t _pad[3]{};                             // pad to 16 bytes for shift-based slot indexing
	};
	static_assert(sizeof(QueueLockSlot) == 16);
	static_assert(offsetof(QueueLockSlot, packed) == 0);

	// Wind Waker HD has ~38 live OSMessageQueue instances; pool sized at 4096 to
	// give zero hash collisions in practice (measured: collision rate 0% at 4096
	// vs 5.3% at 2048, 10.5% at 1024, 15.8% at 256). Per-slot waiter counts are
	// shared across queues hashing to the same slot, so a collision turns every
	// no-op JIT wake on the colliding sender into a real shard-lock acquisition;
	// at 256 slots that was visible as ~5–6% of cycles in __OSLockSchedulerShard.
	//
	// Memory cost: 4096 * 16 = 64 KB BSS. Well within L2; only the ~38 actively
	// hashed slots are hot in L1.
	//
	// Index extraction is `(h >> QUEUE_LOCK_POOL_INDEX_SHIFT) & (POOL_SIZE - 1)`
	// — the AArch64 JIT body relies on this matching its `lsr` immediate.
	constexpr size_t QUEUE_LOCK_POOL_SIZE        = 4096;
	constexpr uint32_t QUEUE_LOCK_POOL_INDEX_SHIFT = 52;  // 64 - log2(4096)
	static_assert((QUEUE_LOCK_POOL_SIZE & (QUEUE_LOCK_POOL_SIZE - 1)) == 0, "pool size must be a power of two");
	static_assert((1ull << (64 - QUEUE_LOCK_POOL_INDEX_SHIFT)) == QUEUE_LOCK_POOL_SIZE, "shift must match pool size");

	extern QueueLockSlot g_queueLockPool[QUEUE_LOCK_POOL_SIZE];

	struct OSMessage
	{
		uint32be		message;
		uint32be		data0;
		uint32be		data1;
		uint32be		data2;
	};

	struct OSMessageQueue
	{
		/* +0x00 */ uint32be				magic;
		/* +0x04 */ MEMPTR<void>			userData;
		/* +0x08 */ uint32be				ukn08;
		/* +0x0C */ OSThreadQueue			threadQueueSend;
		/* +0x1C */ OSThreadQueue			threadQueueReceive;
		/* +0x2C */ MEMPTR<OSMessage>		msgArray;
		/* +0x30 */ uint32be				msgCount;
		/* +0x34 */ uint32be				firstIndex;
		/* +0x38 */ uint32be				usedCount;
	};

	static_assert(sizeof(OSMessageQueue) == 0x3C);

	// flags
	#define OS_MESSAGE_BLOCK				1 // blocking send/receive
	#define OS_MESSAGE_HIGH_PRIORITY		2 // put message in front of all queued messages

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData);
	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount);
	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags);
	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg);
	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags);

	OSMessageQueue* OSGetSystemMessageQueue();

	void InitializeMessageQueue();

	// HLE indices for the JIT fast-path recognizer. -1 until InitializeMessageQueue runs.
	extern sint32 g_hleIdx_OSSendMessage;
	extern sint32 g_hleIdx_OSReceiveMessage;

	// Host pointer to the system message queue. Used by the AArch64 JIT inlined body
	// to bail to the C++ slow path when the target is the system queue (which has
	// extra UpdateSystemMessageQueue / HandleReceivedSystemMessage hooks).
	extern OSMessageQueue* g_systemMessageQueuePtr;

	// Wake helpers used by the JIT-inlined fast path when it observes a non-zero
	// pending-waiter counter after enqueue/dequeue. Both take the scheduler lock,
	// no-op if the per-queue thread list is empty (false positives from slot
	// hash-collisions land here harmlessly).
	void OSWakeOneSender(OSMessageQueue* msgQueue);
	void OSWakeOneReceiver(OSMessageQueue* msgQueue);
};