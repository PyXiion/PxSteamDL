// SPDX-License-Identifier: LGPL-3.0-or-later
#include "chunk.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include "zip.hpp"

namespace pxsteamdl::detail {

namespace {

// VZstd: "VSZa", CRC-32 (4), Zstd frame, CRC-32 (4), original size (4), reserved (4), "zsv".
constexpr std::string_view kVZstdMagic = "VSZa";
constexpr std::string_view kVZstdFooter = "zsv";
constexpr std::size_t kVZstdHeaderSize = 8;
constexpr std::size_t kVZstdFooterSize = 15;
constexpr std::size_t kVZstdSizeFromEnd = 11;

// VZip: "VZa", timestamp (4), LZMA properties (5), LZMA data, CRC-32 (4), original size (4), "zv".
constexpr std::string_view kVZipMagic = "VZa";
constexpr std::size_t kVZipPropertiesOffset = 7;
constexpr std::size_t kVZipPropertiesSize = 5;
constexpr std::size_t kVZipHeaderSize = 12;
constexpr std::size_t kVZipFooterSize = 10;
constexpr std::size_t kVZipSizeFromEnd = 6;
// An .lzma ("alone") header: properties, then the uncompressed size as 64-bit little-endian.
constexpr std::size_t kLzmaAloneHeaderSize = 13;

constexpr std::string_view kZipMagic{"PK\x03\x04", 4};

bool StartsWith(ByteSpan data, std::string_view prefix) {
  return data.size() >= prefix.size() && std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
}

bool EndsWith(ByteSpan data, std::string_view suffix) {
  return data.size() >= suffix.size() &&
         std::memcmp(data.data() + data.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

void DecompressVZstd(ByteSpan data, Bytes& result) {
  if (!EndsWith(data, kVZstdFooter) || ReadLe32(data, data.size() - kVZstdSizeFromEnd) != result.size()) {
    Fail("chunk: invalid VZstd footer");
  }
  ByteSpan frame = data.subspan(kVZstdHeaderSize, data.size() - kVZstdHeaderSize - kVZstdFooterSize);
  std::size_t written = ZSTD_decompress(result.data(), result.size(), frame.data(), frame.size());
  if (ZSTD_isError(written) || written != result.size()) Fail("chunk: Zstd decompression failed");
}

void DecompressVZip(ByteSpan data, Bytes& result) {
  if (!EndsWith(data, "zv") || ReadLe32(data, data.size() - kVZipSizeFromEnd) != result.size()) {
    Fail("chunk: invalid VZip footer");
  }
  // Rewrite as an .lzma stream, which liblzma decodes directly.
  ByteSpan properties = data.subspan(kVZipPropertiesOffset, kVZipPropertiesSize);
  ByteSpan packed = data.subspan(kVZipHeaderSize, data.size() - kVZipHeaderSize - kVZipFooterSize);
  Bytes alone;
  alone.reserve(kLzmaAloneHeaderSize + packed.size());
  alone.insert(alone.end(), properties.begin(), properties.end());
  for (int i = 0; i < 8; ++i) alone.push_back(static_cast<std::uint8_t>(std::uint64_t{result.size()} >> (8 * i)));
  alone.insert(alone.end(), packed.begin(), packed.end());

  lzma_stream stream = LZMA_STREAM_INIT;
  if (lzma_alone_decoder(&stream, UINT64_MAX) != LZMA_OK) Fail("chunk: LZMA initialization failed");
  stream.next_in = alone.data();
  stream.avail_in = alone.size();
  std::uint8_t empty = 0;
  stream.next_out = result.empty() ? &empty : result.data();
  stream.avail_out = result.empty() ? 1 : result.size();
  lzma_ret status = lzma_code(&stream, LZMA_FINISH);
  auto written = stream.total_out;
  lzma_end(&stream);
  if (status != LZMA_STREAM_END || written != result.size()) Fail("chunk: LZMA decompression failed");
}

}  // namespace

Bytes DecodeChunk(ByteSpan encrypted, const AesKey& key, const ManifestChunk& chunk) {
  if (encrypted.size() != chunk.compressed_size) Fail("chunk: compressed size mismatch");
  Bytes plain = SymmetricDecrypt(encrypted, key);
  ByteSpan data(plain);
  Bytes result(chunk.original_size);
  if (data.size() >= kVZstdHeaderSize + kVZstdFooterSize && StartsWith(data, kVZstdMagic)) {
    DecompressVZstd(data, result);
  } else if (data.size() >= kVZipHeaderSize + kVZipFooterSize && StartsWith(data, kVZipMagic)) {
    DecompressVZip(data, result);
  } else if (StartsWith(data, kZipMagic)) {
    result = UnzipSingleEntry(data);
    if (result.size() != chunk.original_size) Fail("chunk: ZIP length mismatch");
  } else {
    Fail("chunk: unknown compression format");
  }
  if (adler32_z(0, result.data(), result.size()) != chunk.checksum) Fail("chunk: Adler32 mismatch");
  return result;
}

}  // namespace pxsteamdl::detail
