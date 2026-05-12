#pragma once

// Writer-mode scheduler lock. Excludes all other writer and shard holders.
// Use for: fiber switching (PPCCore_switchToScheduler*), thread create/destroy,
// suspend/resume, and any code path that may block the current thread or modify
// global scheduler state.
void __OSLockScheduler(void* obj = nullptr);
bool __OSHasSchedulerLock();
bool __OSTryLockScheduler(void* obj = nullptr);
void __OSUnlockScheduler(void* obj = nullptr);

// Shard-mode scheduler lock. Excludes only writer holders and other shard
// holders that hash to the same shard slot. obj must be non-null and stable
// (used as the hash key — typically the owning sync primitive pointer).
//
// Safe for: wake helpers that don't trigger same-core reschedule, atomic
// state queries on a single primitive. NOT safe for: any path that calls
// PPCCore_switchToSchedulerWithLock, modifies global structures, or holds the
// lock across a fiber switch.
//
// Reschedule-skipping wake variants (wakeupSingleThreadWaitQueueShard /
// wakeupEntireWaitQueueShard) are provided for use under shard mode.
void __OSLockSchedulerShard(void* obj);
void __OSUnlockSchedulerShard(void* obj);

namespace coreinit
{
	uint32 OSIsInterruptEnabled();
	uint32 OSDisableInterrupts();
	uint32 OSRestoreInterrupts(uint32 interruptMask);
	uint32 OSEnableInterrupts();

	void InitializeSchedulerLock();
}
