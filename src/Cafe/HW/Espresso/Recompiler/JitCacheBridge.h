#pragma once

// Cemu-side glue for the persistent JIT translation cache.
//
// Owns a single jitcache::Cache instance bound to the foreground title's
// cache directory. The AArch64 backend records relocs through this header
// during codegen; on each successful PPCRecompiler_generateAArch64Code we
// insert the host bytes + reloc list and periodically flush to disk.
//
// Phase 2: write path only. There is no lookup yet -- lazy JIT still
// compiles every PPC function on every launch; the cache exists only to
// produce evidence on disk for later phase 3 work.

#include <cstddef>
#include <cstdint>

namespace JitCacheBridge
{

// Interned ids for the 8 singleton symbols the AArch64 backend bakes into
// JIT bodies (see phase-0 taxonomy in commit b2a5b789). Populated by
// initialize() and stable for the rest of the process lifetime.
// Zero means uninitialized.
extern uint64_t SYM_g_systemMessageQueuePtr;
extern uint64_t SYM_g_queueLockPool;
extern uint64_t SYM_currentCoreThread;
extern uint64_t SYM_OSWakeOneSender;
extern uint64_t SYM_OSWakeOneReceiver;
extern uint64_t SYM_OSSendMessage;
extern uint64_t SYM_OSReceiveMessage;
extern uint64_t SYM_PPCRecompiler_virtualHLE;

// Attach the cache to <UserDataPath>/cache/jit/<titleId>/ and load any
// previous on-disk state. Called from PPCRecompiler_init once the title
// id is known. Safe to call multiple times; subsequent calls re-attach
// to a new directory (e.g. after a title swap).
void initialize(uint64_t titleId);

// Flush pending writes and detach. Called from PPCRecompiler_Shutdown.
// Safe to call without a prior initialize() -- no-op in that case.
void shutdown();

// Per-function lifecycle. Called by the AArch64 backend.
// beginFunction resets the per-function reloc buffer; endFunction
// inserts an EmittedCode entry into the cache and may trigger a flush.
//
// All three are no-ops if initialize() has not been called.
void beginFunction(uint32_t ppcAddr, uint32_t ppcSize);
void recordRuntimeSymbolReloc(uint32_t codeOffset, uint64_t symbolId);
void recordEmbeddedValueReloc(uint32_t codeOffset, uint64_t value);
void endFunction(const uint8_t* hostBytes, size_t hostSize);

} // namespace JitCacheBridge
