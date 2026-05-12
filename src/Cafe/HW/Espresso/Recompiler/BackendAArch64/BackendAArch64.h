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
	// x24 is held back from the allocator pool to cache PPCInterpreter_t::
	// remainingCycles across the JIT execution session -- one ldr at JIT
	// entry / one str at exit instead of an ldr+sub+str triple at every
	// basic block boundary.
	static constexpr int PHYSREG_GPR_COUNT = 24;
	static constexpr int PHYSREG_FPR_BASE = PHYSREG_GPR_COUNT;
	static constexpr int PHYSREG_FPR_COUNT = 31;
}; // namespace IMLArchAArch64