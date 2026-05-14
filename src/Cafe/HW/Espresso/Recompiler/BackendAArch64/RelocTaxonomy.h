#pragma once

// Phase 0 of the persistent JIT translation cache: passive recorder that logs
// every 64-bit absolute pointer emission inside the per-PPC-function host
// code. The cache (see [[project-jitcache]]) needs a closed taxonomy of
// relocations to apply at load time; this file produces the evidence that
// MovzMovk_Abs64 is the only emission shape we need to support.
//
// Read-only / observational: no caching, no code rewriting. The recorder
// appends CSV rows to <UserDataPath>/log/jit_reloc_taxonomy.csv as functions
// are compiled. Pull the file off the device after a session to validate.
//
// Remove this file (and its hook sites in BackendAArch64.cpp) once the cache
// reloc enum is locked in Phase 1.

#include <cstddef>
#include <cstdint>

namespace JitRelocTaxonomy
{

enum class AbsImm64Kind : uint8_t
{
	// Singleton C++ global object/array address (coreinit::g_*, etc.). One
	// distinct value per host symbol; resolvable by symbol name at cache load.
	HLE_GLOBAL_PTR = 0,
	// Singleton host function pointer to a coreinit/HLE export. One distinct
	// value per host symbol; resolvable by symbol name at cache load.
	HLE_FUNCTION_PTR = 1,
	// Singleton host function pointer to a PPCRecompiler_* helper (e.g.
	// PPCRecompiler_virtualHLE). Same resolution model as HLE_FUNCTION_PTR.
	RECOMPILER_HELPER_PTR = 2,
	// IML op_call_imm.callAddress -- per-function variable target. Stored
	// inline with the function bytes; no external resolution needed.
	PPC_CALL_IMM_TARGET = 3,
	// Bare 64-bit constant baked into instruction stream (e.g. a fibonacci
	// hash multiplier). Not actually a relocation -- value is portable across
	// Cemu builds and host processes. Recorded to confirm it's the only
	// non-pointer absolute we emit.
	LITERAL_CONST = 4,
};

const char* kindName(AbsImm64Kind k);

// Called by PPCRecompiler_generateAArch64Code before/after each function's
// codegen. ppcAddress is the guest entry address; hostSize is the total
// number of bytes emitted for that function (computed by the caller after
// codegen completes).
void beginFunction(uint32_t ppcAddress);
void endFunction(size_t hostSize);

// Called from AArch64GenContext_t::emitAbsoluteImm64. codeOffset is the
// host byte offset (CodeGenerator::getSize()) at which the movz/movk sequence
// will begin; value is the immediate that will be loaded; kind is the
// site-supplied classification.
void recordAbsImm64(size_t codeOffset, uint64_t value, AbsImm64Kind kind);

} // namespace JitRelocTaxonomy
