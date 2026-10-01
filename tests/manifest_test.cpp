// SPDX-License-Identifier: LGPL-3.0-or-later
#include "manifest.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <mbedtls/base64.h>

#include "depot_builder.hpp"

namespace pxsteamdl::detail {
namespace {

using testing::BuildManifest;
using testing::ManifestEntry;
using testing::SymmetricEncrypt;
using testing::TestKey;
using testing::ToBytes;

constexpr std::uint32_t kDepot = 294100;
constexpr std::uint64_t kManifestId = 1234567890123ULL;

ManifestChunk MakeChunk(std::uint8_t sha_byte, std::uint64_t offset, std::uint32_t size) {
  ManifestChunk chunk;
  chunk.sha.fill(sha_byte);
  chunk.checksum = 0xABCD;
  chunk.offset = offset;
  chunk.original_size = size;
  chunk.compressed_size = 64;
  return chunk;
}

ManifestEntry MakeFile(const std::string& name, std::uint64_t size, std::uint32_t flags,
                       std::vector<ManifestChunk> chunks) {
  ManifestEntry entry;
  entry.name = name;
  entry.size = size;
  entry.flags = flags;
  entry.sha.fill(1);
  entry.chunks = std::move(chunks);
  return entry;
}

std::string EncryptName(const std::string& name) {
  Bytes encrypted = SymmetricEncrypt(ToBytes(name), TestKey());
  std::string base64(encrypted.size() * 2 + 4, '\0');
  std::size_t length = 0;
  mbedtls_base64_encode(reinterpret_cast<unsigned char*>(base64.data()), base64.size(), &length, encrypted.data(),
                        encrypted.size());
  base64.resize(length);
  return base64;
}

std::vector<ManifestFile> Parse(const Bytes& zip, std::uint64_t manifest_id = kManifestId) {
  return ParseManifest(zip, TestKey(), kDepot, manifest_id);
}

TEST(ManifestTest, ParsesFilesAndSortsChunks) {
  Bytes zip = BuildManifest(
      {
          MakeFile("About\\About.xml", 30, 0, {MakeChunk(2, 10, 20), MakeChunk(3, 0, 10)}),
          MakeFile("Textures", 0, depot_file_flag::kDirectory, {}),
          MakeFile("run.sh", 0, depot_file_flag::kExecutable, {}),
      },
      kDepot, kManifestId);
  std::vector<ManifestFile> files = Parse(zip);
  ASSERT_EQ(files.size(), 3u);

  EXPECT_EQ(files[0].name, "About/About.xml");
  EXPECT_TRUE(files[0].isRegular());
  EXPECT_EQ(files[0].size, 30u);
  ASSERT_EQ(files[0].chunks.size(), 2u);
  EXPECT_EQ(files[0].chunks[0].offset, 0u);
  EXPECT_EQ(files[0].chunks[0].sha[0], 3);
  EXPECT_EQ(files[0].chunks[1].original_size, 20u);
  EXPECT_EQ(files[0].chunks[1].checksum, 0xABCDu);
  EXPECT_EQ(files[0].chunks[1].compressed_size, 64u);

  EXPECT_TRUE(files[1].isDirectory());
  EXPECT_TRUE(files[2].isExecutable());
}

TEST(ManifestTest, ParsesSymlinks) {
  ManifestEntry link = MakeFile("current", 0, depot_file_flag::kSymlink, {});
  link.link_target = "versions\\1.5";
  std::vector<ManifestFile> files = Parse(BuildManifest({link}, kDepot, kManifestId));
  ASSERT_EQ(files.size(), 1u);
  EXPECT_TRUE(files[0].isSymlink());
  EXPECT_EQ(files[0].link_target, "versions/1.5");
}

TEST(ManifestTest, DecryptsNames) {
  Bytes zip = BuildManifest({MakeFile(EncryptName("Defs/Things.xml"), 0, 0, {})}, kDepot, kManifestId, true);
  std::vector<ManifestFile> files = Parse(zip);
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files[0].name, "Defs/Things.xml");
}

TEST(ManifestTest, RejectsOtherDepotOrManifest) {
  EXPECT_THROW(Parse(BuildManifest({MakeFile("a", 0, 0, {})}, kDepot + 1, kManifestId)), std::runtime_error);
  EXPECT_THROW(Parse(BuildManifest({MakeFile("a", 0, 0, {})}, kDepot, kManifestId), kManifestId + 1),
               std::runtime_error);
}

TEST(ManifestTest, RejectsChunksThatDoNotCoverFile) {
  EXPECT_THROW(
      Parse(BuildManifest({MakeFile("gap", 30, 0, {MakeChunk(1, 0, 10), MakeChunk(2, 11, 19)})}, kDepot, kManifestId)),
      std::runtime_error);
  EXPECT_THROW(Parse(BuildManifest({MakeFile("short", 30, 0, {MakeChunk(1, 0, 10)})}, kDepot, kManifestId)),
               std::runtime_error);
}

TEST(ManifestTest, RejectsUnsafePath) {
  EXPECT_THROW(Parse(BuildManifest({MakeFile("../escape", 0, 0, {})}, kDepot, kManifestId)), std::runtime_error);
}

TEST(ManifestTest, RejectsDirectoryWithChunks) {
  EXPECT_THROW(Parse(BuildManifest({MakeFile("dir", 10, depot_file_flag::kDirectory, {MakeChunk(1, 0, 10)})}, kDepot,
                                   kManifestId)),
               std::runtime_error);
}

}  // namespace
}  // namespace pxsteamdl::detail
