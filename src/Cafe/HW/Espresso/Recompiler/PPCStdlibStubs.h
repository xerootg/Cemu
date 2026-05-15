#pragma once

#include <cstdint>

// Detection + dispatch for CodeWarrior runtime stdlib helpers (__lldiv,
// __divull, etc.). When a function being JIT'd matches one of these helpers
// by content hash, we replace the translated body with a host-side stub
// that performs the operation directly (e.g. one AArch64 sdiv) and returns
// via the standard PPC blr path. Saves ~80-150 PPC insns per call.
//
// The hash is over the first N PPC instruction bytes of the helper
// prologue. CW runtime functions are bit-identical across all titles
// compiled with the same Metrowerks for PowerPC version, so a single
// hash entry catches every Wii U title that links the helper.

struct PPCRecFunction_t;

// Identifier for which substitution generator to call.
enum class PPCStdlibStubKind : uint32_t
{
	None,
	LongLongDivide_Signed,    // __lldiv  (signed 64-bit / signed 64-bit)
	// Future: LongLongDivide_Unsigned, LongLongMod_*, Memcpy, Memset, Strlen, ...
};

// Look up the stub kind for a freshly-encountered PPC function. Returns
// PPCStdlibStubKind::None when the function doesn't match any known helper.
//
// `funcStartAddr` is the PPC entry-point address; `funcSizeBytes` is the
// total length the function-boundary tracker found. The lookup hashes a
// fixed window of the prologue (or the whole function if shorter) and
// compares against the registry of known helper hashes.
PPCStdlibStubKind PPCStdlibStubs_Identify(uint32_t funcStartAddr, uint32_t funcSizeBytes);

// Emit the host-code stub for a substituted helper into ppcRecFunc->x86Code
// (along with x86Size and x86CodeLen). Caller installs the resulting
// function via the standard makeRecompiledFunctionActive path. Returns
// false if this kind isn't supported on the current backend.
//
// Implemented per-backend; the AArch64 version lives in BackendAArch64.cpp.
bool PPCRecompiler_emitStdlibStub_AArch64(PPCRecFunction_t* ppcRecFunc, PPCStdlibStubKind kind);
