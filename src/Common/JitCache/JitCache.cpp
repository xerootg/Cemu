#include "JitCache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <xxhash.h>

#if defined(_WIN32)
	#include <io.h>
	#define jc_fileno _fileno
	#define jc_fsync(fd) _commit(fd)
#else
	#include <unistd.h>
	#define jc_fileno fileno
	#define jc_fsync(fd) fsync(fd)
#endif

// Local fs alias -- mirror of the one in Cemu's precompiled.h, but defined
// here too so the library is buildable standalone (no Cemu precompiled
// header required).
namespace fs = std::filesystem;

namespace jitcache
{

namespace
{

// --- On-disk layout -------------------------------------------------------
//
// Two files under the attached directory: manifest.bin and code.bin.
//
// manifest.bin:
//   ManifestHeader
//   FunctionRecord[entryCount]           (sorted by fingerprint)
//   symbol table:
//     for each symbol: uint32 nameLen, char name[nameLen]
//   (symbol id 0 is implicit and not stored.)
//
// code.bin: concatenation of, per entry (in the same order as manifest):
//   uint8 hostBytes[codeSize]
//   Reloc  relocs[relocCount]
//
// Both files are written atomically: write to .tmp, fsync, rename. The
// rename targets are stable so a partial state on disk is never readable
// by a subsequent load.
//
// Bumping kCacheFormatVersion (or its prefix string below) invalidates
// every on-disk cache.

constexpr char kManifestMagic[8] = {'J', 'I', 'T', 'C', 'A', 'C', 'H', '1'};

struct ManifestHeader
{
	char magic[8];
	uint32_t cacheFormatVersion;
	uint32_t reserved;
	uint64_t entryCount;
	uint64_t symbolCount;
	uint64_t symbolTableOffset; // byte offset within manifest.bin
	uint64_t symbolTableSize;   // bytes
	uint64_t codeBinSize;       // expected size of code.bin in bytes
};
static_assert(sizeof(ManifestHeader) == 56, "ManifestHeader layout is part of the on-disk format");

struct FunctionRecord
{
	Fingerprint fingerprint;   // 16 bytes
	uint64_t codeOffset;       // offset within code.bin
	uint64_t codeSize;
	uint64_t relocOffset;      // offset within code.bin (immediately after code)
	uint32_t relocCount;
	uint32_t entryPointCount;
	uint64_t entryPointOffset; // offset within code.bin (immediately after relocs)
};
static_assert(sizeof(FunctionRecord) == 56, "FunctionRecord layout is part of the on-disk format");

// --- AArch64 movz/movk patching ------------------------------------------

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

// --- Fingerprint hash ----------------------------------------------------

struct XXH3StateGuard
{
	XXH3_state_t* s;
	XXH3StateGuard() : s(XXH3_createState()) {}
	~XXH3StateGuard()
	{
		if (s)
			XXH3_freeState(s);
	}
	XXH3StateGuard(const XXH3StateGuard&) = delete;
	XXH3StateGuard& operator=(const XXH3StateGuard&) = delete;
};

struct FingerprintHash
{
	size_t operator()(const Fingerprint& fp) const noexcept
	{
		return static_cast<size_t>(fp.lo ^ fp.hi);
	}
};

// --- I/O helpers ---------------------------------------------------------

struct FileCloser
{
	void operator()(FILE* f) const noexcept
	{
		if (f)
			std::fclose(f);
	}
};
using FileHandle = std::unique_ptr<FILE, FileCloser>;

bool writeAll(FILE* f, const void* data, size_t bytes)
{
	return std::fwrite(data, 1, bytes, f) == bytes;
}

bool readAll(FILE* f, void* data, size_t bytes)
{
	return std::fread(data, 1, bytes, f) == bytes;
}

void wipeCacheFiles(const fs::path& dir) noexcept
{
	std::error_code ec;
	fs::remove(dir / "manifest.bin", ec);
	fs::remove(dir / "code.bin", ec);
	fs::remove(dir / "manifest.bin.tmp", ec);
	fs::remove(dir / "code.bin.tmp", ec);
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

// --- Cache::Impl ---------------------------------------------------------

struct Cache::Impl
{
	std::unordered_map<Fingerprint, EmittedCode, FingerprintHash> entries;
	std::vector<std::string> symbolNames; // index 0 reserved (empty)
	std::unordered_map<std::string, uint64_t> symbolIds;
	fs::path cacheDir; // empty -> not attached

	Impl()
	{
		symbolNames.emplace_back(); // id 0 -> ""
	}

	// Load on-disk state from cacheDir into in-memory tables. Returns
	// false if the cache was missing, version-mismatched, or corrupt --
	// in which case both files are wiped and the cache stays empty but
	// usable. Never throws.
	bool loadFromDisk() noexcept
	{
		const fs::path manifestPath = cacheDir / "manifest.bin";
		const fs::path codePath = cacheDir / "code.bin";

		FileHandle manifest(std::fopen(manifestPath.string().c_str(), "rb"));
		FileHandle codeFile(std::fopen(codePath.string().c_str(), "rb"));
		if (!manifest || !codeFile)
		{
			// Missing is not an error -- a freshly-attached cache is empty.
			// But if exactly one of the two files exists, treat that as
			// corrupt and wipe both for consistency.
			if (static_cast<bool>(manifest) != static_cast<bool>(codeFile))
				wipeCacheFiles(cacheDir);
			return false;
		}

		ManifestHeader header{};
		if (!readAll(manifest.get(), &header, sizeof(header)))
		{
			wipeCacheFiles(cacheDir);
			return false;
		}
		if (std::memcmp(header.magic, kManifestMagic, sizeof(kManifestMagic)) != 0
		    || header.cacheFormatVersion != kCacheFormatVersion)
		{
			wipeCacheFiles(cacheDir);
			return false;
		}

		// Verify code.bin's size matches what the manifest expects --
		// catches a half-written code.bin paired with a stale manifest.
		std::error_code ec;
		const uint64_t codeBinSize = static_cast<uint64_t>(fs::file_size(codePath, ec));
		if (ec || codeBinSize != header.codeBinSize)
		{
			wipeCacheFiles(cacheDir);
			return false;
		}

		std::vector<FunctionRecord> records(header.entryCount);
		if (header.entryCount
		    && !readAll(manifest.get(), records.data(),
		                records.size() * sizeof(FunctionRecord)))
		{
			wipeCacheFiles(cacheDir);
			return false;
		}

		// Read the symbol table.
		if (std::fseek(manifest.get(),
		               static_cast<long>(header.symbolTableOffset),
		               SEEK_SET) != 0)
		{
			wipeCacheFiles(cacheDir);
			return false;
		}
		std::vector<std::string> loadedSymbols;
		loadedSymbols.reserve(header.symbolCount + 1);
		loadedSymbols.emplace_back(); // id 0
		for (uint64_t i = 0; i < header.symbolCount; ++i)
		{
			uint32_t nameLen = 0;
			if (!readAll(manifest.get(), &nameLen, sizeof(nameLen)))
			{
				wipeCacheFiles(cacheDir);
				return false;
			}
			std::string name(nameLen, '\0');
			if (nameLen && !readAll(manifest.get(), name.data(), nameLen))
			{
				wipeCacheFiles(cacheDir);
				return false;
			}
			loadedSymbols.push_back(std::move(name));
		}

		// Read every entry's code + relocs from code.bin.
		std::unordered_map<Fingerprint, EmittedCode, FingerprintHash> loadedEntries;
		loadedEntries.reserve(records.size());
		for (const FunctionRecord& rec : records)
		{
			if (std::fseek(codeFile.get(),
			               static_cast<long>(rec.codeOffset),
			               SEEK_SET) != 0)
			{
				wipeCacheFiles(cacheDir);
				return false;
			}
			EmittedCode ec_local;
			ec_local.hostBytes.resize(rec.codeSize);
			if (rec.codeSize
			    && !readAll(codeFile.get(), ec_local.hostBytes.data(), rec.codeSize))
			{
				wipeCacheFiles(cacheDir);
				return false;
			}
			ec_local.relocs.resize(rec.relocCount);
			if (rec.relocCount
			    && !readAll(codeFile.get(), ec_local.relocs.data(),
			                rec.relocCount * sizeof(Reloc)))
			{
				wipeCacheFiles(cacheDir);
				return false;
			}
			ec_local.entryPoints.resize(rec.entryPointCount);
			if (rec.entryPointCount
			    && !readAll(codeFile.get(), ec_local.entryPoints.data(),
			                rec.entryPointCount * sizeof(EntryPoint)))
			{
				wipeCacheFiles(cacheDir);
				return false;
			}
			loadedEntries.emplace(rec.fingerprint, std::move(ec_local));
		}

		// Commit -- swap loaded data into the live tables.
		entries = std::move(loadedEntries);
		symbolNames = std::move(loadedSymbols);
		symbolIds.clear();
		for (size_t i = 1; i < symbolNames.size(); ++i)
			symbolIds.emplace(symbolNames[i], i);
		return true;
	}

	bool flushToDisk() noexcept
	{
		if (cacheDir.empty())
			return false;

		std::error_code ec;
		fs::create_directories(cacheDir, ec);
		// create_directories can fail on permission issues; continue anyway --
		// fopen below will produce the actual diagnostic on the temp paths.

		const fs::path manifestPath = cacheDir / "manifest.bin";
		const fs::path codePath = cacheDir / "code.bin";
		const fs::path manifestTmp = cacheDir / "manifest.bin.tmp";
		const fs::path codeTmp = cacheDir / "code.bin.tmp";

		// Sort entries by fingerprint for deterministic layout. Two flushes
		// with the same in-memory state produce byte-identical manifest +
		// code files.
		std::vector<std::pair<Fingerprint, const EmittedCode*>> ordered;
		ordered.reserve(entries.size());
		for (const auto& [fp, code] : entries)
			ordered.emplace_back(fp, &code);
		std::sort(ordered.begin(), ordered.end(),
		          [](const auto& a, const auto& b) {
			          if (a.first.hi != b.first.hi)
				          return a.first.hi < b.first.hi;
			          return a.first.lo < b.first.lo;
		          });

		FileHandle codeFile(std::fopen(codeTmp.string().c_str(), "wb"));
		if (!codeFile)
			return false;

		std::vector<FunctionRecord> records;
		records.reserve(ordered.size());
		uint64_t cursor = 0;
		for (const auto& [fp, codePtr] : ordered)
		{
			FunctionRecord rec{};
			rec.fingerprint = fp;
			rec.codeOffset = cursor;
			rec.codeSize = codePtr->hostBytes.size();
			cursor += codePtr->hostBytes.size();
			rec.relocOffset = cursor;
			rec.relocCount = static_cast<uint32_t>(codePtr->relocs.size());
			cursor += codePtr->relocs.size() * sizeof(Reloc);
			rec.entryPointOffset = cursor;
			rec.entryPointCount = static_cast<uint32_t>(codePtr->entryPoints.size());
			cursor += codePtr->entryPoints.size() * sizeof(EntryPoint);

			if (rec.codeSize
			    && !writeAll(codeFile.get(), codePtr->hostBytes.data(), rec.codeSize))
				return false;
			if (rec.relocCount
			    && !writeAll(codeFile.get(), codePtr->relocs.data(),
			                 rec.relocCount * sizeof(Reloc)))
				return false;
			if (rec.entryPointCount
			    && !writeAll(codeFile.get(), codePtr->entryPoints.data(),
			                 rec.entryPointCount * sizeof(EntryPoint)))
				return false;
			records.push_back(rec);
		}
		if (std::fflush(codeFile.get()) != 0)
			return false;
		if (jc_fsync(jc_fileno(codeFile.get())) != 0)
			return false;
		codeFile.reset();

		// --- Write manifest.tmp ---
		FileHandle manifestFile(std::fopen(manifestTmp.string().c_str(), "wb"));
		if (!manifestFile)
			return false;

		ManifestHeader header{};
		std::memcpy(header.magic, kManifestMagic, sizeof(kManifestMagic));
		header.cacheFormatVersion = kCacheFormatVersion;
		header.reserved = 0;
		header.entryCount = records.size();
		header.symbolCount = symbolNames.size() > 0 ? symbolNames.size() - 1 : 0;
		// header + records, then symbol table.
		const uint64_t recordsSize = records.size() * sizeof(FunctionRecord);
		header.symbolTableOffset = sizeof(ManifestHeader) + recordsSize;
		// symbolTableSize computed below as we serialize.
		header.codeBinSize = cursor;

		// Write a zero header first; we'll seek back and rewrite it once
		// symbolTableSize is known.
		if (!writeAll(manifestFile.get(), &header, sizeof(header)))
			return false;
		if (!records.empty()
		    && !writeAll(manifestFile.get(), records.data(), recordsSize))
			return false;

		uint64_t symbolTableSize = 0;
		for (size_t i = 1; i < symbolNames.size(); ++i)
		{
			const std::string& name = symbolNames[i];
			uint32_t nameLen = static_cast<uint32_t>(name.size());
			if (!writeAll(manifestFile.get(), &nameLen, sizeof(nameLen)))
				return false;
			if (nameLen && !writeAll(manifestFile.get(), name.data(), nameLen))
				return false;
			symbolTableSize += sizeof(uint32_t) + nameLen;
		}
		header.symbolTableSize = symbolTableSize;

		// Rewrite the header with the final symbolTableSize.
		if (std::fseek(manifestFile.get(), 0, SEEK_SET) != 0)
			return false;
		if (!writeAll(manifestFile.get(), &header, sizeof(header)))
			return false;

		if (std::fflush(manifestFile.get()) != 0)
			return false;
		if (jc_fsync(jc_fileno(manifestFile.get())) != 0)
			return false;
		manifestFile.reset();

		// --- Atomic rename of both files. Rename code.bin first so a
		// reader observing the new manifest is guaranteed to see a
		// matching code.bin. fs::rename is atomic on the same filesystem.
		fs::rename(codeTmp, codePath, ec);
		if (ec)
			return false;
		fs::rename(manifestTmp, manifestPath, ec);
		if (ec)
			return false;

		return true;
	}
};

// --- Cache public methods -----------------------------------------------

Cache::Cache() : m_impl(std::make_unique<Impl>()) {}
Cache::~Cache() = default;

Fingerprint Cache::fingerprint(const FunctionKey& key) const
{
	XXH3StateGuard state;
	XXH3_128bits_reset(state.s);

	// Magic prefix ties the fingerprint to this library version. Changing
	// the algorithm (e.g. swapping in a different hash) MUST change this
	// string -- bumping kCacheFormatVersion alone is not enough, since the
	// on-disk wipe is keyed on the version field but the fingerprint
	// values stored *inside* a cache would silently collide otherwise.
	static constexpr char kMagic[] = "jitcache-v2";
	XXH3_128bits_update(state.s, kMagic, sizeof(kMagic) - 1);
	XXH3_128bits_update(state.s, &key.codegenVersion, sizeof(key.codegenVersion));
	XXH3_128bits_update(state.s, &key.hostCpuFeatureBits, sizeof(key.hostCpuFeatureBits));
	XXH3_128bits_update(state.s, &key.moduleId, sizeof(key.moduleId));
	XXH3_128bits_update(state.s, &key.ppcEntryAddr, sizeof(key.ppcEntryAddr));
	XXH3_128bits_update(state.s, &key.ppcLen, sizeof(key.ppcLen));
	if (key.ppcBytes && key.ppcLen)
		XXH3_128bits_update(state.s, key.ppcBytes, key.ppcLen);

	XXH128_hash_t h = XXH3_128bits_digest(state.s);
	return Fingerprint{h.low64, h.high64};
}

void Cache::load(const fs::path& dir)
{
	m_impl->cacheDir = dir;
	std::error_code ec;
	fs::create_directories(dir, ec);
	(void)m_impl->loadFromDisk(); // best-effort; failure leaves cache empty
}

void Cache::insert(const FunctionKey& key, const EmittedCode& emitted)
{
	m_impl->entries[fingerprint(key)] = emitted;
}

bool Cache::flush()
{
	return m_impl->flushToDisk();
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
