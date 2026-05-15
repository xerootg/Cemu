#pragma once

// Phase C: shipped offline JIT cache seed.
//
// Loads a pre-built cache image bundled in the APK into the per-title cache
// directory on first launch, so the very first run of a title pays zero JIT
// cost. The seed file is a zstd-compressed container holding the same
// manifest.bin + code.bin pair the runtime cache uses, plus an integrity
// header. A catalog.json shipped alongside lists which titles have seeds
// and what codegen version they correspond to.
//
// Reliability story:
//   - kCodegenVersion mismatch in the catalog -> seed ignored, falls
//     through to phase B precompile. Same outcome as no seed at all.
//   - SHA-256 mismatch in the header -> seed ignored.
//   - Decompression failure -> seed ignored.
//   - Any I/O error -> seed ignored.
//
// The runtime cache still validates each function's XXH3-128 fingerprint
// per-entry on lookup, so even a malformed seed that bypasses these checks
// can't make the JIT execute bad bytes -- the worst case is "cache hit
// failed, fall through to fresh codegen for this function".

#include <cstdint>
#include <filesystem>

namespace JitCacheSeed
{

// On-disk seed container (zstd-compressed).
//   bytes  0..15    magic "CEMU_JIT_SEED\0\0\0"
//   bytes 16..19    container_version (uint32 LE)        -- this header
//   bytes 20..23    codegen_version   (uint32 LE)        -- must match runtime
//   bytes 24..27    cache_format_version (uint32 LE)     -- must match runtime
//   bytes 28..31    _pad
//   bytes 32..39    manifest_bytes   (uint64 LE)
//   bytes 40..47    code_bytes       (uint64 LE)
//   bytes 48..79    sha256 of (manifest_payload || code_payload)
//   bytes 80..      manifest_payload (manifest_bytes), then code_payload
constexpr uint32_t kSeedContainerVersion = 1;
constexpr size_t   kSeedHeaderSize       = 80;
constexpr char     kSeedMagic[16]        = {'C','E','M','U','_','J','I','T','_','S','E','E','D','\0','\0','\0'};

// Try to bootstrap a seed for `titleId` into `cacheDir`. Returns true iff
// the seed was found, validated end-to-end, and manifest.bin + code.bin
// were written to disk. On false the cache directory is left as-is and
// the caller proceeds normally (phase B precompile will fill it).
//
// `seedRoot` is <userDataPath>/cache_seed/ -- where CemuApplication.kt
// has copied the APK assets. `runtimeCodegenVersion` and
// `runtimeCacheFormatVersion` are the constants the rest of the cache
// is using; a seed that doesn't match exactly is rejected.
bool tryBootstrap(uint64_t titleId,
                  const std::filesystem::path& cacheDir,
                  const std::filesystem::path& seedRoot,
                  uint32_t runtimeCodegenVersion,
                  uint32_t runtimeCacheFormatVersion);

} // namespace JitCacheSeed
