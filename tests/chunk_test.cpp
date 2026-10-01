// SPDX-License-Identifier: LGPL-3.0-or-later
#include "chunk.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <zlib.h>

#include "test_util.hpp"

namespace pxsteamdl::detail {
namespace {

using testing::MakeZip;
using testing::SymmetricEncrypt;
using testing::TestKey;
using testing::ToBytes;

const Bytes kContent = ToBytes("chunk content, short enough for a one-byte zstd content size");

// A zstd frame holding content in one raw (uncompressed) block; content must be under 256 bytes.
Bytes RawZstdFrame(ByteSpan content) {
  Bytes frame{0x28, 0xB5, 0x2F, 0xFD};
  frame.push_back(0x20);  // single segment, 1-byte content size
  frame.push_back(static_cast<std::uint8_t>(content.size()));
  std::uint32_t block_header = (static_cast<std::uint32_t>(content.size()) << 3) | 1;  // raw, last block
  for (int i = 0; i < 3; ++i) frame.push_back(static_cast<std::uint8_t>(block_header >> (8 * i)));
  frame.insert(frame.end(), content.begin(), content.end());
  return frame;
}

Bytes VZstd(ByteSpan content) {
  Bytes data = ToBytes("VSZa");
  AppendLe32(data, 0);  // CRC-32, not checked
  Bytes frame = RawZstdFrame(content);
  data.insert(data.end(), frame.begin(), frame.end());
  AppendLe32(data, 0);
  AppendLe32(data, static_cast<std::uint32_t>(content.size()));
  AppendLe32(data, 0);
  data.insert(data.end(), {'z', 's', 'v'});
  return data;
}

ManifestChunk ChunkFor(ByteSpan content, ByteSpan encrypted) {
  ManifestChunk chunk;
  chunk.checksum = static_cast<std::uint32_t>(adler32_z(0, content.data(), content.size()));
  chunk.original_size = static_cast<std::uint32_t>(content.size());
  chunk.compressed_size = static_cast<std::uint32_t>(encrypted.size());
  return chunk;
}

TEST(ChunkTest, DecodesVZstd) {
  Bytes encrypted = SymmetricEncrypt(VZstd(kContent), TestKey());
  EXPECT_EQ(DecodeChunk(encrypted, TestKey(), ChunkFor(kContent, encrypted)), kContent);
}

TEST(ChunkTest, DecodesZip) {
  Bytes encrypted = SymmetricEncrypt(MakeZip(kContent, true), TestKey());
  EXPECT_EQ(DecodeChunk(encrypted, TestKey(), ChunkFor(kContent, encrypted)), kContent);
}

TEST(ChunkTest, RejectsChecksumMismatch) {
  Bytes encrypted = SymmetricEncrypt(VZstd(kContent), TestKey());
  ManifestChunk chunk = ChunkFor(kContent, encrypted);
  chunk.checksum ^= 1;
  EXPECT_THROW(DecodeChunk(encrypted, TestKey(), chunk), std::runtime_error);
}

TEST(ChunkTest, RejectsSizeMismatch) {
  Bytes encrypted = SymmetricEncrypt(VZstd(kContent), TestKey());
  ManifestChunk chunk = ChunkFor(kContent, encrypted);
  chunk.compressed_size += 16;
  EXPECT_THROW(DecodeChunk(encrypted, TestKey(), chunk), std::runtime_error);
  chunk = ChunkFor(kContent, encrypted);
  chunk.original_size += 1;
  EXPECT_THROW(DecodeChunk(encrypted, TestKey(), chunk), std::runtime_error);
}

TEST(ChunkTest, RejectsUnknownFormat) {
  Bytes encrypted = SymmetricEncrypt(kContent, TestKey());
  EXPECT_THROW(DecodeChunk(encrypted, TestKey(), ChunkFor(kContent, encrypted)), std::runtime_error);
}

}  // namespace
}  // namespace pxsteamdl::detail
