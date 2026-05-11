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
	struct alignas(16) QueueLockSlot
	{
		std::atomic<uint32_t> lockState{0};             // offset 0
		std::atomic<uint32_t> pendingReceiveWaiters{0}; // offset 4
		std::atomic<uint32_t> pendingSendWaiters{0};    // offset 8
		uint32_t _pad{0};                               // offset 12 — keeps slot size at 16 for shift-based indexing
	};
	static_assert(sizeof(QueueLockSlot) == 16);

	constexpr size_t QUEUE_LOCK_POOL_SIZE = 256;
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