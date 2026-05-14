#pragma once

// Persistent PPC->host JIT translation cache (skeleton).
//
// This library stores host code bytes per PPC function so subsequent launches
// can skip the IML decode/opt/regalloc/emit pipeline. Public API only -- no
// Cemu types in this header. The producer (Cemu's recompiler backend) calls
// insert() with EmittedCode after compiling; later launches call lookup()
// which applies any pending relocations via a consumer-supplied Resolver.
//
// Phase 1a: in-memory only, placeholder 128-bit hash (FNV-1a). No disk yet.
// Phase 1b: XXH3-128 fingerprint + on-disk manifest + code blob + atomic flush.
// Phase 2:  wires the producer side into Cemu.
// Phase 3:  read path behind a flag with JITCACHE_VERIFY=1 in debug builds.
//
// See project-jitcache and project-precompile-2026-05-12 for the full plan.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jitcache
{

// Cache format version. Bump on any binary-incompatible change to either
// the manifest layout, the fingerprint algorithm, or the wire format of
// stored relocs / strings. Mismatched value on disk -> wipe + start empty.
//
// v1: FNV-1a 128, in-memory only (phase 1a -- never written to disk).
// v2: XXH3-128 fingerprint, manifest.bin + code.bin on disk (phase 1b).
// v3: EntryPoint array per entry; FunctionRecord grew from 48 to 56 bytes
//     (phase 3a, prerequisite for the read path).
constexpr uint32_t kCacheFormatVersion = 3;

// Relocation kinds applied to host code bytes at lookup time.
// Phase 0 (commit b2a5b789) confirmed Aarch64_MovzMovk_Abs64 is the only
// kind currently emitted per-PPC-function on AArch64. Direct branch
// (BL_Imm26 / B_Imm26) is intentionally deferred to a future phase -- see
// project-jitcache. New kinds added here MUST also bump kCacheFormatVersion.
enum class RelocKind : uint8_t
{
	// Sequence of up to 4 AArch64 movz/movk instructions building a 64-bit
	// constant in a destination register. codeOffset points at the first
	// movz; the patch rewrites the 16-bit immediate fields of each
	// instruction in order without touching the register-number bits.
	Aarch64_MovzMovk_Abs64 = 0,
};

// How a reloc's target is resolved.
enum class TargetKind : uint8_t
{
	// Singleton host symbol resolved through Resolver::resolveRuntimeSymbol.
	// targetId is an interned id from Cache::internSymbol.
	RuntimeSymbol = 0,
	// Inline value carried through from compile time. targetId is the raw
	// 64-bit value to patch. Used for op_call_imm.callAddress and any
	// other literal absolute baked into the JIT body.
	EmbeddedValue = 1,
	// Host code address of another JITed PPC function. Reserved for a
	// future direct-branch reloc kind; currently unused.
	PpcCodeAddr = 2,
};

// A guest -> host entry point. Multiple entry points per cached function
// are common: PPC functions are reached by any `bl <addr>` from elsewhere
// in the binary, and the recompiler discovers each enterable IML segment.
// On a cache hit the consumer rebuilds the dispatcher jump table from this
// array; without it, control flow into the cached body would be stuck at
// the leading entry only.
struct EntryPoint
{
	uint32_t ppcAddr;     // PPC address that, when called, lands here
	uint32_t hostOffset;  // byte offset within hostBytes
};
static_assert(sizeof(EntryPoint) == 8, "EntryPoint layout is part of the on-disk format");

struct Reloc
{
	// Byte offset within the function's host code blob where the patch
	// begins. For Aarch64_MovzMovk_Abs64 this is the first movz; the
	// following movk instructions sit at codeOffset + 4, +8, +12.
	uint32_t codeOffset;
	RelocKind kind;
	TargetKind targetKind;
	uint8_t _pad[2];
	// Meaning depends on targetKind:
	//   RuntimeSymbol  -> interned symbol id (see Cache::internSymbol)
	//   EmbeddedValue  -> the raw 64-bit value to patch
	//   PpcCodeAddr    -> PPC entry address of the target function
	uint64_t targetId;
};
static_assert(sizeof(Reloc) == 16, "Reloc must be 16 bytes for the on-disk layout");

// Implemented by the consumer (Cemu). The Cache never includes consumer
// headers. resolveRuntimeSymbol must be cheap and side-effect-free: it is
// called once per RuntimeSymbol reloc on every cache hit.
class Resolver
{
public:
	virtual ~Resolver() = default;
	// Look up a host pointer for an interned singleton symbol. Return 0
	// to signal "unknown"; the cache will treat the function as
	// unresolvable and report lookup failure.
	virtual uint64_t resolveRuntimeSymbol(uint64_t symbolId) const = 0;
	// Resolve a JITed PPC function's host entry address. Reserved for
	// the PpcCodeAddr target kind; currently unused by Phase 1.
	virtual uint64_t resolvePpcCodeAddr(uint64_t ppcAddress) const = 0;
};

// Identity of a function for cache lookup. The cache derives a 128-bit
// fingerprint by hashing all fields plus the PPC bytes themselves; any
// difference (codegen version, host CPU features, module id, entry address,
// body length, or even one PPC byte) yields a different fingerprint and a
// cache miss.
struct FunctionKey
{
	// Per-Cemu-build codegen version. Bump whenever the AArch64 backend
	// changes anything that affects emitted bytes (peephole pass, regalloc
	// tweak, ABI change). Two builds with different codegenVersion never
	// share cache entries even if everything else matches.
	uint32_t codegenVersion = 0;
	// Per-host-CPU-features fingerprint. AArch64 currently has no
	// optional-feature dispatch in the backend, so this is a constant on
	// AArch64; reserved for conditional codegen (e.g. SVE-aware paths).
	uint32_t hostCpuFeatureBits = 0;
	// Identifies the loaded RPL module (typically a content hash of the
	// .rpx). Different modules with overlapping PPC addresses still
	// produce different keys.
	uint64_t moduleId = 0;
	// Guest entry address of the function in PPC space.
	uint32_t ppcEntryAddr = 0;
	// Length in bytes of the PPC body that was compiled.
	uint32_t ppcLen = 0;
	// Pointer to the PPC body bytes. NOT stored in the manifest -- only
	// hashed during key construction. The caller must keep this buffer
	// alive across the insert/lookup call.
	const uint8_t* ppcBytes = nullptr;
};

// 128-bit fingerprint derived from a FunctionKey. The lookup primary key.
struct Fingerprint
{
	uint64_t lo = 0;
	uint64_t hi = 0;

	bool operator==(const Fingerprint& o) const noexcept { return lo == o.lo && hi == o.hi; }
	bool operator!=(const Fingerprint& o) const noexcept { return !(*this == o); }
};

// Host code emitted for one PPC function, plus the relocs to apply at
// load time before the function is callable, plus the set of guest entry
// points the consumer can dispatch into.
struct EmittedCode
{
	std::vector<uint8_t> hostBytes;
	std::vector<Reloc> relocs;
	std::vector<EntryPoint> entryPoints;
};

// Public Cache interface. Construction is cheap; all real work happens
// during insert/lookup.
class Cache
{
public:
	Cache();
	~Cache();

	Cache(const Cache&) = delete;
	Cache& operator=(const Cache&) = delete;

	// Hash a FunctionKey into a 128-bit fingerprint. Same key bytes always
	// yield the same fingerprint within one Cemu build; different
	// codegenVersion / hostCpuFeatureBits values mean two builds never
	// share a cache entry by accident.
	Fingerprint fingerprint(const FunctionKey& key) const;

	// Attach to an on-disk cache rooted at `dir`. Creates the directory
	// if absent. On magic/version mismatch or any I/O error the two cache
	// files (manifest.bin and code.bin) are wiped and the cache stays
	// empty. Always safe to call; never throws.
	//
	// Symbol ids are preserved across a load/insert/flush/load cycle when
	// internSymbol is called with the same strings in the same order.
	// Mixing symbol-id namespaces across runs is the caller's
	// responsibility -- intern up front, before any insert/lookup.
	void load(const std::filesystem::path& dir);

	// Insert a function's compiled output. Replaces any existing entry
	// for the same fingerprint. The new entry is dirty until flush().
	void insert(const FunctionKey& key, const EmittedCode& emitted);

	// Write all entries + symbol table to disk atomically (.tmp + fsync
	// + rename for both files). No-op if not attached to a directory.
	// Returns true on success, false if any I/O step failed (in which
	// case the on-disk cache is left as it was before flush()).
	bool flush();

	// Look up an entry by FunctionKey. On hit, applies relocs in place
	// using `resolver` and returns the patched host bytes via `out`.
	// Returns false on miss or if any reloc resolved to 0 / unknown kind.
	bool lookup(const FunctionKey& key, const Resolver& resolver, EmittedCode& out) const;

	// Lookup by fingerprint directly (skips re-hashing). Same return
	// semantics as the FunctionKey overload.
	bool lookup(const Fingerprint& fp, const Resolver& resolver, EmittedCode& out) const;

	// Number of resident entries.
	size_t entryCount() const noexcept;

	// Interned string id for a RuntimeSymbol target. Empty string is
	// reserved and returns 0. Repeated calls with the same name return
	// the same id.
	uint64_t internSymbol(const std::string& name);
	// Reverse mapping for diagnostics and the Resolver implementation.
	// Returns nullptr if id is unknown or 0.
	const std::string* symbolName(uint64_t id) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Apply a list of relocs to a mutable host code buffer in place. Returns
// false if any reloc resolves to 0 (RuntimeSymbol / PpcCodeAddr) or names
// an unknown kind. Exposed in the public API so producers can validate
// their reloc lists against a stub resolver during development.
bool applyRelocs(uint8_t* hostBytes, size_t hostSize,
                 const Reloc* relocs, size_t relocCount,
                 const Resolver& resolver);

} // namespace jitcache
