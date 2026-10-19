// SPDX-License-Identifier: LGPL-3.0-or-later
// Synthetic SteamPipe content for the benchmark server: items made of files made of chunks, in the exact formats the
// library decodes (ZIP-deflated, AES-256-CBC encrypted chunks; a zipped binary manifest).
#pragma once

#include <mbedtls/aes.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include "crypto.hpp"
#include "manifest.hpp"
#include "proto.hpp"

namespace pxsteamdl::bench {

using detail::AesKey;
using detail::Bytes;
using detail::ByteSpan;

inline constexpr std::uint32_t kAppId = 294100;
inline constexpr std::uint64_t kRequestCode = 777;

inline AesKey DepotKey() {
  AesKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);
  return key;
}

inline void AppendLe16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

// A ZIP archive with one entry named "z", deflated at the given zlib level.
inline Bytes MakeZip(ByteSpan content, int level) {
  z_stream stream{};
  deflateInit2(&stream, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  Bytes packed(deflateBound(&stream, static_cast<uLong>(content.size())));
  stream.next_in = const_cast<Bytef*>(content.data());
  stream.avail_in = static_cast<uInt>(content.size());
  stream.next_out = packed.data();
  stream.avail_out = static_cast<uInt>(packed.size());
  deflate(&stream, Z_FINISH);
  packed.resize(stream.total_out);
  deflateEnd(&stream);

  auto crc = static_cast<std::uint32_t>(crc32_z(0, content.data(), content.size()));
  Bytes zip;
  detail::AppendLe32(zip, 0x04034b50);
  AppendLe16(zip, 20);
  AppendLe16(zip, 0);
  AppendLe16(zip, 8);
  detail::AppendLe32(zip, 0);
  detail::AppendLe32(zip, crc);
  detail::AppendLe32(zip, static_cast<std::uint32_t>(packed.size()));
  detail::AppendLe32(zip, static_cast<std::uint32_t>(content.size()));
  AppendLe16(zip, 1);
  AppendLe16(zip, 0);
  zip.push_back('z');
  zip.insert(zip.end(), packed.begin(), packed.end());
  auto central_offset = static_cast<std::uint32_t>(zip.size());
  detail::AppendLe32(zip, 0x02014b50);
  AppendLe16(zip, 20);
  AppendLe16(zip, 20);
  AppendLe16(zip, 0);
  AppendLe16(zip, 8);
  detail::AppendLe32(zip, 0);
  detail::AppendLe32(zip, crc);
  detail::AppendLe32(zip, static_cast<std::uint32_t>(packed.size()));
  detail::AppendLe32(zip, static_cast<std::uint32_t>(content.size()));
  AppendLe16(zip, 1);
  AppendLe16(zip, 0);
  AppendLe16(zip, 0);
  AppendLe16(zip, 0);
  AppendLe16(zip, 0);
  detail::AppendLe32(zip, 0);
  detail::AppendLe32(zip, 0);
  zip.push_back('z');
  auto central_size = static_cast<std::uint32_t>(zip.size() - central_offset);
  detail::AppendLe32(zip, 0x06054b50);
  AppendLe16(zip, 0);
  AppendLe16(zip, 0);
  AppendLe16(zip, 1);
  AppendLe16(zip, 1);
  detail::AppendLe32(zip, central_size);
  detail::AppendLe32(zip, central_offset);
  AppendLe16(zip, 0);
  return zip;
}

// Steam's symmetric encryption: ECB-encrypted IV, then CBC with PKCS#7 padding.
inline Bytes SymmetricEncrypt(ByteSpan plain, const AesKey& key) {
  std::array<std::uint8_t, 16> iv{};
  for (std::size_t i = 0; i < iv.size(); ++i) iv[i] = static_cast<std::uint8_t>(0xA0 + i);
  Bytes padded(plain.begin(), plain.end());
  auto padding = static_cast<std::uint8_t>(16 - padded.size() % 16);
  padded.insert(padded.end(), padding, padding);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key.data(), 256);
  Bytes out(16 + padded.size());
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, iv.data(), out.data());
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded.size(), iv.data(), padded.data(), out.data() + 16);
  mbedtls_aes_free(&aes);
  return out;
}

// Text-like bytes (deflate shrinks them to roughly a third), reproducible from the seed.
inline Bytes MakeData(std::uint64_t seed, std::size_t size) {
  std::uint64_t state = seed * 0x9E3779B97F4A7C15ULL + 1;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  std::vector<std::string> words(512);
  for (auto& word : words) {
    std::size_t length = 3 + next() % 8;
    for (std::size_t i = 0; i < length; ++i) word.push_back(static_cast<char>('a' + next() % 26));
  }
  Bytes data;
  data.reserve(size + 16);
  while (data.size() < size) {
    // Squaring the draw makes some words much more frequent than others.
    std::uint64_t pick = next() % 512;
    const std::string& word = words[(pick * pick) % 512];
    data.insert(data.end(), word.begin(), word.end());
    data.push_back(next() % 9 == 0 ? '\n' : ' ');
  }
  data.resize(size);
  return data;
}

inline detail::Bytes EncodeFixed32(std::uint32_t field, std::uint32_t value) {
  Bytes out{static_cast<std::uint8_t>((field << 3) | 5)};
  detail::AppendLe32(out, value);
  return out;
}

inline Bytes EncodeChunkEntry(const detail::ManifestChunk& chunk) {
  return detail::Concat({detail::EncodeBytes(1, chunk.sha), EncodeFixed32(2, chunk.checksum),
                         detail::EncodeUint(3, chunk.offset), detail::EncodeUint(4, chunk.original_size),
                         detail::EncodeUint(5, chunk.compressed_size)});
}

struct FileEntry {
  std::string name;
  std::uint64_t size = 0;
  detail::Sha1Hash sha{};
  std::vector<detail::ManifestChunk> chunks;
};

inline void AppendSection(Bytes& out, std::uint32_t magic, const Bytes& section) {
  detail::AppendLe32(out, magic);
  detail::AppendLe32(out, static_cast<std::uint32_t>(section.size()));
  out.insert(out.end(), section.begin(), section.end());
}

inline Bytes BuildManifest(const std::vector<FileEntry>& files, std::uint32_t depot, std::uint64_t manifest_id) {
  Bytes payload;
  for (const FileEntry& file : files) {
    Bytes entry = detail::Concat({detail::EncodeString(1, file.name), detail::EncodeUint(2, file.size),
                                  detail::EncodeUint(3, 0), detail::EncodeBytes(5, file.sha)});
    for (const auto& chunk : file.chunks) {
      Bytes field = detail::EncodeBytes(6, EncodeChunkEntry(chunk));
      entry.insert(entry.end(), field.begin(), field.end());
    }
    Bytes field = detail::EncodeBytes(1, entry);
    payload.insert(payload.end(), field.begin(), field.end());
  }
  Bytes binary;
  AppendSection(binary, 0x71f617d0, payload);
  AppendSection(
      binary, 0x1f4812be,
      detail::Concat({detail::EncodeUint(1, depot), detail::EncodeUint(2, manifest_id), detail::EncodeUint(4, 0)}));
  AppendSection(binary, 0x1b81b817, {});
  detail::AppendLe32(binary, 0x32c415ab);
  return MakeZip(binary, 6);
}

struct Group {
  std::string name;
  std::uint64_t first_id = 0;
  int items = 0;
  int files = 0;
  std::size_t file_bytes = 0;
  std::size_t chunk_bytes = 0;
};

struct Item {
  std::string title;
  std::uint64_t manifest_id = 0;
  Bytes manifest;
};

struct Corpus {
  std::unordered_map<std::uint64_t, Item> items;
  std::unordered_map<std::string, Bytes> chunks;  // encrypted chunk by SHA-1 in hex
  std::uint64_t total_plain = 0;
  std::uint64_t total_wire = 0;

  void add(const Group& group, std::uint64_t seed) {
    AesKey key = DepotKey();
    for (int i = 0; i < group.items; ++i) {
      std::uint64_t id = group.first_id + static_cast<std::uint64_t>(i);
      struct Built {
        FileEntry entry;
        std::vector<std::pair<std::string, Bytes>> chunks;
      };
      // Files are built in parallel; chunks are deflated at level 1 to keep start-up short.
      std::vector<std::future<Built>> pending;
      for (int f = 0; f < group.files; ++f) {
        pending.push_back(std::async(std::launch::async, [&, f] {
          Built built;
          built.entry.name = "d" + std::to_string(f / 10) + "/" + group.name + "-" + std::to_string(f) + ".bin";
          Bytes plain = MakeData(seed + id * 100003 + static_cast<std::uint64_t>(f), group.file_bytes);
          built.entry.size = plain.size();
          built.entry.sha = detail::Sha1(plain);
          for (std::size_t offset = 0; offset < plain.size(); offset += group.chunk_bytes) {
            ByteSpan piece(plain.data() + offset, std::min(group.chunk_bytes, plain.size() - offset));
            Bytes encrypted = SymmetricEncrypt(MakeZip(piece, 1), key);
            detail::ManifestChunk chunk;
            chunk.sha = detail::Sha1(piece);
            chunk.checksum = static_cast<std::uint32_t>(adler32_z(0, piece.data(), piece.size()));
            chunk.offset = offset;
            chunk.original_size = static_cast<std::uint32_t>(piece.size());
            chunk.compressed_size = static_cast<std::uint32_t>(encrypted.size());
            built.entry.chunks.push_back(chunk);
            built.chunks.emplace_back(detail::ToHex(chunk.sha), std::move(encrypted));
          }
          return built;
        }));
      }
      std::vector<FileEntry> entries;
      for (auto& future : pending) {
        Built built = future.get();
        total_plain += built.entry.size;
        for (auto& [hex, encrypted] : built.chunks) {
          total_wire += encrypted.size();
          chunks.emplace(std::move(hex), std::move(encrypted));
        }
        entries.push_back(std::move(built.entry));
      }
      Item item;
      item.title = group.name + " " + std::to_string(id);
      item.manifest_id = 5000000 + id;
      item.manifest = BuildManifest(entries, kAppId, item.manifest_id);
      items.emplace(id, std::move(item));
    }
  }
};

}  // namespace pxsteamdl::bench
