// Standalone unit tests for the JitCache library.
//
// Build + run (from the repo root, requires system xxhash):
//   g++ -std=c++20 -Wall -Wextra -O2
//       -Isrc/Common/JitCache
//       src/Common/JitCache/JitCache.cpp
//       src/Common/JitCache/jitcache_test.cpp
//       -lxxhash -o /tmp/jitcache_test
//   /tmp/jitcache_test
//
// Exit status 0 = all tests passed.

#include "JitCache.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#if defined(_WIN32)
	#include <process.h>
	#define jc_test_getpid() (static_cast<int>(_getpid()))
#else
	#include <unistd.h>
	#define jc_test_getpid() (static_cast<int>(getpid()))
#endif

namespace
{

int g_failures = 0;
int g_assertions = 0;

#define CHECK(cond)                                                      \
	do                                                                   \
	{                                                                    \
		++g_assertions;                                                  \
		if (!(cond))                                                     \
		{                                                                \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,           \
			             __LINE__, #cond);                               \
			++g_failures;                                                \
		}                                                                \
	} while (0)

// ---- Resolver test doubles ----------------------------------------------

class NullResolver : public jitcache::Resolver
{
public:
	uint64_t resolveRuntimeSymbol(uint64_t) const override { return 0; }
	uint64_t resolvePpcCodeAddr(uint64_t) const override { return 0; }
};

class MapResolver : public jitcache::Resolver
{
public:
	std::unordered_map<uint64_t, uint64_t> symbols;
	std::unordered_map<uint64_t, uint64_t> ppcAddrs;

	uint64_t resolveRuntimeSymbol(uint64_t id) const override
	{
		auto it = symbols.find(id);
		return it == symbols.end() ? 0 : it->second;
	}
	uint64_t resolvePpcCodeAddr(uint64_t addr) const override
	{
		auto it = ppcAddrs.find(addr);
		return it == ppcAddrs.end() ? 0 : it->second;
	}
};

// ---- helpers ------------------------------------------------------------

// Build a key whose ppcBytes pointer remains stable for the lifetime of the
// returned pair. The vector is moved into a local on the caller side.
struct KeyBuf
{
	std::vector<uint8_t> ppc;
	jitcache::FunctionKey key{};
	void bind()
	{
		key.ppcBytes = ppc.data();
		key.ppcLen = static_cast<uint32_t>(ppc.size());
	}
};

// Decode the 16-bit imm field from one AArch64 mov-wide instruction word.
uint16_t decodeImm16(const uint8_t* code)
{
	uint32_t insn;
	std::memcpy(&insn, code, sizeof(insn));
	return static_cast<uint16_t>((insn >> 5) & 0xFFFF);
}

// Decode the full 64-bit value materialized by a 4-instruction movz/movk
// sequence laid out at `code`.
uint64_t decodeAbs64(const uint8_t* code)
{
	uint64_t v = 0;
	for (int i = 0; i < 4; ++i)
		v |= static_cast<uint64_t>(decodeImm16(code + i * 4)) << (i * 16);
	return v;
}

// ---- tests --------------------------------------------------------------

void test_fingerprint_deterministic()
{
	jitcache::Cache cache;
	KeyBuf a{{0x01, 0x02, 0x03, 0x04}, {}};
	a.key.codegenVersion = 7;
	a.key.moduleId = 0xDEADBEEFCAFEBABEULL;
	a.key.ppcEntryAddr = 0x02000000;
	a.bind();

	KeyBuf b = a;
	b.bind(); // re-anchor ppcBytes to b's own copy of the vector

	auto fpA = cache.fingerprint(a.key);
	auto fpB = cache.fingerprint(b.key);
	CHECK(fpA == fpB);
	CHECK(fpA.lo != 0 || fpA.hi != 0);
}

void test_fingerprint_differentiates_each_field()
{
	jitcache::Cache cache;
	KeyBuf base{{0x01, 0x02, 0x03, 0x04}, {}};
	base.key.codegenVersion = 1;
	base.key.hostCpuFeatureBits = 0;
	base.key.moduleId = 0xAAAAAAAAAAAAAAAAULL;
	base.key.ppcEntryAddr = 0x10000000;
	base.bind();
	auto fpBase = cache.fingerprint(base.key);

	auto perturb = [&](auto mutate) {
		KeyBuf k = base;
		k.bind();
		mutate(k.key);
		// rebind in case mutate touched ppc bytes / ppcLen
		k.key.ppcBytes = k.ppc.data();
		k.key.ppcLen = static_cast<uint32_t>(k.ppc.size());
		return cache.fingerprint(k.key);
	};

	CHECK(fpBase != perturb([](auto& k) { k.codegenVersion++; }));
	CHECK(fpBase != perturb([](auto& k) { k.hostCpuFeatureBits++; }));
	CHECK(fpBase != perturb([](auto& k) { k.moduleId ^= 1; }));
	CHECK(fpBase != perturb([](auto& k) { k.ppcEntryAddr++; }));
	// Mutating one byte of ppcBytes
	{
		KeyBuf k = base;
		k.ppc[2] ^= 0x80;
		k.bind();
		CHECK(fpBase != cache.fingerprint(k.key));
	}
	// Differing length (truncate) but same prefix
	{
		KeyBuf k = base;
		k.ppc.resize(2);
		k.bind();
		CHECK(fpBase != cache.fingerprint(k.key));
	}
}

void test_insert_lookup_roundtrip_no_relocs()
{
	jitcache::Cache cache;
	KeyBuf k{{0xDE, 0xAD, 0xBE, 0xEF}, {}};
	k.key.codegenVersion = 1;
	k.key.ppcEntryAddr = 0x02123456;
	k.bind();

	jitcache::EmittedCode emitted;
	emitted.hostBytes = {0x11, 0x22, 0x33, 0x44, 0x55};
	cache.insert(k.key, emitted);
	CHECK(cache.entryCount() == 1);

	NullResolver r;
	jitcache::EmittedCode out;
	CHECK(cache.lookup(k.key, r, out));
	CHECK(out.hostBytes == emitted.hostBytes);
	CHECK(out.relocs.empty());
}

void test_lookup_miss()
{
	jitcache::Cache cache;
	KeyBuf k{{0x00}, {}};
	k.bind();
	NullResolver r;
	jitcache::EmittedCode out;
	CHECK(!cache.lookup(k.key, r, out));
}

void test_reloc_embedded_value_patches_movz_movk_sequence()
{
	// Construct a 16-byte buffer with four "movz/movk imm=0" instructions
	// (Rd / opc / hw / sf bits are arbitrary; only imm16 is patched).
	std::vector<uint8_t> code = {
	    0x09, 0x00, 0x80, 0xD2, // movz x9, #0, lsl 0
	    0x09, 0x00, 0xA0, 0xF2, // movk x9, #0, lsl 16
	    0x09, 0x00, 0xC0, 0xF2, // movk x9, #0, lsl 32
	    0x09, 0x00, 0xE0, 0xF2, // movk x9, #0, lsl 48
	};
	CHECK(decodeAbs64(code.data()) == 0);

	const uint64_t kTarget = 0x123456789ABCDEF0ULL;
	jitcache::Reloc r{};
	r.codeOffset = 0;
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::EmbeddedValue;
	r.targetId = kTarget;

	NullResolver resolver;
	CHECK(jitcache::applyRelocs(code.data(), code.size(), &r, 1, resolver));
	CHECK(decodeAbs64(code.data()) == kTarget);
}

void test_reloc_runtime_symbol_resolves_and_patches()
{
	std::vector<uint8_t> code(16, 0); // start from arbitrary bytes
	// Manually seed with movz/movk so decodeAbs64 yields zero initially.
	uint32_t ops[4] = {0xD2800009, 0xF2A00009, 0xF2C00009, 0xF2E00009};
	std::memcpy(code.data(), ops, sizeof(ops));
	CHECK(decodeAbs64(code.data()) == 0);

	jitcache::Cache cache;
	uint64_t sym = cache.internSymbol("OSWakeOneSender");
	CHECK(sym != 0);

	MapResolver resolver;
	const uint64_t kHostAddr = 0x0000007396C45660ULL;
	resolver.symbols[sym] = kHostAddr;

	jitcache::Reloc r{};
	r.codeOffset = 0;
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::RuntimeSymbol;
	r.targetId = sym;
	CHECK(jitcache::applyRelocs(code.data(), code.size(), &r, 1, resolver));
	CHECK(decodeAbs64(code.data()) == kHostAddr);
}

void test_reloc_runtime_symbol_unknown_returns_false()
{
	std::vector<uint8_t> code(16, 0);
	uint32_t ops[4] = {0xD2800009, 0xF2A00009, 0xF2C00009, 0xF2E00009};
	std::memcpy(code.data(), ops, sizeof(ops));

	jitcache::Reloc r{};
	r.codeOffset = 0;
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::RuntimeSymbol;
	r.targetId = 42; // never interned

	NullResolver resolver; // returns 0 for everything
	CHECK(!jitcache::applyRelocs(code.data(), code.size(), &r, 1, resolver));
}

void test_reloc_codeOffset_bounds_check()
{
	std::vector<uint8_t> code(16, 0);
	jitcache::Reloc r{};
	r.codeOffset = 1; // would overrun: 1 + 16 = 17 > 16
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::EmbeddedValue;
	r.targetId = 0xDEADBEEFULL;

	NullResolver resolver;
	CHECK(!jitcache::applyRelocs(code.data(), code.size(), &r, 1, resolver));
}

void test_symbol_interning()
{
	jitcache::Cache cache;
	CHECK(cache.internSymbol("") == 0);
	CHECK(cache.symbolName(0) == nullptr);
	CHECK(cache.symbolName(99999) == nullptr);

	uint64_t a1 = cache.internSymbol("foo");
	uint64_t a2 = cache.internSymbol("foo");
	uint64_t b = cache.internSymbol("bar");
	CHECK(a1 == a2);
	CHECK(a1 != b);
	CHECK(*cache.symbolName(a1) == "foo");
	CHECK(*cache.symbolName(b) == "bar");
}

// ---- persistence helpers ------------------------------------------------

namespace fs = std::filesystem;

fs::path makeTempCacheDir()
{
	// Use a process-id+counter-derived subdir so concurrent test runs
	// don't trample each other. Deletes any previous contents.
	static std::atomic<unsigned> counter{0};
	unsigned id = counter.fetch_add(1, std::memory_order_relaxed);
	fs::path p = fs::temp_directory_path() / ("jitcache_test_" + std::to_string(jc_test_getpid()) + "_" + std::to_string(id));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

// Helper: build an EmittedCode with a 4-insn movz/movk sequence + one
// EmbeddedValue reloc targeting `target`. Used by persistence tests.
jitcache::EmittedCode makeEmbeddedValueEntry(uint64_t target)
{
	jitcache::EmittedCode e;
	e.hostBytes.resize(16, 0);
	uint32_t ops[4] = {0xD2800009, 0xF2A00009, 0xF2C00009, 0xF2E00009};
	std::memcpy(e.hostBytes.data(), ops, sizeof(ops));
	jitcache::Reloc r{};
	r.codeOffset = 0;
	r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r.targetKind = jitcache::TargetKind::EmbeddedValue;
	r.targetId = target;
	e.relocs.push_back(r);
	return e;
}

void test_persistence_roundtrip_basic()
{
	fs::path dir = makeTempCacheDir();

	std::vector<uint8_t> ppc1 = {0x60, 0x00, 0x00, 0x00};
	std::vector<uint8_t> ppc2 = {0x4E, 0x80, 0x00, 0x20};
	std::vector<uint8_t> ppc3 = {0x38, 0x60, 0x00, 0x00, 0x4E, 0x80, 0x00, 0x20};

	jitcache::FunctionKey k1{}, k2{}, k3{};
	k1.codegenVersion = 1; k1.moduleId = 0xAAAA; k1.ppcEntryAddr = 0x100;
	k1.ppcBytes = ppc1.data(); k1.ppcLen = ppc1.size();
	k2.codegenVersion = 1; k2.moduleId = 0xAAAA; k2.ppcEntryAddr = 0x200;
	k2.ppcBytes = ppc2.data(); k2.ppcLen = ppc2.size();
	k3.codegenVersion = 1; k3.moduleId = 0xAAAA; k3.ppcEntryAddr = 0x300;
	k3.ppcBytes = ppc3.data(); k3.ppcLen = ppc3.size();

	{
		jitcache::Cache c;
		c.load(dir);
		CHECK(c.entryCount() == 0);
		c.insert(k1, makeEmbeddedValueEntry(0x1111111122222222ULL));
		c.insert(k2, makeEmbeddedValueEntry(0x3333333344444444ULL));
		c.insert(k3, makeEmbeddedValueEntry(0x5555555566666666ULL));
		CHECK(c.flush());
	}

	// Fresh Cache reading the same dir should see all three entries.
	jitcache::Cache c2;
	c2.load(dir);
	CHECK(c2.entryCount() == 3);

	NullResolver r;
	jitcache::EmittedCode out;
	CHECK(c2.lookup(k1, r, out));
	CHECK(decodeAbs64(out.hostBytes.data()) == 0x1111111122222222ULL);
	CHECK(c2.lookup(k2, r, out));
	CHECK(decodeAbs64(out.hostBytes.data()) == 0x3333333344444444ULL);
	CHECK(c2.lookup(k3, r, out));
	CHECK(decodeAbs64(out.hostBytes.data()) == 0x5555555566666666ULL);

	fs::remove_all(dir);
}

void test_persistence_symbol_table_roundtrip()
{
	fs::path dir = makeTempCacheDir();

	std::vector<uint8_t> ppc = {0x60, 0x00, 0x00, 0x00};
	jitcache::FunctionKey k{};
	k.codegenVersion = 1; k.moduleId = 0xBEEF; k.ppcEntryAddr = 0x1000;
	k.ppcBytes = ppc.data(); k.ppcLen = ppc.size();

	uint64_t symA = 0, symB = 0;
	{
		jitcache::Cache c;
		c.load(dir);
		symA = c.internSymbol("OSWakeOneSender");
		symB = c.internSymbol("OSWakeOneReceiver");
		CHECK(symA != 0);
		CHECK(symB != 0);
		CHECK(symA != symB);

		jitcache::EmittedCode e;
		e.hostBytes.resize(16, 0);
		uint32_t ops[4] = {0xD2800009, 0xF2A00009, 0xF2C00009, 0xF2E00009};
		std::memcpy(e.hostBytes.data(), ops, sizeof(ops));
		jitcache::Reloc r{};
		r.codeOffset = 0;
		r.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
		r.targetKind = jitcache::TargetKind::RuntimeSymbol;
		r.targetId = symA;
		e.relocs.push_back(r);

		c.insert(k, e);
		CHECK(c.flush());
	}

	jitcache::Cache c2;
	c2.load(dir);

	// Symbol ids should round-trip; their string contents must match.
	const std::string* nameA = c2.symbolName(symA);
	const std::string* nameB = c2.symbolName(symB);
	CHECK(nameA != nullptr);
	CHECK(nameB != nullptr);
	CHECK(*nameA == "OSWakeOneSender");
	CHECK(*nameB == "OSWakeOneReceiver");

	// Re-interning the same names returns the same ids.
	CHECK(c2.internSymbol("OSWakeOneSender") == symA);
	CHECK(c2.internSymbol("OSWakeOneReceiver") == symB);

	// Lookup with a resolver that maps symA to a known host pointer.
	MapResolver resolver;
	const uint64_t kHostAddr = 0xCAFEBABEDEADBEEFULL;
	resolver.symbols[symA] = kHostAddr;
	jitcache::EmittedCode out;
	CHECK(c2.lookup(k, resolver, out));
	CHECK(decodeAbs64(out.hostBytes.data()) == kHostAddr);

	fs::remove_all(dir);
}

void test_persistence_version_mismatch_wipes()
{
	fs::path dir = makeTempCacheDir();

	std::vector<uint8_t> ppc = {0x60, 0x00, 0x00, 0x00};
	jitcache::FunctionKey k{};
	k.codegenVersion = 1; k.moduleId = 0xC0DE; k.ppcEntryAddr = 0x2000;
	k.ppcBytes = ppc.data(); k.ppcLen = ppc.size();

	{
		jitcache::Cache c;
		c.load(dir);
		c.insert(k, makeEmbeddedValueEntry(0x7777777788888888ULL));
		CHECK(c.flush());
	}
	CHECK(fs::exists(dir / "manifest.bin"));
	CHECK(fs::exists(dir / "code.bin"));

	// Corrupt the manifest version field. The on-disk layout puts
	// cacheFormatVersion at byte offset 8 (right after the 8-byte magic).
	{
		std::fstream f(dir / "manifest.bin", std::ios::binary | std::ios::in | std::ios::out);
		CHECK(f.is_open());
		f.seekp(8);
		uint32_t bogus = 0xDEADBEEF;
		f.write(reinterpret_cast<const char*>(&bogus), sizeof(bogus));
	}

	jitcache::Cache c2;
	c2.load(dir);
	CHECK(c2.entryCount() == 0);
	CHECK(!fs::exists(dir / "manifest.bin"));
	CHECK(!fs::exists(dir / "code.bin"));

	fs::remove_all(dir);
}

void test_persistence_bad_magic_wipes()
{
	fs::path dir = makeTempCacheDir();

	std::vector<uint8_t> ppc = {0x60, 0x00, 0x00, 0x00};
	jitcache::FunctionKey k{};
	k.codegenVersion = 1; k.moduleId = 0xC0DE; k.ppcEntryAddr = 0x3000;
	k.ppcBytes = ppc.data(); k.ppcLen = ppc.size();

	{
		jitcache::Cache c;
		c.load(dir);
		c.insert(k, makeEmbeddedValueEntry(0xAAAAAAAA00000000ULL));
		CHECK(c.flush());
	}
	{
		std::fstream f(dir / "manifest.bin", std::ios::binary | std::ios::in | std::ios::out);
		CHECK(f.is_open());
		f.seekp(0);
		const char bogus[8] = {'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X'};
		f.write(bogus, sizeof(bogus));
	}

	jitcache::Cache c2;
	c2.load(dir);
	CHECK(c2.entryCount() == 0);
	CHECK(!fs::exists(dir / "manifest.bin"));

	fs::remove_all(dir);
}

void test_persistence_orphan_file_wipes()
{
	// If only manifest.bin or only code.bin exists, treat that as a
	// half-written state and wipe.
	fs::path dir = makeTempCacheDir();
	{
		std::ofstream f(dir / "manifest.bin", std::ios::binary);
		f << "anything";
	}
	jitcache::Cache c;
	c.load(dir);
	CHECK(c.entryCount() == 0);
	CHECK(!fs::exists(dir / "manifest.bin"));
	CHECK(!fs::exists(dir / "code.bin"));
	fs::remove_all(dir);
}

void test_persistence_flush_is_deterministic()
{
	fs::path dirA = makeTempCacheDir();
	fs::path dirB = makeTempCacheDir();

	std::vector<uint8_t> ppc1 = {0x60, 0x00, 0x00, 0x00};
	std::vector<uint8_t> ppc2 = {0x4E, 0x80, 0x00, 0x20};

	jitcache::FunctionKey k1{}, k2{};
	k1.codegenVersion = 1; k1.moduleId = 0xDEAD; k1.ppcEntryAddr = 0x100;
	k1.ppcBytes = ppc1.data(); k1.ppcLen = ppc1.size();
	k2.codegenVersion = 1; k2.moduleId = 0xDEAD; k2.ppcEntryAddr = 0x200;
	k2.ppcBytes = ppc2.data(); k2.ppcLen = ppc2.size();

	{
		jitcache::Cache a;
		a.load(dirA);
		a.insert(k1, makeEmbeddedValueEntry(0x1ULL));
		a.insert(k2, makeEmbeddedValueEntry(0x2ULL));
		CHECK(a.flush());
	}
	{
		jitcache::Cache b;
		b.load(dirB);
		b.insert(k2, makeEmbeddedValueEntry(0x2ULL)); // reverse insert order
		b.insert(k1, makeEmbeddedValueEntry(0x1ULL));
		CHECK(b.flush());
	}

	auto readBytes = [](const fs::path& p) {
		std::ifstream f(p, std::ios::binary);
		return std::vector<char>(std::istreambuf_iterator<char>(f), {});
	};
	auto manA = readBytes(dirA / "manifest.bin");
	auto manB = readBytes(dirB / "manifest.bin");
	auto codA = readBytes(dirA / "code.bin");
	auto codB = readBytes(dirB / "code.bin");
	CHECK(manA == manB);
	CHECK(codA == codB);

	fs::remove_all(dirA);
	fs::remove_all(dirB);
}

void test_entry_points_roundtrip_memory_only()
{
	jitcache::Cache cache;
	KeyBuf k{{0x12, 0x34}, {}};
	k.key.codegenVersion = 1;
	k.key.moduleId = 0xEEEE;
	k.bind();

	jitcache::EmittedCode e;
	e.hostBytes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
	e.entryPoints = {
	    {0x02000000, 0},
	    {0x02000010, 4},
	    {0x02000020, 8},
	};
	cache.insert(k.key, e);

	NullResolver r;
	jitcache::EmittedCode out;
	CHECK(cache.lookup(k.key, r, out));
	CHECK(out.entryPoints.size() == 3);
	CHECK(out.entryPoints[0].ppcAddr == 0x02000000);
	CHECK(out.entryPoints[0].hostOffset == 0);
	CHECK(out.entryPoints[1].ppcAddr == 0x02000010);
	CHECK(out.entryPoints[1].hostOffset == 4);
	CHECK(out.entryPoints[2].ppcAddr == 0x02000020);
	CHECK(out.entryPoints[2].hostOffset == 8);
}

void test_entry_points_persistence_roundtrip()
{
	fs::path dir = makeTempCacheDir();

	std::vector<uint8_t> ppc1 = {0x60, 0x00, 0x00, 0x00};
	std::vector<uint8_t> ppc2 = {0x4E, 0x80, 0x00, 0x20};
	jitcache::FunctionKey k1{}, k2{};
	k1.codegenVersion = 1; k1.moduleId = 0xCAFE; k1.ppcEntryAddr = 0x1000;
	k1.ppcBytes = ppc1.data(); k1.ppcLen = ppc1.size();
	k2.codegenVersion = 1; k2.moduleId = 0xCAFE; k2.ppcEntryAddr = 0x2000;
	k2.ppcBytes = ppc2.data(); k2.ppcLen = ppc2.size();

	{
		jitcache::Cache c;
		c.load(dir);
		jitcache::EmittedCode e1;
		e1.hostBytes = {0xAA, 0xBB, 0xCC, 0xDD};
		e1.entryPoints = { {0x1000, 0}, {0x1020, 4} };
		c.insert(k1, e1);

		jitcache::EmittedCode e2;
		e2.hostBytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
		// e2 deliberately has no entry points -- the unenterable bookkeeping case
		c.insert(k2, e2);
		CHECK(c.flush());
	}

	jitcache::Cache c2;
	c2.load(dir);
	NullResolver r;
	jitcache::EmittedCode out;
	CHECK(c2.lookup(k1, r, out));
	CHECK(out.entryPoints.size() == 2);
	CHECK(out.entryPoints[0].ppcAddr == 0x1000);
	CHECK(out.entryPoints[1].hostOffset == 4);

	CHECK(c2.lookup(k2, r, out));
	CHECK(out.entryPoints.empty());

	fs::remove_all(dir);
}

void test_insert_lookup_with_relocs_full_pipeline()
{
	jitcache::Cache cache;
	uint64_t symId = cache.internSymbol("PPCRecompiler_virtualHLE");

	// 32 bytes: 4-insn movz/movk at offset 0, then 4 more at offset 16.
	std::vector<uint8_t> code(32, 0);
	uint32_t ops[8] = {
	    0xD2800009, 0xF2A00009, 0xF2C00009, 0xF2E00009, // movz/movk x9
	    0xD280000A, 0xF2A0000A, 0xF2C0000A, 0xF2E0000A, // movz/movk x10
	};
	std::memcpy(code.data(), ops, sizeof(ops));

	jitcache::EmittedCode emitted;
	emitted.hostBytes = code;
	jitcache::Reloc r0{};
	r0.codeOffset = 0;
	r0.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r0.targetKind = jitcache::TargetKind::RuntimeSymbol;
	r0.targetId = symId;
	jitcache::Reloc r1{};
	r1.codeOffset = 16;
	r1.kind = jitcache::RelocKind::Aarch64_MovzMovk_Abs64;
	r1.targetKind = jitcache::TargetKind::EmbeddedValue;
	r1.targetId = 0xCAFEF00DBAADF00DULL;
	emitted.relocs = {r0, r1};

	KeyBuf k{{0x60, 0x00, 0x00, 0x00}, {}};
	k.key.codegenVersion = 1;
	k.key.moduleId = 0xABCDEF;
	k.key.ppcEntryAddr = 0x02000100;
	k.bind();

	cache.insert(k.key, emitted);

	MapResolver resolver;
	const uint64_t kHostHelperAddr = 0x00000073970F4F10ULL;
	resolver.symbols[symId] = kHostHelperAddr;

	jitcache::EmittedCode out;
	CHECK(cache.lookup(k.key, resolver, out));
	CHECK(decodeAbs64(out.hostBytes.data() + 0) == kHostHelperAddr);
	CHECK(decodeAbs64(out.hostBytes.data() + 16) == 0xCAFEF00DBAADF00DULL);

	// The stored entry should be untouched (we copied before patching).
	jitcache::EmittedCode out2;
	resolver.symbols[symId] = 0xDEADBEEF00000000ULL; // pretend resolution changed
	CHECK(cache.lookup(k.key, resolver, out2));
	CHECK(decodeAbs64(out2.hostBytes.data() + 0) == 0xDEADBEEF00000000ULL);
	CHECK(decodeAbs64(out2.hostBytes.data() + 16) == 0xCAFEF00DBAADF00DULL);
}

} // namespace

int main()
{
	test_fingerprint_deterministic();
	test_fingerprint_differentiates_each_field();
	test_insert_lookup_roundtrip_no_relocs();
	test_lookup_miss();
	test_reloc_embedded_value_patches_movz_movk_sequence();
	test_reloc_runtime_symbol_resolves_and_patches();
	test_reloc_runtime_symbol_unknown_returns_false();
	test_reloc_codeOffset_bounds_check();
	test_symbol_interning();
	test_insert_lookup_with_relocs_full_pipeline();

	test_persistence_roundtrip_basic();
	test_persistence_symbol_table_roundtrip();
	test_persistence_version_mismatch_wipes();
	test_persistence_bad_magic_wipes();
	test_persistence_orphan_file_wipes();
	test_persistence_flush_is_deterministic();

	test_entry_points_roundtrip_memory_only();
	test_entry_points_persistence_roundtrip();

	std::printf("jitcache_test: %d assertions, %d failures\n",
	            g_assertions, g_failures);
	return g_failures == 0 ? 0 : 1;
}
