#include "JitCacheBridge.h"

#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "Common/JitCache/JitCache.h"
#include "Common/precompiled.h"
#include "Cemu/Logging/CemuLogging.h"
#include "HW/MMU/MMU.h"
#include "HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "HW/Espresso/Recompiler/PPCRecompiler.h"
#include "HW/Espresso/Recompiler/JitCacheSeed.h"
#include "HW/Espresso/Recompiler/BackendAArch64/BackendAArch64.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "config/ActiveSettings.h"

// Forward decls for the call_imm helper functions whose host address the
// IML bakes into the JIT body. fres_espresso / frsqrte_espresso are
// declared in PPCInterpreterInternal.h (already included above) but
// GetTBL/GetTBU only exist in PPCRecompilerImlGen.cpp's anonymous scope.
extern "C++" {
	ATTR_MS_ABI uint32 PPCRecompiler_GetTBL();
	ATTR_MS_ABI uint32 PPCRecompiler_GetTBU();
}

namespace JitCacheBridge
{

// Bump on any change to AArch64 codegen that alters the emitted bytes for
// a given PPC body. Bumping forces every fingerprint to differ and
// invalidates the on-disk cache across builds.
//
// History:
//   1 = phase 2 initial wiring (b2a5b789 taxonomy + d79270a1 disk format)
//   2 = phase 3a entry-point capture; same bytes but the cached entry-point
//       table is required for correct dispatch on hit. Old v1 entries do
//       not have it and must miss.
//   3 = phase 3b fixed-length movz+3*movk for reloc-target absolutes.
//       Was variable-length via xbyak's mov(XReg, uint64); the reloc
//       patcher would clobber 4 bytes past the actual emission whenever
//       the value's top chunk was zero (~925 of 14k functions on WW HD,
//       caught by JITCACHE_VERIFY). New emission is always 16 bytes;
//       size cost ~12 bytes per affected site, ~10KB session-wide.
//   4 = call_imm helper host pointers (fres_espresso, frsqrte_espresso,
//       PPCRecompiler_GetTBL, PPCRecompiler_GetTBU) now route through
//       RuntimeSymbol relocs instead of EmbeddedValue. The pre-fix bytes
//       baked the run's ASLR-randomized addresses literally; cache
//       reads in subsequent runs returned the stale addresses and the
//       JIT body called into random host memory. Fixed the 14 verify
//       mismatches in WW HD per session.
//   5 = invalidation bump. Phase A's discovery experiment (commit
//       8b62cfd9, later rolled back in faffaa87) queued prologue +
//       longcall candidates that hit false positives in CodeWarrior's
//       intermixed-data-in-text -- the boundary tracker walked the data
//       as code and the JIT cached AArch64 bytes for what looked like a
//       function but wasn't. Those garbage entries survived the rollback
//       because the codegen ITSELF didn't change (so kCodegenVersion=4
//       fingerprints still matched). On run 2 the cache hit returned the
//       garbage, the runtime installed and executed it -> SIGILL. Bumping
//       here forces every entry from a phase-A-polluted session to
//       fingerprint-miss and re-JIT against the corrected (bl-only)
//       discovery set.
constexpr uint32_t kCodegenVersion = 5;

uint64_t SYM_g_systemMessageQueuePtr = 0;
uint64_t SYM_g_queueLockPool = 0;
uint64_t SYM_currentCoreThread = 0;
uint64_t SYM_OSWakeOneSender = 0;
uint64_t SYM_OSWakeOneReceiver = 0;
uint64_t SYM_OSSendMessage = 0;
uint64_t SYM_OSReceiveMessage = 0;
uint64_t SYM_PPCRecompiler_virtualHLE = 0;
uint64_t SYM_fres_espresso = 0;
uint64_t SYM_frsqrte_espresso = 0;
uint64_t SYM_PPCRecompiler_GetTBL = 0;
uint64_t SYM_PPCRecompiler_GetTBU = 0;

namespace
{

// Single mutex guards all state below. The recompiler currently has one
// compile worker thread, but PPCRecompiler_recompileAtAddress can also be
// invoked from the main thread on the recompile-if-unvisited fast path,
// so any state touched from codegen must be serialized.
std::mutex g_mutex;
jitcache::Cache g_cache;
bool g_initialized = false;
uint64_t g_moduleId = 0;
bool g_readEnabled = false;
bool g_verifyEnabled = false;
fs::path g_cacheDir;

// Maps interned symbol id -> host pointer the resolver returns on lookup.
// Populated by initialize() in lock-step with the cache.internSymbol calls
// that produce the ids. Indexed by symbol id (id 0 is the empty-string
// sentinel and never resolves).
std::array<uint64_t, 16> g_symbolHostAddrs{};

// Counters surfaced at shutdown.
std::atomic<uint64_t> g_totalHits{0};
std::atomic<uint64_t> g_totalMisses{0};
std::atomic<uint64_t> g_verifyMismatches{0};
std::atomic<uint64_t> g_verifyChecks{0};

// Verify mode pays the full IML+codegen cost on every cache hit. Cap the
// total checks per session so framerate isn't pinned by diagnostics --
// 256 is enough to catch a handful of mismatches at typical rates (~14 in
// 14k hits == ~0.1%), and the dumps go to disk for offline analysis.
constexpr uint64_t kMaxVerifyChecks = 256;

class HostSymbolResolver : public jitcache::Resolver
{
public:
	uint64_t resolveRuntimeSymbol(uint64_t symbolId) const override
	{
		if (symbolId == 0 || symbolId >= g_symbolHostAddrs.size())
			return 0;
		return g_symbolHostAddrs[symbolId];
	}
	uint64_t resolvePpcCodeAddr(uint64_t /*ppcAddress*/) const override
	{
		// Phase 3 doesn't emit PpcCodeAddr-kind relocs anywhere; reserved
		// for the future direct-branch reloc kind.
		return 0;
	}
};

const HostSymbolResolver g_resolver;

bool envFlagTrue(const char* name)
{
	const char* v = std::getenv(name);
	if (!v)
		return false;
	return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

// Per-function buffer. The codegen flow calls beginFunction, then any
// number of recordRuntimeSymbolReloc / recordEmbeddedValueReloc, then
// endFunction. Only one function is in flight at a time on the worker
// thread; this is single-buffered intentionally.
uint32_t g_currentPpcAddr = 0;
uint32_t g_currentPpcSize = 0;
bool g_inFunction = false;
std::vector<jitcache::Reloc> g_currentRelocs;

// Cache flush is a full-manifest rewrite: it serializes every resident
// entry (the new ones AND the ones already on disk from prior sessions).
// On a precompile-warm cache with 30k+ entries, each flush is ~30 MB of
// I/O + fsync. Flushing every N inserts during a precompile drain
// produced a GB-scale write storm that pinned the framerate at <10 fps.
//
// Until the on-disk format gains append-only writes (a phase 4 item),
// run-time periodic flushes are disabled entirely. The only flush
// happens at shutdown(), so we lose new entries from this session if
// the process crashes -- acceptable for a JIT cache (next launch just
// re-JITs the lost functions).
uint64_t g_insertsSinceFlush = 0; // kept for future use; never gates a flush today
std::atomic<uint64_t> g_totalInserts{0};
std::atomic<uint64_t> g_totalRelocs{0};

} // namespace

void initialize(uint64_t titleId)
{
	std::lock_guard<std::mutex> lk(g_mutex);

	g_moduleId = titleId;
	g_cacheDir = ActiveSettings::GetUserDataPath(
	    fmt::format("cache/jit/{:016x}", titleId));
	std::error_code ec;
	fs::create_directories(g_cacheDir, ec);

	// Phase C: if the user has no manifest.bin yet (fresh install or first
	// time playing this title) AND the APK shipped a seed for this title,
	// hydrate the cache dir from the seed before loading. Any failure
	// inside tryBootstrap leaves cacheDir empty -- the normal load() +
	// phase B precompile below picks up the slack.
	const fs::path manifestPath = g_cacheDir / "manifest.bin";
	if (!fs::exists(manifestPath))
	{
		const fs::path seedRoot = ActiveSettings::GetUserDataPath("cache_seed");
		if (JitCacheSeed::tryBootstrap(titleId, g_cacheDir, seedRoot,
		                               kCodegenVersion,
		                               jitcache::kCacheFormatVersion))
		{
			cemuLog_log(LogType::Force,
			            "JitCache: hydrated from shipped seed before first run");
		}
	}

	g_cache.load(g_cacheDir);

	// Intern the eight singleton symbols phase 0 confirmed as the entire
	// closed set. Symbol id 0 is reserved by the Cache. Capture the host
	// pointer for each symbol into g_symbolHostAddrs at the same index so
	// the resolver can map id -> address without a string lookup.
	auto intern = [&](const char* name, uint64_t hostAddr) {
		uint64_t id = g_cache.internSymbol(name);
		if (id < g_symbolHostAddrs.size())
			g_symbolHostAddrs[id] = hostAddr;
		return id;
	};
	SYM_g_systemMessageQueuePtr  = intern("coreinit::g_systemMessageQueuePtr",  reinterpret_cast<uint64_t>(&coreinit::g_systemMessageQueuePtr));
	SYM_g_queueLockPool          = intern("coreinit::g_queueLockPool",          reinterpret_cast<uint64_t>(&coreinit::g_queueLockPool[0]));
	SYM_currentCoreThread        = intern("coreinit::__currentCoreThread",      reinterpret_cast<uint64_t>(&coreinit::__currentCoreThread[0]));
	SYM_OSWakeOneSender          = intern("coreinit::OSWakeOneSender",          reinterpret_cast<uint64_t>(&coreinit::OSWakeOneSender));
	SYM_OSWakeOneReceiver        = intern("coreinit::OSWakeOneReceiver",        reinterpret_cast<uint64_t>(&coreinit::OSWakeOneReceiver));
	SYM_OSSendMessage            = intern("coreinit::OSSendMessage",            reinterpret_cast<uint64_t>(&coreinit::OSSendMessage));
	SYM_OSReceiveMessage         = intern("coreinit::OSReceiveMessage",         reinterpret_cast<uint64_t>(&coreinit::OSReceiveMessage));
	SYM_PPCRecompiler_virtualHLE = intern("PPCRecompiler_virtualHLE",           reinterpret_cast<uint64_t>(PPCRecompiler_getVirtualHLEHostAddr()));
	SYM_fres_espresso            = intern("fres_espresso",                     reinterpret_cast<uint64_t>(&fres_espresso));
	SYM_frsqrte_espresso         = intern("frsqrte_espresso",                  reinterpret_cast<uint64_t>(&frsqrte_espresso));
	SYM_PPCRecompiler_GetTBL     = intern("PPCRecompiler_GetTBL",              reinterpret_cast<uint64_t>(&PPCRecompiler_GetTBL));
	SYM_PPCRecompiler_GetTBU     = intern("PPCRecompiler_GetTBU",              reinterpret_cast<uint64_t>(&PPCRecompiler_GetTBU));

	g_initialized = true;
	g_inFunction = false;
	g_currentRelocs.clear();
	g_insertsSinceFlush = 0;

	// Cache read is ON by default starting at kCodegenVersion=4: the v4
	// bump invalidates every entry written before the call_imm helper-
	// symbol fix (fres/frsqrte/GetTBL/GetTBU now route through the
	// resolver, not as frozen embedded values), so any v4 entry we
	// observe was written with the corrected codegen and is safe to
	// execute. VERIFY stays off by default: it doubles the work per
	// cache hit, which crushes framerate without giving any safety
	// benefit on a known-good cache.
	//
	// JITCACHE_READ=0 disables the cache read path (each launch
	// re-JITs everything). JITCACHE_VERIFY=1 turns on the memcmp
	// diagnostic during development.
	const char* readEnv = std::getenv("JITCACHE_READ");
	g_readEnabled = (readEnv == nullptr) || (readEnv[0] != '0');
	const char* verifyEnv = std::getenv("JITCACHE_VERIFY");
	g_verifyEnabled = verifyEnv && verifyEnv[0] != '0';
	g_totalHits = 0;
	g_totalMisses = 0;
	g_verifyMismatches = 0;

	cemuLog_log(LogType::Force,
	            "JitCache: attached to {}, {} entries loaded (read={} verify={})",
	            _pathToUtf8(g_cacheDir),
	            g_cache.entryCount(),
	            g_readEnabled ? "on" : "off",
	            g_verifyEnabled ? "on" : "off");
}

void shutdown()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized)
		return;
	const bool ok = g_cache.flush();
	cemuLog_log(LogType::Force,
	            "JitCache: shutdown -- {} entries, {} inserts, {} relocs, {} hits, {} misses, {} verify-mismatches (flush {})",
	            g_cache.entryCount(),
	            g_totalInserts.load(std::memory_order_relaxed),
	            g_totalRelocs.load(std::memory_order_relaxed),
	            g_totalHits.load(std::memory_order_relaxed),
	            g_totalMisses.load(std::memory_order_relaxed),
	            g_verifyMismatches.load(std::memory_order_relaxed),
	            ok ? "ok" : "FAILED");
	g_initialized = false;
	g_inFunction = false;
	g_currentRelocs.clear();
}

void beginFunction(uint32_t ppcAddr, uint32_t ppcSize)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized)
		return;
	g_currentPpcAddr = ppcAddr;
	g_currentPpcSize = ppcSize;
	g_currentRelocs.clear();
	g_inFunction = true;
}

void recordRuntimeSymbolReloc(uint32_t codeOffset, uint64_t symbolId)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized || !g_inFunction)
		return;
	jitcache::Reloc r{};
	r.codeOffset = codeOffset;
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::RuntimeSymbol;
	r.targetId = symbolId;
	g_currentRelocs.push_back(r);
	g_totalRelocs.fetch_add(1, std::memory_order_relaxed);
}

void recordEmbeddedValueReloc(uint32_t codeOffset, uint64_t value)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized || !g_inFunction)
		return;
	jitcache::Reloc r{};
	r.codeOffset = codeOffset;
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::EmbeddedValue;
	r.targetId = value;
	g_currentRelocs.push_back(r);
	g_totalRelocs.fetch_add(1, std::memory_order_relaxed);
}

void abortFunction()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized || !g_inFunction)
		return;
	g_inFunction = false;
	g_currentRelocs.clear();
}

void endFunction(const uint8_t* hostBytes, size_t hostSize,
                 const EntryPoint* entryPoints, size_t entryPointCount)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized || !g_inFunction)
		return;
	g_inFunction = false;

	// Build the FunctionKey by hashing the PPC body bytes alongside the
	// other identity fields. memory_getPointerFromVirtualOffset returns
	// a host pointer into the guest address space; PPC code pages are
	// stable for the duration of a recompile.
	const uint8_t* ppcBytes = memory_getPointerFromVirtualOffset(g_currentPpcAddr);
	if (!ppcBytes || hostBytes == nullptr || hostSize == 0)
	{
		g_currentRelocs.clear();
		return;
	}

	jitcache::FunctionKey key{};
	key.codegenVersion = kCodegenVersion;
	key.hostCpuFeatureBits = 0; // AArch64 backend has no conditional emit
	key.moduleId = g_moduleId;
	key.ppcEntryAddr = g_currentPpcAddr;
	key.ppcLen = g_currentPpcSize;
	key.ppcBytes = ppcBytes;

	jitcache::EmittedCode emitted;
	emitted.hostBytes.assign(hostBytes, hostBytes + hostSize);
	emitted.relocs = std::move(g_currentRelocs);
	g_currentRelocs.clear();
	emitted.entryPoints.resize(entryPointCount);
	for (size_t i = 0; i < entryPointCount; ++i)
	{
		emitted.entryPoints[i].ppcAddr = entryPoints[i].ppcAddr;
		emitted.entryPoints[i].hostOffset = entryPoints[i].hostOffset;
	}

	g_cache.insert(key, emitted);
	g_totalInserts.fetch_add(1, std::memory_order_relaxed);

	// Periodic flush intentionally disabled (see g_insertsSinceFlush
	// comment): full-manifest rewrites during a precompile drain pin the
	// framerate. Only shutdown() flushes today.
	++g_insertsSinceFlush;
}

bool peekLookup(PPCRecFunction_t* ppcRecFunc,
                std::vector<uint8_t>& hostBytesOut,
                std::vector<EntryPoint>& entryPointsOut)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized || !g_readEnabled)
		return false;

	const uint8_t* ppcBytes = memory_getPointerFromVirtualOffset(ppcRecFunc->ppcAddress);
	if (!ppcBytes)
	{
		g_totalMisses.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	jitcache::FunctionKey key{};
	key.codegenVersion = kCodegenVersion;
	key.hostCpuFeatureBits = 0;
	key.moduleId = g_moduleId;
	key.ppcEntryAddr = ppcRecFunc->ppcAddress;
	key.ppcLen = ppcRecFunc->ppcSize;
	key.ppcBytes = ppcBytes;

	jitcache::EmittedCode loaded;
	if (!g_cache.lookup(key, g_resolver, loaded))
	{
		g_totalMisses.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	hostBytesOut = std::move(loaded.hostBytes);
	entryPointsOut.clear();
	entryPointsOut.reserve(loaded.entryPoints.size());
	for (const auto& ep : loaded.entryPoints)
	{
		EntryPoint b{};
		b.ppcAddr = ep.ppcAddr;
		b.hostOffset = ep.hostOffset;
		entryPointsOut.push_back(b);
	}

	g_totalHits.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool isReadEnabled()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_initialized && g_readEnabled;
}

bool isVerifyEnabled()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_initialized && g_verifyEnabled;
}

bool consumeVerifyBudget()
{
	if (!isVerifyEnabled())
		return false;
	const uint64_t prev = g_verifyChecks.fetch_add(1, std::memory_order_relaxed);
	return prev < kMaxVerifyChecks;
}

void recordVerifyMismatch(uint32_t ppcAddr,
                          const uint8_t* cachedBytes, size_t cachedSize,
                          const uint8_t* freshBytes, size_t freshSize)
{
	constexpr size_t kMaxVerifyDumps = 32;
	const uint64_t n = g_verifyMismatches.fetch_add(1, std::memory_order_relaxed);

	// Find the first byte where the two blobs disagree. If sizes differ
	// the firstDiff is the shorter length (everything past it is
	// trivially missing on one side).
	const size_t cmpLen = std::min(cachedSize, freshSize);
	size_t firstDiff = cmpLen;
	for (size_t i = 0; i < cmpLen; ++i)
	{
		if (cachedBytes[i] != freshBytes[i])
		{
			firstDiff = i;
			break;
		}
	}

	cemuLog_log(LogType::Force,
	            "JitCache: VERIFY mismatch at PPC 0x{:08x} -- cached {} B vs fresh {} B, first diff @ {} (0x{:x})",
	            ppcAddr, cachedSize, freshSize, firstDiff, firstDiff);

	// Cap on-disk dumps so a flood of mismatches can't fill storage.
	if (n >= kMaxVerifyDumps)
		return;

	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized || g_cacheDir.empty())
		return;
	const fs::path diffDir = g_cacheDir / "verify_diff";
	std::error_code ec;
	fs::create_directories(diffDir, ec);
	auto write = [&](const fs::path& p, const uint8_t* data, size_t size) {
		FILE* f = std::fopen(p.string().c_str(), "wb");
		if (!f) return;
		std::fwrite(data, 1, size, f);
		std::fclose(f);
	};
	const std::string base = fmt::format("{:08x}", ppcAddr);
	write(diffDir / (base + ".cached.bin"), cachedBytes, cachedSize);
	write(diffDir / (base + ".fresh.bin"),  freshBytes,  freshSize);
}

uint64_t symbolIdForHostAddr(uint64_t hostAddr)
{
	// Linear scan over the interned symbol table. Cost is ~12 compares
	// per call_imm site -- trivial vs the codegen path it sits in.
	if (hostAddr == 0)
		return 0;
	for (size_t i = 1; i < g_symbolHostAddrs.size(); ++i)
	{
		if (g_symbolHostAddrs[i] == hostAddr)
			return static_cast<uint64_t>(i);
	}
	return 0;
}

} // namespace JitCacheBridge
