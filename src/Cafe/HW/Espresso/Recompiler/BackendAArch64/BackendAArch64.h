#pragma once

#include "HW/Espresso/Recompiler/IML/IMLInstruction.h"
#include "../PPCRecompiler.h"

bool PPCRecompiler_generateAArch64Code(struct PPCRecFunction_t* PPCRecFunction, struct ppcImlGenContext_t* ppcImlGenContext);
void PPCRecompiler_cleanupAArch64Code(void* code, size_t size);

// Phase 3b read path: allocate executable memory through the same xbyak
// allocator the codegen path uses, copy cached host bytes into it, and
// mark it Read+Execute. On success, ppcRecFunc->x86Code / x86Size /
// x86CodeLen are populated; the memory is owned by ppcRecFunc and is
// freed by the standard PPCRecompiler_cleanupAArch64Code path.
bool PPCRecompiler_loadAArch64FromCache(struct PPCRecFunction_t* ppcRecFunc, const uint8_t* hostBytes, size_t hostSize);

// Host address of the AArch64 backend's PPCRecompiler_virtualHLE so the
// JitCacheBridge can map its interned RuntimeSymbol id back to the live
// function. BackendX64.cpp also defines PPCRecompiler_virtualHLE; taking
// the function's address directly from JitCacheBridge.cpp pulls BOTH
// definitions through the static lib and trips the linker. Going through
// an accessor defined here keeps the bridge's reference scoped to the
// AArch64 backend's .o file only.
void* PPCRecompiler_getVirtualHLEHostAddr();

void PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();

// architecture specific constants
namespace IMLArchAArch64
{
	static constexpr int PHYSREG_GPR_BASE = 0;
	// x23 and x24 are held back from the allocator pool to cache hot
	// PPCInterpreter_t fields across the JIT execution session:
	//   x23 = PPC_LR_REG (spr.LR)        -- consumed by every bl/blr/mflr/mtlr
	//   x24 = REMAINING_CYCLES_REG       -- consumed at every basic-block entry
	// Both are PPCInterpreter_t scalars that the JIT touches dozens of times
	// per ms; the alternative is an ldr/str round-trip to the struct at every
	// reference. Net loss is two fewer host GPRs for the IML allocator; the
	// gain is two of the most-emitted IML names becoming free register moves.
	static constexpr int PHYSREG_GPR_COUNT = 23;
	static constexpr int PHYSREG_FPR_BASE = PHYSREG_GPR_COUNT;
	static constexpr int PHYSREG_FPR_COUNT = 31;
}; // namespace IMLArchAArch64