// SPDX-License-Identifier: LGPL-3.0-or-later
#include "manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <mbedtls/base64.h>

#include "paths.hpp"
#include "proto.hpp"
#include "zip.hpp"

namespace pxsteamdl::detail {

namespace {

// Section magics of the binary manifest; each but the end marker is followed by a 32-bit size.
constexpr std::uint32_t kPayloadMagic = 0x71f617d0;
constexpr std::uint32_t kMetadataMagic = 0x1f4812be;
constexpr std::uint32_t kSignatureMagic = 0x1b81b817;
constexpr std::uint32_t kEndMagic = 0x32c415ab;

// ContentManifestPayload.FileMapping.ChunkData field numbers.
namespace chunk_field {
constexpr std::uint32_t kSha = 1;
constexpr std::uint32_t kChecksum = 2;
constexpr std::uint32_t kOffset = 3;
constexpr std::uint32_t kOriginalSize = 4;
constexpr std::uint32_t kCompressedSize = 5;
}  // namespace chunk_field

// ContentManifestPayload.FileMapping field numbers.
namespace file_field {
constexpr std::uint32_t kName = 1;
constexpr std::uint32_t kSize = 2;
constexpr std::uint32_t kFlags = 3;
constexpr std::uint32_t kSha = 5;
constexpr std::uint32_t kChunk = 6;
constexpr std::uint32_t kLinkTarget = 7;
}  // namespace file_field

// ContentManifestPayload and ContentManifestMetadata field numbers.
constexpr std::uint32_t kPayloadMappingField = 1;
namespace metadata_field {
constexpr std::uint32_t kDepotId = 1;
constexpr std::uint32_t kManifestId = 2;
constexpr std::uint32_t kFilenamesEncrypted = 4;
}  // namespace metadata_field

// Steam never expects a chunk below one AES block of IV plus one of data.
constexpr std::uint32_t kMinCompressedChunkSize = 32;

std::uint32_t ToChunkSize(std::uint64_t value) {
    if (value > UINT32_MAX) Fail("manifest: chunk too large");
    return static_cast<std::uint32_t>(value);
}

void CopySha(ByteSpan bytes, Sha1Hash& sha, const char* error) {
    if (bytes.size() != sha.size()) Fail(error);
    std::copy(bytes.begin(), bytes.end(), sha.begin());
}

ManifestChunk ParseChunk(ByteSpan bytes) {
    ManifestChunk chunk;
    bool has_sha = false;
    bool has_original_size = false;
    bool has_compressed_size = false;
    ProtoReader reader(bytes);
    while (auto field = reader.Next()) {
        if (field->Is(chunk_field::kSha, WireType::kLengthDelimited)) {
            CopySha(field->bytes, chunk.sha, "manifest: invalid chunk SHA-1");
            has_sha = true;
        } else if (field->Is(chunk_field::kChecksum, WireType::kFixed32)) {
            chunk.checksum = static_cast<std::uint32_t>(field->integer);
        } else if (field->Is(chunk_field::kOffset, WireType::kVarint)) {
            chunk.offset = field->integer;
        } else if (field->Is(chunk_field::kOriginalSize, WireType::kVarint)) {
            chunk.original_size = ToChunkSize(field->integer);
            has_original_size = true;
        } else if (field->Is(chunk_field::kCompressedSize, WireType::kVarint)) {
            chunk.compressed_size = ToChunkSize(field->integer);
            has_compressed_size = true;
        }
    }
    if (!has_sha || !has_original_size || !has_compressed_size || chunk.compressed_size < kMinCompressedChunkSize) {
        Fail("manifest: incomplete chunk");
    }
    return chunk;
}

ManifestFile ParseFile(ByteSpan bytes) {
    ManifestFile file;
    bool has_sha = false;
    ProtoReader reader(bytes);
    while (auto field = reader.Next()) {
        if (field->Is(file_field::kName, WireType::kLengthDelimited)) {
            file.name = AsString(field->bytes);
        } else if (field->Is(file_field::kSize, WireType::kVarint)) {
            file.size = field->integer;
        } else if (field->Is(file_field::kFlags, WireType::kVarint)) {
            file.flags = static_cast<std::uint32_t>(field->integer);
        } else if (field->Is(file_field::kSha, WireType::kLengthDelimited)) {
            CopySha(field->bytes, file.sha, "manifest: invalid file SHA-1");
            has_sha = true;
        } else if (field->Is(file_field::kChunk, WireType::kLengthDelimited)) {
            file.chunks.push_back(ParseChunk(field->bytes));
        } else if (field->Is(file_field::kLinkTarget, WireType::kLengthDelimited)) {
            file.link_target = AsString(field->bytes);
        }
    }
    if (file.IsRegular() && !has_sha) Fail("manifest: file missing SHA-1");
    return file;
}

struct Metadata {
    std::uint32_t depot = 0;
    std::uint64_t manifest_id = 0;
    bool filenames_encrypted = false;
};

Metadata ParseMetadata(ByteSpan bytes) {
    Metadata metadata;
    ProtoReader reader(bytes);
    while (auto field = reader.Next()) {
        if (field->Is(metadata_field::kDepotId, WireType::kVarint)) {
            metadata.depot = static_cast<std::uint32_t>(field->integer);
        } else if (field->Is(metadata_field::kManifestId, WireType::kVarint)) {
            metadata.manifest_id = field->integer;
        } else if (field->Is(metadata_field::kFilenamesEncrypted, WireType::kVarint)) {
            metadata.filenames_encrypted = field->integer != 0;
        }
    }
    return metadata;
}

// Decodes an encrypted name: base64 (possibly wrapped) of a symmetric ciphertext, NUL-padded.
std::string DecryptName(std::string_view encoded, const AesKey& key) {
    std::string base64;
    base64.reserve(encoded.size());
    for (char c : encoded) {
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t') base64.push_back(c);
    }
    if (base64.empty() || base64.size() % 4) Fail("manifest: invalid base64 filename");
    Bytes encrypted(base64.size() / 4 * 3);
    std::size_t length = 0;
    if (mbedtls_base64_decode(encrypted.data(), encrypted.size(), &length, AsBytes(base64).data(), base64.size()) !=
        0) {
        Fail("manifest: invalid base64 filename");
    }
    encrypted.resize(length);
    Bytes plain = SymmetricDecrypt(encrypted, key);
    while (!plain.empty() && plain.back() == 0) plain.pop_back();
    return AsString(plain);
}

// Checks that the chunks of a regular file, once sorted, cover it exactly.
void SortAndCheckChunks(ManifestFile& file) {
    if (file.size > static_cast<std::uint64_t>(INT64_MAX)) Fail("manifest: file too large");
    std::sort(file.chunks.begin(), file.chunks.end(),
              [](const ManifestChunk& a, const ManifestChunk& b) { return a.offset < b.offset; });
    std::uint64_t next = 0;
    for (const ManifestChunk& chunk : file.chunks) {
        if (chunk.offset != next || chunk.original_size > file.size - next) {
            Fail("manifest: file chunks do not cover expected size: " + file.name);
        }
        next += chunk.original_size;
    }
    if (next != file.size) Fail("manifest: file chunks do not cover expected size: " + file.name);
}

void DecodeFile(ManifestFile& file, const AesKey& key, bool encrypted) {
    if (encrypted) {
        file.name = DecryptName(file.name, key);
        if (!file.link_target.empty()) file.link_target = DecryptName(file.link_target, key);
    }
    file.name = NormalizeManifestPath(std::move(file.name));
    if (file.IsSymlink()) {
        file.link_target = NormalizeManifestPath(std::move(file.link_target));
        if (!file.chunks.empty()) Fail("manifest: symlink contains chunks");
    } else if (file.IsDirectory()) {
        if (!file.chunks.empty()) Fail("manifest: directory contains chunks");
    } else {
        SortAndCheckChunks(file);
    }
}

}  // namespace

std::vector<ManifestFile> ParseManifest(ByteSpan zip, const AesKey& key, std::uint32_t depot,
                                        std::uint64_t manifest_id) {
    Bytes binary = UnzipSingleEntry(zip);
    ByteSpan bytes(binary);
    std::vector<ManifestFile> files;
    Metadata metadata;
    bool has_payload = false;
    bool has_metadata = false;
    bool has_signature = false;
    bool has_end = false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::uint32_t magic = ReadLe32(bytes, offset);
        offset += 4;
        if (magic == kEndMagic) {
            has_end = offset == bytes.size();
            break;
        }
        std::uint32_t size = ReadLe32(bytes, offset);
        offset += 4;
        ByteSpan section = Slice(bytes, offset, size);
        offset += size;
        if (magic == kPayloadMagic && !has_payload) {
            has_payload = true;
            ProtoReader reader(section);
            while (auto field = reader.Next()) {
                if (field->Is(kPayloadMappingField, WireType::kLengthDelimited)) {
                    files.push_back(ParseFile(field->bytes));
                }
            }
        } else if (magic == kMetadataMagic && !has_metadata) {
            has_metadata = true;
            metadata = ParseMetadata(section);
        } else if (magic == kSignatureMagic && !has_signature) {
            has_signature = true;
            ProtoReader reader(section);
            while (reader.Next()) {
                // Only checked to be well-formed.
            }
        } else {
            Fail("manifest: invalid or repeated section");
        }
    }
    if (!has_payload || !has_metadata || !has_signature || !has_end || metadata.depot != depot ||
        metadata.manifest_id != manifest_id) {
        Fail("manifest: incomplete sections or wrong depot/manifest ID");
    }

    for (ManifestFile& file : files) DecodeFile(file, key, metadata.filenames_encrypted);
    return files;
}

}  // namespace pxsteamdl::detail
