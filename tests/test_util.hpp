// SPDX-License-Identifier: LGPL-3.0-or-later
// Builders for the formats the library decodes: ZIP archives and Steam's symmetric encryption.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <mbedtls/aes.h>
#include <zlib.h>

#include "common.hpp"
#include "crypto.hpp"

namespace pxsteamdl::detail::testing {

inline Bytes ToBytes(std::string_view text) { return Bytes(text.begin(), text.end()); }

inline void AppendLe16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline Bytes RawDeflate(ByteSpan data) {
  z_stream stream{};
  deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  Bytes out(deflateBound(&stream, static_cast<uLong>(data.size())));
  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out;
}

inline Bytes Gzip(ByteSpan data) {
  z_stream stream{};
  deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  Bytes out(deflateBound(&stream, static_cast<uLong>(data.size())) + 32);
  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out;
}

// A ZIP archive with one entry named "z", stored or deflated.
inline Bytes MakeZip(ByteSpan content, bool deflated) {
  Bytes packed = deflated ? RawDeflate(content) : Bytes(content.begin(), content.end());
  auto crc = static_cast<std::uint32_t>(crc32_z(0, content.data(), content.size()));
  std::uint16_t method = deflated ? 8 : 0;
  Bytes zip;
  // Local file header.
  AppendLe32(zip, 0x04034b50);
  AppendLe16(zip, 20);  // version needed
  AppendLe16(zip, 0);   // flags
  AppendLe16(zip, method);
  AppendLe32(zip, 0);  // time, date
  AppendLe32(zip, crc);
  AppendLe32(zip, static_cast<std::uint32_t>(packed.size()));
  AppendLe32(zip, static_cast<std::uint32_t>(content.size()));
  AppendLe16(zip, 1);  // name length
  AppendLe16(zip, 0);  // extra length
  zip.push_back('z');
  zip.insert(zip.end(), packed.begin(), packed.end());
  // Central directory.
  auto central_offset = static_cast<std::uint32_t>(zip.size());
  AppendLe32(zip, 0x02014b50);
  AppendLe16(zip, 20);  // version made by
  AppendLe16(zip, 20);  // version needed
  AppendLe16(zip, 0);   // flags
  AppendLe16(zip, method);
  AppendLe32(zip, 0);  // time, date
  AppendLe32(zip, crc);
  AppendLe32(zip, static_cast<std::uint32_t>(packed.size()));
  AppendLe32(zip, static_cast<std::uint32_t>(content.size()));
  AppendLe16(zip, 1);  // name length
  AppendLe16(zip, 0);  // extra length
  AppendLe16(zip, 0);  // comment length
  AppendLe16(zip, 0);  // disk
  AppendLe16(zip, 0);  // internal attributes
  AppendLe32(zip, 0);  // external attributes
  AppendLe32(zip, 0);  // local header offset
  zip.push_back('z');
  auto central_size = static_cast<std::uint32_t>(zip.size() - central_offset);
  // End of central directory.
  AppendLe32(zip, 0x06054b50);
  AppendLe16(zip, 0);  // this disk
  AppendLe16(zip, 0);  // central directory disk
  AppendLe16(zip, 1);  // entries on this disk
  AppendLe16(zip, 1);  // entries
  AppendLe32(zip, central_size);
  AppendLe32(zip, central_offset);
  AppendLe16(zip, 0);  // comment length
  return zip;
}

inline AesKey TestKey() {
  AesKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);
  return key;
}

// The inverse of SymmetricDecrypt: ECB-encrypted IV, then CBC with PKCS#7 padding.
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

}  // namespace pxsteamdl::detail::testing
