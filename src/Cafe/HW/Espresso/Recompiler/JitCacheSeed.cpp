#include "JitCacheSeed.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "Common/precompiled.h"
#include "Cemu/Logging/CemuLogging.h"

#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <zstd.h>

namespace JitCacheSeed
{

namespace
{

// Read an entire small file (~few MB) into memory. Returns empty on failure.
std::vector<uint8_t> readFileWhole(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	in.seekg(0, std::ios::end);
	const auto sz = in.tellg();
	if (sz < 0 || sz > (1024 * 1024 * 256)) // sanity cap: 256 MB
		return {};
	in.seekg(0, std::ios::beg);
	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz))
		return {};
	return buf;
}

bool writeFileWhole(const fs::path& path, const uint8_t* data, size_t size)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	if (size > 0)
		out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	return static_cast<bool>(out);
}

// Look up the catalog entry for titleId. Returns nullptr if not present.
// Caller is responsible for keeping the parsed Document alive.
const rapidjson::Value* findCatalogEntry(const rapidjson::Document& doc,
                                          uint64_t titleId)
{
	if (!doc.IsObject())
		return nullptr;
	auto entries = doc.FindMember("entries");
	if (entries == doc.MemberEnd() || !entries->value.IsObject())
		return nullptr;
	char idStr[17];
	std::snprintf(idStr, sizeof(idStr), "%016llx",
	              static_cast<unsigned long long>(titleId));
	auto it = entries->value.FindMember(idStr);
	if (it == entries->value.MemberEnd() || !it->value.IsObject())
		return nullptr;
	return &it->value;
}

bool catalogVersionMatches(const rapidjson::Value& entry,
                           uint32_t runtimeCodegenVersion,
                           uint32_t runtimeCacheFormatVersion)
{
	auto cv = entry.FindMember("codegen_version");
	auto fv = entry.FindMember("cache_format_version");
	if (cv == entry.MemberEnd() || !cv->value.IsUint())
		return false;
	if (fv == entry.MemberEnd() || !fv->value.IsUint())
		return false;
	return cv->value.GetUint() == runtimeCodegenVersion
	       && fv->value.GetUint() == runtimeCacheFormatVersion;
}

} // namespace

bool tryBootstrap(uint64_t titleId,
                  const fs::path& cacheDir,
                  const fs::path& seedRoot,
                  uint32_t runtimeCodegenVersion,
                  uint32_t runtimeCacheFormatVersion)
{
	std::error_code ec;
	if (!fs::is_directory(seedRoot, ec))
		return false;

	const fs::path catalogPath = seedRoot / "catalog.json";
	std::vector<uint8_t> catalogBytes = readFileWhole(catalogPath);
	if (catalogBytes.empty())
		return false;

	rapidjson::Document doc;
	doc.Parse(reinterpret_cast<const char*>(catalogBytes.data()), catalogBytes.size());
	if (doc.HasParseError())
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: catalog.json parse error -- ignoring shipped seed");
		return false;
	}

	const rapidjson::Value* entry = findCatalogEntry(doc, titleId);
	if (!entry)
		return false; // no seed for this title; quiet -- normal case

	if (!catalogVersionMatches(*entry, runtimeCodegenVersion, runtimeCacheFormatVersion))
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: catalog entry for title {:016x} has mismatched versions "
		            "(runtime codegen={} cache_format={}), skipping seed -- precompile will run normally",
		            titleId, runtimeCodegenVersion, runtimeCacheFormatVersion);
		return false;
	}

	char filename[32];
	std::snprintf(filename, sizeof(filename), "%016llx.jseed.zst",
	              static_cast<unsigned long long>(titleId));
	const fs::path seedPath = seedRoot / filename;
	std::vector<uint8_t> compressed = readFileWhole(seedPath);
	if (compressed.empty())
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: catalog lists title {:016x} but {} is missing or empty",
		            titleId, filename);
		return false;
	}

	const unsigned long long uncompressedSizeRaw =
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (uncompressedSizeRaw == ZSTD_CONTENTSIZE_ERROR
	    || uncompressedSizeRaw == ZSTD_CONTENTSIZE_UNKNOWN
	    || uncompressedSizeRaw > (1024ULL * 1024 * 256)) // 256 MB sanity cap
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} has bad zstd frame size, skipping",
		            filename);
		return false;
	}

	std::vector<uint8_t> decompressed(static_cast<size_t>(uncompressedSizeRaw));
	const size_t decompressed_actual = ZSTD_decompress(
	    decompressed.data(), decompressed.size(),
	    compressed.data(), compressed.size());
	if (ZSTD_isError(decompressed_actual)
	    || decompressed_actual != decompressed.size())
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} zstd decompress failed ({}), skipping",
		            filename,
		            ZSTD_isError(decompressed_actual) ? ZSTD_getErrorName(decompressed_actual) : "size mismatch");
		return false;
	}

	if (decompressed.size() < kSeedHeaderSize
	    || std::memcmp(decompressed.data(), kSeedMagic, sizeof(kSeedMagic)) != 0)
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} bad header magic, skipping", filename);
		return false;
	}

	uint32_t containerVersion = 0;
	uint32_t seedCodegenVersion = 0;
	uint32_t seedCacheFormatVersion = 0;
	uint64_t manifestBytes = 0;
	uint64_t codeBytes = 0;
	std::memcpy(&containerVersion,        decompressed.data() + 16, 4);
	std::memcpy(&seedCodegenVersion,      decompressed.data() + 20, 4);
	std::memcpy(&seedCacheFormatVersion,  decompressed.data() + 24, 4);
	// bytes 28..31 are pad
	std::memcpy(&manifestBytes,           decompressed.data() + 32, 8);
	std::memcpy(&codeBytes,               decompressed.data() + 40, 8);
	const uint8_t* headerSha = decompressed.data() + 48;

	if (containerVersion != kSeedContainerVersion)
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} container version {} doesn't match runtime expectation {}, skipping",
		            filename, containerVersion, kSeedContainerVersion);
		return false;
	}
	if (seedCodegenVersion != runtimeCodegenVersion
	    || seedCacheFormatVersion != runtimeCacheFormatVersion)
	{
		// The catalog already rejected this case above, but the seed's
		// own header is the second checkpoint -- belt and suspenders.
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} header versions disagree with catalog, skipping",
		            filename);
		return false;
	}

	const size_t expectedTotal = kSeedHeaderSize + manifestBytes + codeBytes;
	if (expectedTotal != decompressed.size())
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} payload size mismatch (header says {} + {} = {}, decompressed is {})",
		            filename, manifestBytes, codeBytes, expectedTotal, decompressed.size());
		return false;
	}

	// SHA-256(manifest_payload || code_payload). Use the byte ranges as
	// laid out in the decompressed buffer, no extra copies.
	uint8_t digest[SHA256_DIGEST_LENGTH];
	SHA256_CTX shaCtx;
	SHA256_Init(&shaCtx);
	SHA256_Update(&shaCtx, decompressed.data() + kSeedHeaderSize, manifestBytes);
	SHA256_Update(&shaCtx, decompressed.data() + kSeedHeaderSize + manifestBytes, codeBytes);
	SHA256_Final(digest, &shaCtx);

	if (std::memcmp(digest, headerSha, SHA256_DIGEST_LENGTH) != 0)
	{
		cemuLog_log(LogType::Force,
		            "JitCacheSeed: seed {} SHA-256 mismatch, refusing to write -- precompile will run normally",
		            filename);
		return false;
	}

	// All checks passed. Write manifest.bin and code.bin into the cache
	// directory; the caller will Cache::load() them next.
	fs::create_directories(cacheDir, ec);
	if (!writeFileWhole(cacheDir / "manifest.bin",
	                    decompressed.data() + kSeedHeaderSize,
	                    manifestBytes))
		return false;
	if (!writeFileWhole(cacheDir / "code.bin",
	                    decompressed.data() + kSeedHeaderSize + manifestBytes,
	                    codeBytes))
	{
		// manifest got written but code.bin didn't -- delete the
		// half-written manifest so the next load() doesn't trip the
		// orphan-file wipe path.
		fs::remove(cacheDir / "manifest.bin", ec);
		return false;
	}

	cemuLog_log(LogType::Force,
	            "JitCacheSeed: hydrated title {:016x} from shipped seed ({} entries equivalent, manifest {} B + code {} B, sha-256 verified)",
	            titleId, "many", manifestBytes, codeBytes);
	return true;
}

} // namespace JitCacheSeed
