#pragma once

#include "HW/Espresso/Recompiler/IML/IMLInstruction.h"
#include "../PPCRecompiler.h"

bool PPCRecompiler_generateAArch64Code(struct PPCRecFunction_t* PPCRecFunction, struct ppcImlGenContext_t* ppcImlGenContext);
void PPCRecompiler_cleanupAArch64Code(void* code, size_t size);

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