// SPDX-License-Identifier: LGPL-3.0-or-later
// SteamPipe depot manifests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "crypto.hpp"

namespace pxsteamdl::detail {

struct ManifestChunk {
    // SHA-1 of the chunk's content, which also names it on the CDN.
    Sha1Hash sha{};
    // Adler-32 of the decompressed data.
    std::uint32_t checksum = 0;
    // Position of the chunk within its file.
    std::uint64_t offset = 0;
    std::uint32_t original_size = 0;
    // Size of the encrypted, compressed chunk as served by the CDN.
    std::uint32_t compressed_size = 0;
};

// EDepotFileFlag bits used here.
namespace depot_file_flag {
inline constexpr std::uint32_t kExecutable = 0x20;
inline constexpr std::uint32_t kDirectory = 0x40;
inline constexpr std::uint32_t kCustomExecutable = 0x80;
inline constexpr std::uint32_t kSymlink = 0x200;
}  // namespace depot_file_flag

struct ManifestFile {
    // Validated relative path with '/' separators.
    std::string name;
    // Relative target of a symlink.
    std::string link_target;
    std::uint64_t size = 0;
    std::uint32_t flags = 0;
    Sha1Hash sha{};
    // Sorted by offset; they cover the file exactly.
    std::vector<ManifestChunk> chunks;

    bool IsDirectory() const { return flags & depot_file_flag::kDirectory; }
    bool IsSymlink() const { return flags & depot_file_flag::kSymlink; }
    bool IsRegular() const { return !IsDirectory() && !IsSymlink(); }
    bool IsExecutable() const { return flags & (depot_file_flag::kExecutable | depot_file_flag::kCustomExecutable); }
};

// Parses a zipped manifest of the given depot and manifest ID, decrypting file names with key if they are
// encrypted. Throws if the manifest is malformed, belongs to another depot or manifest, or contains unsafe paths.
std::vector<ManifestFile> ParseManifest(ByteSpan zip, const AesKey& key, std::uint32_t depot,
                                        std::uint64_t manifest_id);

}  // namespace pxsteamdl::detail
