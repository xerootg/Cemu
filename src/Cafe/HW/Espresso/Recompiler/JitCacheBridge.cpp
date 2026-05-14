#include "JitCacheBridge.h"

#include <atomic>
#include <mutex>

#include "Common/JitCache/JitCache.h"
#include "Common/precompiled.h"
#include "Cemu/Logging/CemuLogging.h"
#include "HW/MMU/MMU.h"
#include "config/ActiveSettings.h"

namespace JitCacheBridge
{

// Bump on any change to AArch64 codegen that alters the emitted bytes for
// a given PPC body. Bumping forces every fingerprint to differ and
// invalidates the on-disk cache across builds.
//
// History:
//   1 = phase 2 initial wiring (b2a5b789 taxonomy + d79270a1 disk format)
constexpr uint32_t kCodegenVersion = 1;

uint64_t SYM_g_systemMessageQueuePtr = 0;
uint64_t SYM_g_queueLockPool = 0;
uint64_t SYM_currentCoreThread = 0;
uint64_t SYM_OSWakeOneSender = 0;
uint64_t SYM_OSWakeOneReceiver = 0;
uint64_t SYM_OSSendMessage = 0;
uint64_t SYM_OSReceiveMessage = 0;
uint64_t SYM_PPCRecompiler_virtualHLE = 0;

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

// Per-function buffer. The codegen flow calls beginFunction, then any
// number of recordRuntimeSymbolReloc / recordEmbeddedValueReloc, then
// endFunction. Only one function is in flight at a time on the worker
// thread; this is single-buffered intentionally.
uint32_t g_currentPpcAddr = 0;
uint32_t g_currentPpcSize = 0;
bool g_inFunction = false;
std::vector<jitcache::Reloc> g_currentRelocs;

// Flush bookkeeping. Flushing every insert would fsync per-function;
// batch instead and rely on shutdown() for the final write.
constexpr uint64_t kFlushEveryInserts = 256;
uint64_t g_insertsSinceFlush = 0;
std::atomic<uint64_t> g_totalInserts{0};
std::atomic<uint64_t> g_totalRelocs{0};

} // namespace

void initialize(uint64_t titleId)
{
	std::lock_guard<std::mutex> lk(g_mutex);

	g_moduleId = titleId;
	const fs::path cacheDir = ActiveSettings::GetUserDataPath(
	    fmt::format("cache/jit/{:016x}", titleId));
	std::error_code ec;
	fs::create_directories(cacheDir, ec);
	g_cache.load(cacheDir);

	// Intern the eight singleton symbols phase 0 confirmed as the entire
	// closed set. Symbol id 0 is reserved by the Cache.
	SYM_g_systemMessageQueuePtr     = g_cache.internSymbol("coreinit::g_systemMessageQueuePtr");
	SYM_g_queueLockPool             = g_cache.internSymbol("coreinit::g_queueLockPool");
	SYM_currentCoreThread           = g_cache.internSymbol("coreinit::__currentCoreThread");
	SYM_OSWakeOneSender             = g_cache.internSymbol("coreinit::OSWakeOneSender");
	SYM_OSWakeOneReceiver           = g_cache.internSymbol("coreinit::OSWakeOneReceiver");
	SYM_OSSendMessage               = g_cache.internSymbol("coreinit::OSSendMessage");
	SYM_OSReceiveMessage            = g_cache.internSymbol("coreinit::OSReceiveMessage");
	SYM_PPCRecompiler_virtualHLE    = g_cache.internSymbol("PPCRecompiler_virtualHLE");

	g_initialized = true;
	g_inFunction = false;
	g_currentRelocs.clear();
	g_insertsSinceFlush = 0;

	cemuLog_log(LogType::Force,
	            "JitCache: attached to {}, {} entries loaded",
	            _pathToUtf8(cacheDir),
	            g_cache.entryCount());
}

void shutdown()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (!g_initialized)
		return;
	const bool ok = g_cache.flush();
	cemuLog_log(LogType::Force,
	            "JitCache: shutdown -- {} entries, {} inserts this session, {} relocs (flush {})",
	            g_cache.entryCount(),
	            g_totalInserts.load(std::memory_order_relaxed),
	            g_totalRelocs.load(std::memory_order_relaxed),
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

	if (++g_insertsSinceFlush >= kFlushEveryInserts)
	{
		g_insertsSinceFlush = 0;
		// Flush failure here is non-fatal; we just keep accumulating
		// and try again at the next batch boundary or at shutdown.
		(void)g_cache.flush();
	}
}

} // namespace JitCacheBridge
