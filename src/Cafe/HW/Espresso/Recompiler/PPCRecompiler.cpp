#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "JitCacheBridge.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/OS/common/OSCommon.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"

#include <atomic>
#include <mutex>
#ifdef __unix__
#include <unistd.h>
#endif
#include "Common/cpu_features.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"

#define PPCREC_FORCE_SYNCHRONOUS_COMPILATION	0 // if 1, then function recompilation will block and execute on the thread that called PPCRecompiler_visitAddressNoBlock
#define PPCREC_LOG_RECOMPILATION_RESULTS		0

struct PPCInvalidationRange
{
	MPTR startAddress;
	uint32 size;

	PPCInvalidationRange(MPTR _startAddress, uint32 _size) : startAddress(_startAddress), size(_size) {};
};

struct
{
	FSpinlock recompilerSpinlock;
	std::queue<MPTR> targetQueue;
	std::vector<PPCInvalidationRange> invalidationRanges;
}PPCRecompilerState;

RangeStore<PPCRecFunction_t*, uint32, 7703, 0x2000> rangeStore_ppcRanges;

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
static std::mutex s_singleRecompilationMutex;
#endif

bool ppcRecompilerEnabled = false;

void PPCRecompiler_recompileAtAddress(uint32 address);

// this function does never block and can fail if the recompiler lock cannot be acquired immediately
void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	PPCRecompilerState.recompilerSpinlock.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
	PPCRecompilerState.recompilerSpinlock.unlock();
	s_singleRecompilationMutex.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompiler_recompileAtAddress(enterAddress);
	}
	s_singleRecompilationMutex.unlock();
	return;
#endif
	// quick read-only check without lock
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	// try to acquire lock
	if (!PPCRecompilerState.recompilerSpinlock.try_lock())
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		// was visited since previous check
		PPCRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	// add to recompilation queue and flag as visited
	PPCRecompilerState.targetQueue.emplace(enterAddress);
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;

	PPCRecompilerState.recompilerSpinlock.unlock();
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (ppcRecompilerEnabled == false)
		return;
	PPCRecompiler_visitAddressNoBlock(enterAddress);
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
#if BOOST_OS_WINDOWS
	uint32 prevState = _controlfp(0, 0);
	_controlfp(_RC_NEAR, _MCW_RC);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	_controlfp(prevState, _MCW_RC);
	// debug recompiler exit - useful to find frequently executed functions which couldn't be recompiled
	#ifdef CEMU_DEBUG_ASSERT
	if (hCPU->remainingCycles > 0 && GetAsyncKeyState(VK_F4))
	{
		auto t = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
		cemuLog_log(LogType::Force, "Recompiler exit: 0x{:08x} LR: 0x{:08x} Timestamp {}.{:04}", hCPU->instructionPointer, hCPU->spr.LR, dur / 1000LL, (dur % 1000LL));
	}
	#endif
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif
	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (ppcRecompilerEnabled == false)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (ppcRecompilerEnabled == false)
		return;
	if (hCPU->remainingCycles <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompiler_visitAddressNoBlock(enterAddress);
	}
	else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		// enter
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}
bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);

PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker)
{
	if (range.startAddress >= PPC_REC_CODE_AREA_END)
	{
		cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
		return nullptr;
	}
	uint32 codeGenRangeStart;
	uint32 codeGenRangeSize = 0;
	coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
	if (codeGenRangeSize != 0)
	{
		if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
		{
			if (coreinit::codeGenShouldAvoid())
			{
				return nullptr;
			}
		}
	}

	PPCRecFunction_t* ppcRecFunc = new PPCRecFunction_t();
	ppcRecFunc->ppcAddress = range.startAddress;
	ppcRecFunc->ppcSize = range.length;

#if PPCREC_LOG_RECOMPILATION_RESULTS
	BenchmarkTimer bt;
	bt.Start();
#endif

#if defined(__aarch64__)
	// Phase 3b: ask the JIT cache before doing any work. On a hit with
	// verify=off, we install the cached host bytes, populate entry
	// points and list_ranges identical to what the codegen path would
	// produce, and skip the full IML pipeline entirely.
	//
	// On a hit with verify=on we keep the cached bytes in
	// jitCacheHitHostBytes and fall through to the IML+codegen path so
	// the next memcmp will compare cached against fresh. The fresh
	// codegen wins -- the cached entry is informational only in verify
	// mode.
	std::vector<uint8_t> jitCacheHitHostBytes;
	std::vector<JitCacheBridge::EntryPoint> jitCacheHitEntries;
	const bool jitCacheHit = JitCacheBridge::peekLookup(ppcRecFunc, jitCacheHitHostBytes, jitCacheHitEntries);
	// consumeVerifyBudget is decremented once per hit; after the budget
	// runs out, isVerifyEnabled is still true (env var) but this returns
	// false so the rest of the session takes the fast path.
	const bool jitCacheVerifyThisHit = jitCacheHit && JitCacheBridge::consumeVerifyBudget();
	if (jitCacheHit && !jitCacheVerifyThisHit)
	{
		if (PPCRecompiler_loadAArch64FromCache(ppcRecFunc, jitCacheHitHostBytes.data(), jitCacheHitHostBytes.size()))
		{
			entryPointsOut.clear();
			entryPointsOut.reserve(jitCacheHitEntries.size());
			for (const auto& ep : jitCacheHitEntries)
				entryPointsOut.emplace_back(ep.ppcAddr, ep.hostOffset);

			// Mirror the list_ranges entry that
			// PPCRecompiler_generateIntermediateCode would have pushed.
			// Without it the invalidation tracker can't detect overlap
			// with the cached function on a PPC memory write.
			ppcRecRange_t recRange{};
			recRange.ppcAddress = ppcRecFunc->ppcAddress;
			recRange.ppcSize = ppcRecFunc->ppcSize;
			ppcRecFunc->list_ranges.push_back(recRange);
			return ppcRecFunc;
		}
		// Install failed (e.g. non-4-aligned cached size). Treat as miss
		// and proceed with the full codegen path.
	}
#endif

	// generate intermediate code
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	bool compiledSuccessfully = PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker);
	if (compiledSuccessfully == false)
	{
		delete ppcRecFunc;
		return nullptr;
	}

	uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
	uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();

	if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
	{
		if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
		{
			delete ppcRecFunc;
			return nullptr;
		}
	}

	// apply passes
	if (!PPCRecompiler_ApplyIMLPasses(ppcImlGenContext))
	{
		delete ppcRecFunc;
		return nullptr;
	}

#if defined(ARCH_X86_64)
	// emit x64 code
	bool x64GenerationSuccess = PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext);
	if (x64GenerationSuccess == false)
	{
		return nullptr;
	}
#elif defined(__aarch64__)
	bool aarch64GenerationSuccess = PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext);
	if (aarch64GenerationSuccess == false)
	{
		return nullptr;
	}
#endif
	if (ActiveSettings::DumpRecompilerFunctionsEnabled())
	{
		FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
		if (fs)
		{
			fs->writeData(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
			delete fs;
		}
	}

	// collect list of PPC-->x64 entry points
	entryPointsOut.clear();
	for(IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
	{
		if (imlSegment->isEnterable == false)
			continue;

		uint32 ppcEnterOffset = imlSegment->enterPPCAddress;
		uint32 x64Offset = imlSegment->x64Offset;

		entryPointsOut.emplace_back(ppcEnterOffset, x64Offset);
	}

#if defined(__aarch64__)
	// VERIFY mode: phase 3b cache hit landed at the top of this function,
	// but we ran the full codegen path anyway. Compare cached bytes
	// against fresh codegen and log any mismatch. The fresh codegen
	// stays installed; the cache entry is not overwritten (peekLookup
	// already happened, and endFunction below will replace the entry
	// with the fresh bytes, so any mismatch resolves itself on the next
	// launch).
	if (jitCacheVerifyThisHit)
	{
		const size_t cachedSize = jitCacheHitHostBytes.size();
		const size_t freshSize = ppcRecFunc->x86CodeLen;
		const uint8_t* freshBytes = static_cast<const uint8_t*>(ppcRecFunc->x86Code);
		if (cachedSize != freshSize ||
		    std::memcmp(jitCacheHitHostBytes.data(), freshBytes, cachedSize) != 0)
		{
			JitCacheBridge::recordVerifyMismatch(
			    ppcRecFunc->ppcAddress,
			    jitCacheHitHostBytes.data(), cachedSize,
			    freshBytes, freshSize);
		}
	}

	// Commit the just-emitted function to the JIT cache. Entry points were
	// collected just above; the bridge accumulated relocs during codegen.
	// Has to be after the entry-point collection -- the cache stores entry
	// points so a phase-3 read path can rebuild the dispatcher table on hit.
	std::vector<JitCacheBridge::EntryPoint> bridgeEntries;
	bridgeEntries.reserve(entryPointsOut.size());
	for (const auto& ep : entryPointsOut)
		bridgeEntries.push_back({static_cast<uint32_t>(ep.first), ep.second});
	JitCacheBridge::endFunction(
	    static_cast<const uint8_t*>(ppcRecFunc->x86Code),
	    ppcRecFunc->x86CodeLen,
	    bridgeEntries.data(),
	    bridgeEntries.size());
#endif

#if PPCREC_LOG_RECOMPILATION_RESULTS
	bt.Stop();
	uint32 codeHash = 0;
	for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
	{
		codeHash = _rotr(codeHash, 3);
		codeHash += ((uint8*)ppcRecFunc->x86Code)[i];
	}
	cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> x64: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}", (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code, bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

	return ppcRecFunc;
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLRegisterAllocatorParameters raParam;

	for (auto& it : ppcImlGenContext.mappedRegs)
		raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);

#if defined(ARCH_X86_64)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R12);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);

	// add XMM registers, except XMM15 which is the temporary register
	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 0);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 1);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 2);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 3);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 4);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 5);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 6);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 7);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 8);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 9);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 10);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 11);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 12);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 13);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 14);
#elif defined(__aarch64__)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
	{
		if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
			continue; // Skip reserved platform register
		gprPhysPool.SetAvailable(i);
	}

	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
		fprPhysPool.SetAvailable(i);
#endif

	IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
	// isolate entry points from function flow (enterable segments must not be the target of any other segment)
	// this simplifies logic during register allocation
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);

	// merge certain float load+store patterns
	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	// delay byte swapping for certain load+store patterns
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);

	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);

	return true;
}

// JIT symbol map dump for profilers. Enabled at runtime by creating
// /data/data/info.cemu.cemu/files/dbg_jit_map.txt (Android) — the file's mere
// presence at JIT-function-install time switches on perf-<pid>.map emission to
// /data/local/tmp/perf-<pid>.map, the standard format Linux perf / simpleperf
// reads to resolve JIT'd code addresses. Without the trigger file this is a
// no-op (single boolean load).
//
// Map line format: "HEX_HOST_ADDR HEX_HOST_SIZE PPC_<HEX_PPC_ADDR>"
namespace {
std::atomic<bool> g_jitMapEnabled{false};
std::atomic<bool> g_jitMapChecked{false};
FILE* g_jitMapFile = nullptr;
std::mutex g_jitMapMutex;

void PPCRecompiler_maybeOpenJitMap()
{
	if (g_jitMapChecked.load(std::memory_order_acquire))
		return;
	std::lock_guard lk(g_jitMapMutex);
	if (g_jitMapChecked.load(std::memory_order_relaxed))
		return;
#ifdef __ANDROID__
	// Probe a few paths so the trigger works for both the release (info.cemu.cemu)
	// and debug (info.cemu.cemu.debug) packages, and also accepts a /data/local/tmp
	// drop from adb shell when running as a sandboxed app.
	const char* triggerCandidates[] = {
		"/data/local/tmp/cemu_dbg_jit_map.txt",
		"/data/data/info.cemu.cemu/files/dbg_jit_map.txt",
		"/data/data/info.cemu.cemu.debug/files/dbg_jit_map.txt",
		"/storage/emulated/0/Android/data/info.cemu.cemu/files/dbg_jit_map.txt",
		"/storage/emulated/0/Android/data/info.cemu.cemu.debug/files/dbg_jit_map.txt",
	};
	FILE* trigger = nullptr;
	for (const char* p : triggerCandidates)
	{
		trigger = fopen(p, "r");
		cemuLog_log(LogType::Force, "JIT map trigger probe: {} -> {}", p, trigger ? "found" : "absent");
		if (trigger)
			break;
	}
#else
	FILE* trigger = fopen("dbg_jit_map.txt", "r");
#endif
	if (trigger)
	{
		fclose(trigger);
		char path[256];
#ifdef __ANDROID__
		// Try a series of paths in order of decreasing reach. /data/local/tmp/ is
		// the perf-tooling-standard location but a sandboxed app generally can't
		// write there. The app's own external data dir is always writable.
		const char* outCandidates[] = {
			"/data/local/tmp/perf-%d.map",
			"/storage/emulated/0/Android/data/info.cemu.cemu.debug/files/perf-%d.map",
			"/storage/emulated/0/Android/data/info.cemu.cemu/files/perf-%d.map",
			"/data/data/info.cemu.cemu.debug/files/perf-%d.map",
			"/data/data/info.cemu.cemu/files/perf-%d.map",
		};
		for (const char* fmt : outCandidates)
		{
			snprintf(path, sizeof(path), fmt, (int)getpid());
			g_jitMapFile = fopen(path, "w");
			cemuLog_log(LogType::Force, "JIT map output probe: {} -> {}", path, g_jitMapFile ? "ok" : "denied");
			if (g_jitMapFile)
				break;
		}
#else
		snprintf(path, sizeof(path), "perf-%d.map", (int)getpid());
		g_jitMapFile = fopen(path, "w");
#endif
		if (g_jitMapFile)
		{
			cemuLog_log(LogType::Force, "JIT map dump enabled: {}", path);
			g_jitMapEnabled.store(true, std::memory_order_release);
		}
		// Also dump the HLE function table next to the JIT map so we can decode
		// trampoline opcodes (each is `(1<<26) | hleIdx`) back to library/function names.
#ifdef __ANDROID__
		const char* hleDumpCandidates[] = {
			"/storage/emulated/0/Android/data/info.cemu.cemu.debug/files/hle_table.txt",
			"/storage/emulated/0/Android/data/info.cemu.cemu/files/hle_table.txt",
			"/data/data/info.cemu.cemu.debug/files/hle_table.txt",
			"/data/data/info.cemu.cemu/files/hle_table.txt",
		};
		FILE* hleOut = nullptr;
		for (const char* p : hleDumpCandidates)
		{
			hleOut = fopen(p, "w");
			if (hleOut)
			{
				cemuLog_log(LogType::Force, "HLE table dump: {}", p);
				break;
			}
		}
#else
		FILE* hleOut = fopen("hle_table.txt", "w");
#endif
		if (hleOut)
		{
			osLib_dumpFunctionTable(hleOut);
			fclose(hleOut);
		}
	}
	g_jitMapChecked.store(true, std::memory_order_release);
}

void PPCRecompiler_recordJitMapping(uint32 ppcAddr, void* hostAddr, size_t hostSize)
{
	if (!g_jitMapEnabled.load(std::memory_order_acquire))
		return;
	std::lock_guard lk(g_jitMapMutex);
	if (!g_jitMapFile)
		return;
	fprintf(g_jitMapFile, "%llx %zx PPC_%08x\n",
	        (unsigned long long)(uintptr_t)hostAddr, hostSize, ppcAddr);
	fflush(g_jitMapFile);
}
}

bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
	PPCRecompiler_maybeOpenJitMap();
	PPCRecompiler_recordJitMapping(initialEntryPoint, ppcRecFunc->x86Code, ppcRecFunc->x86Size);
	// update jump table
	PPCRecompilerState.recompilerSpinlock.lock();

	// check if the initial entrypoint is still flagged for recompilation
	// its possible that the range has been invalidated during the time it took to translate the function
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[initialEntryPoint / 4] != PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// check if the current range got invalidated during the time it took to recompile it
	bool isInvalidated = false;
	for (auto& invRange : PPCRecompilerState.invalidationRanges)
	{
		MPTR rStartAddr = invRange.startAddress;
		MPTR rEndAddr = rStartAddr + invRange.size;
		for (auto& recFuncRange : ppcRecFunc->list_ranges)
		{
			if (recFuncRange.ppcAddress < (rEndAddr) && (recFuncRange.ppcAddress + recFuncRange.ppcSize) >= rStartAddr)
			{
				isInvalidated = true;
				break;
			}
		}
	}
	PPCRecompilerState.invalidationRanges.clear();
	if (isInvalidated)
	{
		PPCRecompilerState.recompilerSpinlock.unlock();
		return false;
	}


	// update jump table
	for (auto& itr : entryPoints)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[itr.first / 4] = (PPCREC_JUMP_ENTRY)((uint8*)ppcRecFunc->x86Code + itr.second);
	}


	// due to inlining, some entrypoints can get optimized away
	// therefore we reset all addresses that are still marked as visited (but not recompiled)
	// we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
	// if they are reachable, the interpreter will queue them again
	for (uint32 v = range.startAddress; v <= (range.startAddress + range.length); v += 4)
	{
		auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4];
		if (funcPtr == PPCRecompiler_leaveRecompilerCode_visited)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}

	// register ranges
	for (auto& r : ppcRecFunc->list_ranges)
	{
		r.storedRange = rangeStore_ppcRanges.storeRange(ppcRecFunc, r.ppcAddress, r.ppcAddress + r.ppcSize);
	}
	PPCRecompilerState.recompilerSpinlock.unlock();


	return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4] == PPCRecompiler_leaveRecompilerCode_visited);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	PPCRecompilerState.recompilerSpinlock.lock();

	std::set<uint32> entryAddresses;

	entryAddresses.emplace(address);

	PPCRecompilerState.recompilerSpinlock.unlock();

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	auto func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries);

	if (!func)
	{
		return; // recompilation failed
	}
	bool r = PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints);
}

std::thread s_threadRecompiler;
std::atomic_bool s_recompilerThreadStopSignal{false};

void PPCRecompiler_thread()
{
	SetThreadName("PPCRecompiler");
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	return;
#endif

	while (true)
	{
        if(s_recompilerThreadStopSignal)
            return;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// asynchronous recompilation:
		// 1) take address from queue
		// 2) check if address is still marked as visited
		// 3) if yes -> calculate size, gather all entry points, recompile and update jump table
		while (true)
		{
			PPCRecompilerState.recompilerSpinlock.lock();
			if (PPCRecompilerState.targetQueue.empty())
			{
				PPCRecompilerState.recompilerSpinlock.unlock();
				break;
			}
			auto enterAddress = PPCRecompilerState.targetQueue.front();
			PPCRecompilerState.targetQueue.pop();

			auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
			if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
			{
				// only recompile functions if marked as visited
				PPCRecompilerState.recompilerSpinlock.unlock();
				continue;
			}
			PPCRecompilerState.recompilerSpinlock.unlock();

			PPCRecompiler_recompileAtAddress(enterAddress);
			if(s_recompilerThreadStopSignal)
				return;
		}
	}
}

#define PPC_REC_ALLOC_BLOCK_SIZE	(4*1024*1024) // 4MB

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p1 = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerFuncTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	void* p3 = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p1 || !p3 )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

struct ppcRecompilerFuncRange_t
{
	MPTR	ppcStart;
	uint32  ppcSize;
	void*   x86Start;
	size_t  x86Size;
};

bool PPCRecompiler_findFuncRanges(uint32 addr, ppcRecompilerFuncRange_t* rangesOut, size_t* countInOut)
{
	PPCRecompilerState.recompilerSpinlock.lock();
	size_t countIn = *countInOut;
	size_t countOut = 0;

	rangeStore_ppcRanges.findRanges(addr, addr + 4, [rangesOut, countIn, &countOut](uint32 start, uint32 end, PPCRecFunction_t* func)
	{
		if (countOut < countIn)
		{
			rangesOut[countOut].ppcStart = start;
			rangesOut[countOut].ppcSize = (end-start);
			rangesOut[countOut].x86Start = func->x86Code;
			rangesOut[countOut].x86Size = func->x86Size;
		}
		countOut++;
	}
	);
	PPCRecompilerState.recompilerSpinlock.unlock();
	*countInOut = countOut;
	if (countOut > countIn)
		return false;
	return true;
}

extern "C" DLLEXPORT uintptr_t * PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_invalidateTableRange(uint32 offset, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	for (uint32 i = 0; i < size / 4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerFuncTable[offset / 4 + i] = nullptr;
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset / 4 + i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_deleteFunction(PPCRecFunction_t* func)
{
	// assumes PPCRecompilerState.recompilerSpinlock is already held
	cemu_assert_debug(PPCRecompilerState.recompilerSpinlock.is_locked());
	for (auto& r : func->list_ranges)
	{
		PPCRecompiler_invalidateTableRange(r.ppcAddress, r.ppcSize);
		if(r.storedRange)
			rangeStore_ppcRanges.deleteRange(r.storedRange);
		r.storedRange = nullptr;
	}
	// todo - free x86 code
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (ppcRecompilerEnabled == false)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);

	PPCRecompilerState.recompilerSpinlock.lock();

	uint32 rStart;
	uint32 rEnd;
	PPCRecFunction_t* rFunc;

	// mark range as unvisited
	for (uint64 currentAddr = (uint64)startAddr&~3; currentAddr < (uint64)(endAddr&~3); currentAddr += 4)
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[currentAddr / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;

	// add entry to invalidation queue
	PPCRecompilerState.invalidationRanges.emplace_back(startAddr, endAddr-startAddr);


	while (rangeStore_ppcRanges.findFirstRange(startAddr, endAddr, rStart, rEnd, rFunc) )
	{
		PPCRecompiler_deleteFunction(rFunc);
	}

	PPCRecompilerState.recompilerSpinlock.unlock();
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[0] = 0xFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;

	// mxcsr
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn = 0x1F80 | 0x8000;
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
}
#else
void PPCRecompiler_initPlatform()
{
    
}
#endif

void PPCRecompiler_init()
{
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		ppcRecompilerEnabled = false;
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}
	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
	}
	debug_printf("Allocating %dMB for recompiler instance data...\n", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    PPCRecompiler_allocateRange(0, 0x1000); // the first entry is used for fallback to interpreter
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

    PPCRecompiler_initPlatform();

#if defined(__aarch64__)
	// Attach the JIT translation cache (write path) for the foreground
	// title. Cemu-side glue interns the well-known symbols and starts
	// catching every successful AArch64 codegen via JitCacheBridge::*.
	JitCacheBridge::initialize(static_cast<uint64_t>(CafeSystem::GetForegroundTitleId()));
#endif

	cemuLog_log(LogType::Force, "Recompiler initialized");

	ppcRecompilerEnabled = true;

	// launch recompilation thread
    s_recompilerThreadStopSignal = false;
    s_threadRecompiler = std::thread(PPCRecompiler_thread);
}

// Bit pattern for `bl <imm26>` (linked relative branch, AA=0, LK=1):
//   opcode (bits 0-5) = 010010 (0x12), AA=0, LK=1 -> 0x48000001.
// Mask 0xFC000003 isolates opcode + AA + LK so we don't match conditional
// branches or absolute (AA=1) variants.
static constexpr uint32 kBLOpcodeMask    = 0xFC000003;
static constexpr uint32 kBLOpcodeValue   = 0x48000001;
// `b <imm26>` (unconditional relative branch, AA=0, LK=0). Tail calls
// use this form; PPC compilers often emit `b <other_function>` as the
// last instruction instead of returning through a different path.
static constexpr uint32 kBOpcodeValue    = 0x48000000;

// Extracts the sign-extended LI field from a bl/b instruction and returns
// the absolute PPC target address (current_addr + LI*4).
static inline uint32 PPCRecompiler_branchTargetFromImm26(uint32 insn, uint32 currentAddr)
{
	int32_t li = static_cast<int32_t>(insn & 0x03FFFFFC);
	if (li & 0x02000000) // sign bit (bit 25) -> sign-extend to int32
		li |= 0xFC000000;
	return currentAddr + static_cast<uint32>(li);
}

// PPC primary opcode (top 6 bits).
static inline uint32 PPCRecompiler_primaryOpcode(uint32 insn)
{
	return (insn >> 26) & 0x3F;
}

// CodeWarrior prologue: every non-leaf function begins with one of
//   mflr r0; ...; stwu r1, -N(r1)
// OR (less common, big stack frames):
//   mflr r0; stwu r1, ...; ...
// We accept any 4-insn window starting with mflr where stwu r1 also appears.
//
//   mflr rD encoding: 31|D|0x0008|0xA6  -> primary=0x1F (31), special bits
//     full pattern: 0x7C0802A6 for mflr r0 (D=0). Mask on rD digit so any
//     destination matches: 0x7C0002A6 with mask 0xFC1FFFFF.
//   stwu r1, N(r1): primary=0x25 (37), rS=1, rA=1. Encoding base 0x9421xxxx.
//     full mask on the opcode + rS + rA fields: 0xFFFF0000 == 0x94210000.
static inline bool PPCRecompiler_isMflrR0(uint32 insn)
{
	return (insn & 0xFC1FFFFF) == 0x7C0002A6;
}
static inline bool PPCRecompiler_isStwuR1R1(uint32 insn)
{
	return (insn & 0xFFFF0000) == 0x94210000;
}

// Longcall trampoline pattern -- used by the linker for `bl` targets out of
// the +/-32MB reach of imm26:
//     lis  rN, hi16          ; primary=15 (0x0F)
//     addi rN, rN, lo16      ; primary=14 (0x0E), or `ori` (0x18)
//     mtctr rN               ; encoding 0x7C0903A6 with mask on rS
//     bctrl                  ; 0x4E800421
// The composed 32-bit target is (hi16 << 16) + (sign-extended lo16). Returns
// 0 if the 4-insn window doesn't match the pattern. The same register must
// be used across all three preceding ops.
static inline uint32 PPCRecompiler_extractLongcallTarget(uint32 i0, uint32 i1, uint32 i2, uint32 i3)
{
	// bctrl is fixed.
	if (i3 != 0x4E800421)
		return 0;
	// lis rN, hi16
	if (PPCRecompiler_primaryOpcode(i0) != 15)
		return 0;
	uint32 rN_lis = (i0 >> 21) & 0x1F;
	uint32 hi16 = i0 & 0xFFFF;
	// addi rN, rN, lo16 (or ori with extension)
	const uint32 op1 = PPCRecompiler_primaryOpcode(i1);
	uint32 rN_addi = (i1 >> 21) & 0x1F;
	uint32 rA_addi = (i1 >> 16) & 0x1F;
	if ((op1 != 14 && op1 != 24) || rN_addi != rN_lis || rA_addi != rN_lis)
		return 0;
	int32_t lo16 = static_cast<int16_t>(i1 & 0xFFFF); // sign-extended for addi
	if (op1 == 24)
		lo16 = static_cast<uint16_t>(i1 & 0xFFFF); // zero-extended for ori
	// mtctr rN -> mtspr CTR. Encoding 0x7C0903A6 with rS in bits 21..25.
	if ((i2 & 0xFC1FFFFF) != 0x7C0903A6)
		return 0;
	uint32 rS_mtctr = (i2 >> 21) & 0x1F;
	if (rS_mtctr != rN_lis)
		return 0;
	return (hi16 << 16) + static_cast<uint32>(lo16);
}

void PPCRecompiler_precompileLoadedModules()
{
	if (!ppcRecompilerEnabled)
		return;

	std::set<uint32> entryAddrs;

	RPLModule** moduleList = RPLLoader_GetModuleList();
	const sint32 moduleCount = RPLLoader_GetModuleCount();

	for (sint32 i = 0; i < moduleCount; ++i)
	{
		RPLModule* mod = moduleList[i];
		if (!mod)
			continue;
		const uint32 textBase = mod->regionMappingBase_text.GetMPTR();
		const uint32 textSize = mod->regionSize_text;
		if (textBase == 0 || textSize == 0)
			continue;

		// Module entrypoint -- guaranteed function start by Wii U OS contract.
		const uint32 entrypoint = RPLLoader_GetModuleEntrypoint(mod);
		if (entrypoint >= textBase && entrypoint < textBase + textSize)
			entryAddrs.insert(entrypoint);

		// Function exports -- guaranteed function starts.
		for (uint32 e = 0; e < mod->exportFCount; ++e)
		{
			const uint32 addr = mod->exportFDataPtr[e].virtualOffset;
			if (addr >= textBase && addr < textBase + textSize)
				entryAddrs.insert(addr);
		}

		// Per-instruction scan unions four signal sources, chosen for their
		// precision against Ghidra ground truth (per the offline discovery
		// tool's report card on cking.rpx + red-pro2.rpx):
		//   1) bl <imm26> xref targets  -- direct callees, ~99% precision
		//   2) prologue (mflr; ... stwu r1,...) -- function entry signature
		//      emitted by every non-leaf CodeWarrior function
		//   3) longcall trampoline      -- (lis;addi|ori;mtctr;bctrl) packs
		//      a 32-bit target for any jump >+/-32MB
		//   4) module exports + entrypoint (already harvested above)
		//
		// We intentionally do NOT walk `b imm26` xrefs: most are intra-
		// function branch labels (not function starts) and the noise
		// produced black-screen behavior in the first prototype.
		const uint32 endOff = (textSize >= 16) ? (textSize - 16) : 0;
		for (uint32 off = 0; off + 4 <= textSize; off += 4)
		{
			const uint32 addr = textBase + off;
			const uint32 i0 = memory_readU32(addr);

			// Source 1: bl imm26.
			if ((i0 & kBLOpcodeMask) == kBLOpcodeValue)
			{
				const uint32 target = PPCRecompiler_branchTargetFromImm26(i0, addr);
				if (target >= textBase && target < textBase + textSize)
					entryAddrs.insert(target);
			}

			// Source 2: prologue. mflr starts a function; if the same
			// instruction is also the start of .text or a stwu r1 lands
			// in the next few insns, that's a function entry. We accept
			// any mflr with a stwu r1,r1 within the next 8 insns (32 B)
			// since CodeWarrior's prologue may interleave a few register
			// saves before establishing the frame.
			if (PPCRecompiler_isMflrR0(i0))
			{
				const uint32 windowEnd = std::min<uint32>(off + 8 * 4, textSize);
				for (uint32 j = off + 4; j + 4 <= windowEnd; j += 4)
				{
					if (PPCRecompiler_isStwuR1R1(memory_readU32(textBase + j)))
					{
						entryAddrs.insert(addr);
						break;
					}
				}
			}

			// Source 3: longcall trampoline. Need a 4-insn window.
			if (off <= endOff)
			{
				const uint32 i1 = memory_readU32(addr + 4);
				const uint32 i2 = memory_readU32(addr + 8);
				const uint32 i3 = memory_readU32(addr + 12);
				const uint32 longTarget =
				    PPCRecompiler_extractLongcallTarget(i0, i1, i2, i3);
				if (longTarget != 0
				    && longTarget >= textBase && longTarget < textBase + textSize)
				{
					entryAddrs.insert(longTarget);
				}
			}
		}
	}

	uint32 queued = 0;
	for (uint32 addr : entryAddrs)
	{
		if (addr >= PPC_REC_CODE_AREA_END || (addr & 3) != 0)
			continue;
		// recompileIfUnvisited is the canonical lazy-JIT entry: it locks
		// the spinlock, flips the dispatch-table slot from unvisited to
		// visited, and pushes to the worker thread's queue. Anything
		// we miss here falls through to the regular lazy-JIT path
		// during gameplay.
		PPCRecompiler_recompileIfUnvisited(addr);
		++queued;
	}

	cemuLog_log(LogType::Force,
	            "Precompile: {} entries discovered across {} modules. The recompiler will warm in the background; first launch is the only slow one, subsequent launches load from the on-disk cache.",
	            queued, moduleCount);

	// Pre-warm notification so the user sees something immediately on the
	// overlay, before the first 5s tick of the monitor thread.
	LatteOverlay_pushNotification(
	    fmt::format("Warming JIT cache: {} functions queued", queued),
	    3500);

	// NON-BLOCKING by design: blocking title load before Latte_Start means
	// the GPU is never initialized and the screen stays black for the
	// entire precompile. Instead we let cemu_initForGame proceed straight
	// to Latte_Start so the title's own splash/intro renders normally
	// while the worker thread drains the queue. A monitor thread emits
	// periodic progress lines so the user has feedback both in log.txt
	// and as a transient on-screen notification.
	//
	// Power story across launches:
	//   Run 1 (empty cache): early gameplay JITs ~18k functions over
	//     ~30-60s. Visible as below-target framerate during early play.
	//     Disk cache fills as side effect.
	//   Run N (warm cache): worker thread drains its queue almost
	//     instantly because every dispatch is a memcpy of cached host
	//     bytes (no IML, no codegen). Gameplay is JIT-cost-free from
	//     the very first frame.
	if (queued > 0)
	{
		std::thread([queuedAtStart = queued]() {
			const auto start = std::chrono::steady_clock::now();
			for (;;)
			{
				std::this_thread::sleep_for(std::chrono::seconds(5));
				PPCRecompilerState.recompilerSpinlock.lock();
				const uint32 remaining = static_cast<uint32>(PPCRecompilerState.targetQueue.size());
				PPCRecompilerState.recompilerSpinlock.unlock();
				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				    std::chrono::steady_clock::now() - start)
				                         .count();
				if (remaining == 0)
				{
					cemuLog_log(LogType::Force,
					            "Precompile complete: drained {} entries in {}s",
					            queuedAtStart, static_cast<long long>(elapsed));
					LatteOverlay_pushNotification(
					    fmt::format("JIT cache warm: {} functions in {}s",
					                queuedAtStart, static_cast<long long>(elapsed)),
					    4000);
					return;
				}
				const uint32 done = (remaining > queuedAtStart) ? 0 : (queuedAtStart - remaining);
				const double pct = queuedAtStart > 0 ? (100.0 * done / queuedAtStart) : 100.0;
				cemuLog_log(LogType::Force,
				            "Precompile progress: {}/{} ({:.0f}%) in {}s",
				            done, queuedAtStart, pct, static_cast<long long>(elapsed));
				LatteOverlay_pushNotification(
				    fmt::format("JIT cache warmup: {:.0f}% ({}/{})",
				                pct, done, queuedAtStart),
				    4500);
			}
		}).detach();
	}
}

void PPCRecompiler_Shutdown()
{
    // shut down recompiler thread
    s_recompilerThreadStopSignal = true;
    if(s_threadRecompiler.joinable())
        s_threadRecompiler.join();
#if defined(__aarch64__)
    // Flush any pending JIT-cache entries to disk before tearing down.
    // Safe to call even if initialize was never invoked.
    JitCacheBridge::shutdown();
#endif
    // clean up queues
    while(!PPCRecompilerState.targetQueue.empty())
        PPCRecompilerState.targetQueue.pop();
    PPCRecompilerState.invalidationRanges.clear();
    // clean range store
    rangeStore_ppcRanges.clear();
    // clean up memory
    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for(uint32 i=0; i<numBlocks; i++)
    {
        if(!ppcRecompiler_reservedBlockMask[i])
            continue;
        // deallocate
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerFuncTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        // mark as unmapped
        ppcRecompiler_reservedBlockMask[i] = false;
    }
}
