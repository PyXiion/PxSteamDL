// SPDX-License-Identifier: LGPL-3.0-or-later
#include "manifest.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <mbedtls/base64.h>

#include "proto.hpp"
#include "test_util.hpp"

namespace pxsteamdl::detail {
namespace {

using testing::MakeZip;
using testing::SymmetricEncrypt;
using testing::TestKey;
using testing::ToBytes;

constexpr std::uint32_t kDepot = 294100;
constexpr std::uint64_t kManifestId = 1234567890123ULL;

Bytes EncodeFixed32(std::uint32_t field, std::uint32_t value) {
    Bytes out{static_cast<std::uint8_t>((field << 3) | 5)};
    AppendLe32(out, value);
    return out;
}

Bytes Chunk(std::uint8_t sha_byte, std::uint64_t offset, std::uint32_t size) {
    return Concat({EncodeBytes(1, Bytes(20, sha_byte)), EncodeFixed32(2, 0xABCD), EncodeUint(3, offset),
                   EncodeUint(4, size), EncodeUint(5, 64)});
}

Bytes File(const std::string& name, std::uint64_t size, std::uint32_t flags, const std::vector<Bytes>& chunks) {
    Bytes file =
        Concat({EncodeString(1, name), EncodeUint(2, size), EncodeUint(3, flags), EncodeBytes(5, Bytes(20, 1))});
    for (const Bytes& chunk : chunks) {
        Bytes field = EncodeBytes(6, chunk);
        file.insert(file.end(), field.begin(), field.end());
    }
    return file;
}

void AppendSection(Bytes& out, std::uint32_t magic, const Bytes& section) {
    AppendLe32(out, magic);
    AppendLe32(out, static_cast<std::uint32_t>(section.size()));
    out.insert(out.end(), section.begin(), section.end());
}

Bytes Manifest(const std::vector<Bytes>& files, bool encrypted = false, std::uint32_t depot = kDepot) {
    Bytes payload;
    for (const Bytes& file : files) {
        Bytes field = EncodeBytes(1, file);
        payload.insert(payload.end(), field.begin(), field.end());
    }
    Bytes binary;
    AppendSection(binary, 0x71f617d0, payload);
    AppendSection(binary, 0x1f4812be,
                  Concat({EncodeUint(1, depot), EncodeUint(2, kManifestId), EncodeUint(4, encrypted ? 1 : 0)}));
    AppendSection(binary, 0x1b81b817, {});
    AppendLe32(binary, 0x32c415ab);
    return MakeZip(binary, true);
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

TEST(ManifestTest, ParsesFilesAndSortsChunks) {
    Bytes zip = Manifest({
        File("About\\About.xml", 30, 0, {Chunk(2, 10, 20), Chunk(3, 0, 10)}),
        File("Textures", 0, depot_file_flag::kDirectory, {}),
        File("run.sh", 0, depot_file_flag::kExecutable, {}),
    });
    std::vector<ManifestFile> files = ParseManifest(zip, TestKey(), kDepot, kManifestId);
    ASSERT_EQ(files.size(), 3u);

    EXPECT_EQ(files[0].name, "About/About.xml");
    EXPECT_TRUE(files[0].IsRegular());
    EXPECT_EQ(files[0].size, 30u);
    ASSERT_EQ(files[0].chunks.size(), 2u);
    EXPECT_EQ(files[0].chunks[0].offset, 0u);
    EXPECT_EQ(files[0].chunks[0].sha[0], 3);
    EXPECT_EQ(files[0].chunks[1].original_size, 20u);
    EXPECT_EQ(files[0].chunks[1].checksum, 0xABCDu);
    EXPECT_EQ(files[0].chunks[1].compressed_size, 64u);

    EXPECT_TRUE(files[1].IsDirectory());
    EXPECT_TRUE(files[2].IsExecutable());
}

TEST(ManifestTest, DecryptsNames) {
    Bytes zip = Manifest({File(EncryptName("Defs/Things.xml"), 0, 0, {})}, true);
    std::vector<ManifestFile> files = ParseManifest(zip, TestKey(), kDepot, kManifestId);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].name, "Defs/Things.xml");
}

TEST(ManifestTest, RejectsOtherDepotOrManifest) {
    Bytes zip = Manifest({File("a", 0, 0, {})}, false, kDepot + 1);
    EXPECT_THROW(ParseManifest(zip, TestKey(), kDepot, kManifestId), std::runtime_error);
    EXPECT_THROW(ParseManifest(Manifest({File("a", 0, 0, {})}), TestKey(), kDepot, kManifestId + 1),
                 std::runtime_error);
}

TEST(ManifestTest, RejectsChunksThatDoNotCoverFile) {
    EXPECT_THROW(ParseManifest(Manifest({File("gap", 30, 0, {Chunk(1, 0, 10), Chunk(2, 11, 19)})}), TestKey(), kDepot,
                               kManifestId),
                 std::runtime_error);
    EXPECT_THROW(ParseManifest(Manifest({File("short", 30, 0, {Chunk(1, 0, 10)})}), TestKey(), kDepot, kManifestId),
                 std::runtime_error);
}

TEST(ManifestTest, RejectsUnsafePath) {
    EXPECT_THROW(ParseManifest(Manifest({File("../escape", 0, 0, {})}), TestKey(), kDepot, kManifestId),
                 std::runtime_error);
}

TEST(ManifestTest, RejectsDirectoryWithChunks) {
    EXPECT_THROW(ParseManifest(Manifest({File("dir", 10, depot_file_flag::kDirectory, {Chunk(1, 0, 10)})}), TestKey(),
                               kDepot, kManifestId),
                 std::runtime_error);
}

}  // namespace
}  // namespace pxsteamdl::detail
