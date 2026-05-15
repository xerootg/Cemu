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
#include <vector>

// Forward declaration at file scope. Without this, `struct PPCRecFunction_t*`
// inside the JitCacheBridge namespace below would forward-declare
// JitCacheBridge::PPCRecFunction_t instead of the global type defined in
// PPCRecompiler.h, and the peekLookup body could not access its members.
struct PPCRecFunction_t;

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
// Helper-function host pointers reached via IML's CALL_IMM. The
// recompiler bakes the address of these into the JIT body; because the
// addresses are process-specific (ASLR), they MUST be re-resolved on
// cache load. Phase 3b shipped them as EmbeddedValue and we paid for it
// with 14 verify mismatches per WW HD session. Now routed through
// RuntimeSymbol via symbolIdForHostAddr below.
extern uint64_t SYM_fres_espresso;
extern uint64_t SYM_frsqrte_espresso;
extern uint64_t SYM_PPCRecompiler_GetTBL;
extern uint64_t SYM_PPCRecompiler_GetTBU;

// Reverse-lookup: returns the interned symbol id whose host address
// equals `hostAddr`, or 0 if no symbol matches. Used by AArch64GenContext_t
// at call_imm codegen so the IML's raw uintptr_t callAddress can be
// emitted as a RuntimeSymbol reloc instead of a frozen EmbeddedValue.
uint64_t symbolIdForHostAddr(uint64_t hostAddr);

// Attach the cache to <UserDataPath>/cache/jit/<titleId>/ and load any
// previous on-disk state. Called from PPCRecompiler_init once the title
// id is known. Safe to call multiple times; subsequent calls re-attach
// to a new directory (e.g. after a title swap).
void initialize(uint64_t titleId);

// Flush pending writes and detach. Called from PPCRecompiler_Shutdown.
// Safe to call without a prior initialize() -- no-op in that case.
void shutdown();

// Per-function lifecycle. Called around AArch64 codegen.
// beginFunction resets the per-function reloc buffer; abortFunction
// discards it (codegen failed); endFunction commits an EmittedCode entry
// into the cache and may trigger a flush.
//
// The caller drives the order:
//   1. beginFunction(ppcAddr, ppcSize)
//   2. ... record*Reloc(...) per emitted absolute pointer ...
//   3a. (failure)   abortFunction()
//   3b. (success)   endFunction(hostBytes, hostSize, entryPoints,
//                               entryPointCount)
//
// endFunction has to come after the IML phase has collected entry points
// from the codegen result, so it is invoked from the PPCRecompiler driver
// rather than from inside generateAArch64Code itself.
//
// All four are no-ops if initialize() has not been called.
struct EntryPoint
{
	uint32_t ppcAddr;
	uint32_t hostOffset;
};

void beginFunction(uint32_t ppcAddr, uint32_t ppcSize);
void recordRuntimeSymbolReloc(uint32_t codeOffset, uint64_t symbolId);
void recordEmbeddedValueReloc(uint32_t codeOffset, uint64_t value);
void abortFunction();
void endFunction(const uint8_t* hostBytes, size_t hostSize,
                 const EntryPoint* entryPoints, size_t entryPointCount);

// Read path: returns true if a cache entry matches the PPC bytes for the
// function described by ppcRecFunc (ppcAddress/ppcSize must already be
// set by the caller). On success, fills hostBytesOut with the cached
// host code (relocs already applied by the resolver) and entryPointsOut
// with the cached entry-point table. Does NOT allocate executable
// memory; the caller is responsible for installing the bytes via
// PPCRecompiler_loadAArch64FromCache (normal mode) or running codegen
// and memcmp'ing the fresh output against the cached bytes (verify mode).
//
// Returns false on miss or any resolver failure (unresolved symbol, bad
// reloc, etc.). On false the caller falls through to the existing
// IML+codegen path. Hit / miss counters are incremented internally.
bool peekLookup(struct PPCRecFunction_t* ppcRecFunc,
                std::vector<uint8_t>& hostBytesOut,
                std::vector<EntryPoint>& entryPointsOut);

// JITCACHE_VERIFY=1: caller is expected to run full IML+codegen on every
// cache hit and memcmp the result against the cached bytes. On mismatch
// it calls recordVerifyMismatch -- this logs the PPC address and
// increments a counter surfaced at shutdown. The cache hit is NOT
// rejected on mismatch (we trust the cache); the log catches unbumped
// codegen changes during development.
bool isReadEnabled();
bool isVerifyEnabled();

// Verify pays a full IML+codegen pass per hit. To keep diagnostics from
// crushing the framerate, the bridge only verifies a bounded number of
// hits per session; the budget is exhausted by the first ~256 hits.
// Call this once per cache hit; returns true iff verify should run on
// this hit. Does nothing if verify is off.
bool consumeVerifyBudget();

// Called when a verify-mode cache hit's bytes disagree with fresh
// codegen. Logs the first-diff offset, dumps both blobs (capped to
// avoid filling disk) to <cacheDir>/verify_diff/<ppc>.{cached,fresh}.bin
// for offline analysis.
void recordVerifyMismatch(uint32_t ppcAddr,
                          const uint8_t* cachedBytes, size_t cachedSize,
                          const uint8_t* freshBytes, size_t freshSize);

} // namespace JitCacheBridge
