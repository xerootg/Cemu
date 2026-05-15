#include "PPCStdlibStubs.h"

#include "Cafe/HW/MMU/MMU.h"
#include "Cemu/Logging/CemuLogging.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace
{

// FNV-1a 64-bit. Cheap, decent dispersion, plenty good for a small static
// registry of known-good byte windows.
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime       = 0x00000100000001b3ULL;

uint64_t Fnv1a64(const uint8_t* data, size_t len)
{
	uint64_t h = kFnvOffsetBasis;
	for (size_t i = 0; i < len; ++i)
	{
		h ^= data[i];
		h *= kFnvPrime;
	}
	return h;
}

// Each registry entry stores the full known-good prologue bytes plus the
// expected total function size. We hash a fixed window (first 64 bytes,
// or fewer for short functions) for the O(1) prefilter, then memcmp the
// full window on a hit. The size check guards against a longer function
// that happens to share a 64-byte prefix with a known helper.
struct StubEntry
{
	PPCStdlibStubKind kind;
	uint32_t      funcSize;     // expected exact function length, in bytes
	uint32_t      hashLen;      // bytes hashed (== sizeof(prologue))
	uint64_t      hash;         // precomputed Fnv1a64 of `prologue`
	const uint8_t* prologue;    // pointer to static data below
};

// __lldiv (signed 64-bit divide), CodeWarrior for PowerPC. Bit-identical
// across every CW-compiled Wii U title. Captured from WW HD (cking.rpx)
// at 0x028f644c. Verified against red-pro2.rpx and appstore.rpx -- same
// bytes at the same internal offsets.
//
// 0x158 bytes total (88 PPC instructions). Hash is over the first 64
// bytes (the prologue + signed-special-case fast path).
constexpr uint8_t kLldiv_Prologue[64] = {
	0x7c, 0x08, 0x02, 0xa6,  // mflr  r0
	0x94, 0x21, 0xff, 0xf0,  // stwu  r1, -0x10(r1)
	0x93, 0xe1, 0x00, 0x0c,  // stw   r31, 0xc(r1)
	0x7c, 0x7f, 0x1b, 0x78,  // mr    r31, r3
	0x57, 0xe9, 0x00, 0x00,  // rlwinm r9, r31, 0, 0, 0
	0x54, 0x8b, 0x00, 0x00,  // rlwinm r11, r4, 0, 0, 0
	0x7c, 0x09, 0x58, 0x40,  // cmplw r9, r11
	0x90, 0x01, 0x00, 0x14,  // stw   r0, 0x14(r1)
	0x40, 0x82, 0x00, 0x6c,  // bne   ...
	0x2c, 0x1f, 0x00, 0x00,  // cmpwi r31, 0
	0x41, 0x82, 0x00, 0x0c,  // beq   ...
	0x2c, 0x1f, 0xff, 0xff,  // cmpwi r31, -1
	0x40, 0x82, 0x00, 0x5c,  // bne   ...
	0x7c, 0xab, 0x2b, 0x78,  // mr    r11, r5
	0x54, 0xc7, 0x00, 0x00,  // rlwinm r7, r6, 0, 0, 0
	0x55, 0x6c, 0x00, 0x00,  // rlwinm r12, r11, 0, 0, 0
};
// Size as reported by Cemu's PPCFunctionBoundaryTracker (0xfc = 63 PPC
// instructions, ending at the function's first blr at offset 0xfc).
// Ghidra reports the larger 0x158 because it follows internal branches
// past the blr; the runtime tracker correctly stops at the first blr,
// which is the function's main return.
constexpr uint32_t kLldiv_FuncSize = 0xfc;

// Registry: extend with more helpers (memcpy, memset, __divull, etc.) by
// appending entries. Lookup is linear -- expected to stay short.
const std::array<StubEntry, 1> kStubRegistry = {{
	{
		PPCStdlibStubKind::LongLongDivide_Signed,
		kLldiv_FuncSize,
		sizeof(kLldiv_Prologue),
		Fnv1a64(kLldiv_Prologue, sizeof(kLldiv_Prologue)),
		kLldiv_Prologue,
	},
}};

} // namespace

PPCStdlibStubKind PPCStdlibStubs_Identify(uint32_t funcStartAddr, uint32_t funcSizeBytes)
{
	if (funcSizeBytes == 0)
		return PPCStdlibStubKind::None;

	// Cheap window read first; bail before doing the full memcmp if the
	// hash doesn't match anything.
	uint8_t window[64];
	const uint32_t windowLen = (funcSizeBytes < sizeof(window)) ? funcSizeBytes : (uint32_t)sizeof(window);
	for (uint32_t i = 0; i < windowLen; i += 4)
	{
		uint32_t insn = memory_readU32(funcStartAddr + i);
		// memory_readU32 already byte-swaps to native; we want raw PPC bytes
		// here for byte-by-byte comparison against our static prologues.
		window[i + 0] = (uint8_t)(insn >> 24);
		window[i + 1] = (uint8_t)(insn >> 16);
		window[i + 2] = (uint8_t)(insn >> 8);
		window[i + 3] = (uint8_t)(insn >> 0);
	}

	const uint64_t windowHash = Fnv1a64(window, windowLen);

	for (const auto& entry : kStubRegistry)
	{
		if (entry.hashLen != windowLen) continue;
		if (entry.hash != windowHash) continue;
		// Hash matches; memcmp the bytes to guard against a hash collision.
		if (std::memcmp(window, entry.prologue, windowLen) != 0) continue;
		// Hash + bytes both match. If size disagrees, log once and bail
		// (don't substitute -- we may be looking at a longer function with
		// a shared prefix). If size matches, accept.
		if (entry.funcSize != funcSizeBytes)
		{
			static std::atomic<bool> s_loggedSizeMismatch[64] = {};
			size_t kindIdx = (size_t)entry.kind;
			if (kindIdx < std::size(s_loggedSizeMismatch) && !s_loggedSizeMismatch[kindIdx].exchange(true))
				cemuLog_log(LogType::Force, "[stdlib-stub] size mismatch for kind={} at PPC 0x{:08x}: expected 0x{:x}, got 0x{:x}",
				            (uint32_t)entry.kind, funcStartAddr, entry.funcSize, funcSizeBytes);
			continue;
		}
		// One-time log per kind so we can confirm the substitution fires
		// in the wild without spamming.
		static std::atomic<bool> s_loggedKinds[64] = {};
		size_t kindIdx = (size_t)entry.kind;
		if (kindIdx < std::size(s_loggedKinds) && !s_loggedKinds[kindIdx].exchange(true))
			cemuLog_log(LogType::Force, "[stdlib-stub] substituted helper kind={} at PPC 0x{:08x} (size 0x{:x})",
			            (uint32_t)entry.kind, funcStartAddr, funcSizeBytes);
		return entry.kind;
	}
	return PPCStdlibStubKind::None;
}
