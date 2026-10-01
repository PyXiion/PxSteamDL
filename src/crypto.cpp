// SPDX-License-Identifier: LGPL-3.0-or-later
#include "crypto.hpp"

#include <algorithm>
#include <cstddef>

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

namespace pxsteamdl::detail {

namespace {

constexpr std::size_t kAesBlockSize = 16;
constexpr unsigned kAesKeyBits = 256;

}  // namespace

Bytes SymmetricDecrypt(ByteSpan encrypted, const AesKey& key) {
  if (encrypted.size() < 2 * kAesBlockSize || encrypted.size() % kAesBlockSize) {
    Fail("AES: invalid ciphertext length");
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  std::array<std::uint8_t, kAesBlockSize> iv{};
  Bytes plain(encrypted.size() - kAesBlockSize);
  int ecb_status = mbedtls_aes_setkey_dec(&aes, key.data(), kAesKeyBits);
  if (ecb_status == 0) ecb_status = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, encrypted.data(), iv.data());
  int cbc_status = 0;
  if (ecb_status == 0) {
    cbc_status = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, plain.size(), iv.data(),
                                       encrypted.data() + kAesBlockSize, plain.data());
  }
  mbedtls_aes_free(&aes);
  if (ecb_status != 0) Fail("AES: IV decryption failed");

  std::uint8_t padding = plain.back();
  bool padding_ok = padding != 0 && padding <= kAesBlockSize &&
                    std::all_of(plain.end() - padding, plain.end(), [padding](std::uint8_t b) { return b == padding; });
  if (cbc_status != 0 || !padding_ok) Fail("AES: CBC decryption or PKCS7 padding failed");
  plain.resize(plain.size() - padding);
  return plain;
}

Sha1Hash Sha1(ByteSpan data) {
  Sha1Hash hash{};
  if (mbedtls_sha1(data.data(), data.size(), hash.data()) != 0) Fail("SHA-1 failed");
  return hash;
}

std::string ToHex(const Sha1Hash& hash) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result(2 * hash.size(), '0');
  for (std::size_t i = 0; i < hash.size(); ++i) {
    result[2 * i] = kDigits[hash[i] >> 4];
    result[2 * i + 1] = kDigits[hash[i] & 15];
  }
  return result;
}

}  // namespace pxsteamdl::detail
