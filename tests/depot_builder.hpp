// SPDX-License-Identifier: LGPL-3.0-or-later
// Builds manifests in the binary format that ParseManifest() reads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "manifest.hpp"
#include "proto.hpp"
#include "test_util.hpp"

namespace pxsteamdl::detail::testing {

// What a manifest says about one file; unlike ManifestFile, the name is stored as given (even encrypted).
struct ManifestEntry {
  std::string name;
  std::uint64_t size = 0;
  std::uint32_t flags = 0;
  Sha1Hash sha{};
  std::string link_target;
  std::vector<ManifestChunk> chunks;
};

inline Bytes EncodeFixed32(std::uint32_t field, std::uint32_t value) {
  Bytes out{static_cast<std::uint8_t>((field << 3) | 5)};
  AppendLe32(out, value);
  return out;
}

inline Bytes EncodeChunkEntry(const ManifestChunk& chunk) {
  return Concat({EncodeBytes(1, chunk.sha), EncodeFixed32(2, chunk.checksum), EncodeUint(3, chunk.offset),
                 EncodeUint(4, chunk.original_size), EncodeUint(5, chunk.compressed_size)});
}

inline Bytes EncodeFileEntry(const ManifestEntry& file) {
  Bytes out = Concat(
      {EncodeString(1, file.name), EncodeUint(2, file.size), EncodeUint(3, file.flags), EncodeBytes(5, file.sha)});
  auto append = [&](const Bytes& field) { out.insert(out.end(), field.begin(), field.end()); };
  if (!file.link_target.empty()) append(EncodeString(7, file.link_target));
  for (const ManifestChunk& chunk : file.chunks) append(EncodeBytes(6, EncodeChunkEntry(chunk)));
  return out;
}

inline void AppendSection(Bytes& out, std::uint32_t magic, const Bytes& section) {
  AppendLe32(out, magic);
  AppendLe32(out, static_cast<std::uint32_t>(section.size()));
  out.insert(out.end(), section.begin(), section.end());
}

// A zipped manifest of the given depot and manifest ID.
inline Bytes BuildManifest(const std::vector<ManifestEntry>& files, std::uint32_t depot, std::uint64_t manifest_id,
                           bool encrypted_names = false) {
  Bytes payload;
  for (const ManifestEntry& file : files) {
    Bytes field = EncodeBytes(1, EncodeFileEntry(file));
    payload.insert(payload.end(), field.begin(), field.end());
  }
  Bytes binary;
  AppendSection(binary, 0x71f617d0, payload);
  AppendSection(binary, 0x1f4812be,
                Concat({EncodeUint(1, depot), EncodeUint(2, manifest_id), EncodeUint(4, encrypted_names ? 1 : 0)}));
  AppendSection(binary, 0x1b81b817, {});
  AppendLe32(binary, 0x32c415ab);
  return MakeZip(binary, true);
}

}  // namespace pxsteamdl::detail::testing
