#include "JitCache.h"

#include <cstring>
#include <unordered_map>

namespace jitcache
{

namespace
{

// FNV-1a 128. Placeholder; Phase 1b swaps in XXH3-128 from xxhash and bumps
// kCacheFormatVersion. Chosen now only because it has no external deps and
// is straightforward to verify by hand against the spec.
//
// offset_basis = 0x6c62272e07bb014262b821756295c58d
// prime        = 0x0000000001000000000000000000013b
struct Fnv1a128
{
	uint64_t lo = 0x62b821756295c58dULL;
	uint64_t hi = 0x6c62272e07bb0142ULL;

	void update(const void* data, size_t len) noexcept
	{
		auto* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < len; ++i)
		{
			lo ^= static_cast<uint64_t>(p[i]);
			mulPrime();
		}
	}

	// (hi:lo) *= prime, where prime = (0x01000000 << 64) | 0x13B.
	// Expanded:
	//   new_lo = lo * 0x13B
	//   new_hi = hi * 0x13B + lo * 0x01000000 (the contribution that crosses
	//            the boundary; the high-half multiplications above bit 96
	//            wrap and vanish modulo 2^128).
	// We also need the carry from lo * 0x13B's high 64 bits into hi.
	void mulPrime() noexcept
	{
		constexpr uint64_t LOW_PRIME = 0x13BULL;
		// lo * LOW_PRIME -> 128-bit intermediate (carry_lo : new_lo)
		uint64_t new_lo;
		uint64_t carry_lo;
		mul64(lo, LOW_PRIME, new_lo, carry_lo);
		// hi * LOW_PRIME contributes to the new hi; we don't need its high
		// half because anything above bit 128 is discarded modulo 2^128.
		uint64_t hi_low_contrib = hi * LOW_PRIME;
		// lo * (1 << 88) contributes lo's bits to hi (specifically, lo's
		// low 40 bits land in hi[63:24]). Bits above that wrap past 2^128.
		uint64_t lo_high_contrib = lo << 24;
		hi = hi_low_contrib + carry_lo + lo_high_contrib;
		lo = new_lo;
	}

	// 64x64 -> 128 unsigned multiply, returning (low, high).
	static void mul64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) noexcept
	{
#if defined(__SIZEOF_INT128__)
		__uint128_t r = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
		lo = static_cast<uint64_t>(r);
		hi = static_cast<uint64_t>(r >> 64);
#else
		uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32;
		uint64_t b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
		uint64_t ll = a0 * b0;
		uint64_t lh = a0 * b1;
		uint64_t hl = a1 * b0;
		uint64_t hh = a1 * b1;
		uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);
		lo = (ll & 0xFFFFFFFFULL) | (mid << 32);
		hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
	}
};

struct FingerprintHash
{
	size_t operator()(const Fingerprint& fp) const noexcept
	{
		// XOR-fold the 128-bit fingerprint into a size_t. The fingerprint
		// is already well-distributed; further mixing here adds nothing.
		return static_cast<size_t>(fp.lo ^ fp.hi);
	}
};

// AArch64 move-wide immediate field layout, applied to a uint32 instruction:
//   bits [31]     sf  (1 = 64-bit)
//   bits [30:29]  opc (10 = movz, 11 = movk)
//   bits [28:23]  100101
//   bits [22:21]  hw  (shift = hw*16)
//   bits [20:5]   imm16
//   bits [4:0]    Rd
// Patching imm16 = clear bits 20..5, OR in the new 16-bit immediate << 5.
constexpr uint32_t kMovWideImm16Mask = 0x001FFFE0u;

void patchMovzMovkAbs64(uint8_t* code, uint64_t value) noexcept
{
	for (int chunk = 0; chunk < 4; ++chunk)
	{
		uint32_t insn;
		std::memcpy(&insn, code + chunk * 4, sizeof(insn));
		uint16_t imm16 = static_cast<uint16_t>(value >> (chunk * 16));
		insn = (insn & ~kMovWideImm16Mask) | (static_cast<uint32_t>(imm16) << 5);
		std::memcpy(code + chunk * 4, &insn, sizeof(insn));
	}
}

} // namespace

bool applyRelocs(uint8_t* hostBytes, size_t hostSize,
                 const Reloc* relocs, size_t relocCount,
                 const Resolver& resolver)
{
	for (size_t i = 0; i < relocCount; ++i)
	{
		const Reloc& r = relocs[i];
		uint64_t value = 0;
		switch (r.targetKind)
		{
		case TargetKind::RuntimeSymbol:
			value = resolver.resolveRuntimeSymbol(r.targetId);
			if (value == 0)
				return false;
			break;
		case TargetKind::EmbeddedValue:
			value = r.targetId;
			break;
		case TargetKind::PpcCodeAddr:
			value = resolver.resolvePpcCodeAddr(r.targetId);
			if (value == 0)
				return false;
			break;
		default:
			return false;
		}

		switch (r.kind)
		{
		case RelocKind::Aarch64_MovzMovk_Abs64:
			// 4 instructions x 4 bytes; codeOffset is the first movz.
			if (static_cast<size_t>(r.codeOffset) + 16 > hostSize)
				return false;
			patchMovzMovkAbs64(hostBytes + r.codeOffset, value);
			break;
		default:
			return false;
		}
	}
	return true;
}

struct Cache::Impl
{
	std::unordered_map<Fingerprint, EmittedCode, FingerprintHash> entries;
	std::vector<std::string> symbolNames; // index 0 reserved (empty)
	std::unordered_map<std::string, uint64_t> symbolIds;

	Impl()
	{
		symbolNames.emplace_back(); // id 0 -> ""
	}
};

Cache::Cache() : m_impl(std::make_unique<Impl>()) {}
Cache::~Cache() = default;

Fingerprint Cache::fingerprint(const FunctionKey& key) const
{
	Fnv1a128 h;
	// Magic constant ties the fingerprint format to this library version.
	// Bumping kCacheFormatVersion only is not enough -- if the fingerprint
	// algorithm itself changes, change this string too so old fingerprints
	// from the old algo can't match by accident.
	static constexpr char kMagic[] = "jitcache-v1";
	h.update(kMagic, sizeof(kMagic) - 1);
	h.update(&key.codegenVersion, sizeof(key.codegenVersion));
	h.update(&key.hostCpuFeatureBits, sizeof(key.hostCpuFeatureBits));
	h.update(&key.moduleId, sizeof(key.moduleId));
	h.update(&key.ppcEntryAddr, sizeof(key.ppcEntryAddr));
	h.update(&key.ppcLen, sizeof(key.ppcLen));
	if (key.ppcBytes && key.ppcLen)
		h.update(key.ppcBytes, key.ppcLen);
	return Fingerprint{h.lo, h.hi};
}

void Cache::insert(const FunctionKey& key, const EmittedCode& emitted)
{
	const Fingerprint fp = fingerprint(key);
	m_impl->entries[fp] = emitted;
}

bool Cache::lookup(const FunctionKey& key, const Resolver& resolver, EmittedCode& out) const
{
	return lookup(fingerprint(key), resolver, out);
}

bool Cache::lookup(const Fingerprint& fp, const Resolver& resolver, EmittedCode& out) const
{
	auto it = m_impl->entries.find(fp);
	if (it == m_impl->entries.end())
		return false;
	out = it->second; // copy: we mutate hostBytes during reloc application
	if (!applyRelocs(out.hostBytes.data(), out.hostBytes.size(),
	                 out.relocs.data(), out.relocs.size(),
	                 resolver))
	{
		return false;
	}
	return true;
}

size_t Cache::entryCount() const noexcept
{
	return m_impl->entries.size();
}

uint64_t Cache::internSymbol(const std::string& name)
{
	if (name.empty())
		return 0;
	auto it = m_impl->symbolIds.find(name);
	if (it != m_impl->symbolIds.end())
		return it->second;
	uint64_t id = m_impl->symbolNames.size();
	m_impl->symbolNames.push_back(name);
	m_impl->symbolIds.emplace(name, id);
	return id;
}

const std::string* Cache::symbolName(uint64_t id) const
{
	if (id == 0 || id >= m_impl->symbolNames.size())
		return nullptr;
	return &m_impl->symbolNames[id];
}

} // namespace jitcache
