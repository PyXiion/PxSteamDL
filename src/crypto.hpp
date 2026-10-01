// SPDX-License-Identifier: LGPL-3.0-or-later
// Steam's symmetric encryption and SHA-1 helpers.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "common.hpp"

namespace pxsteamdl::detail {

using Sha1Hash = std::array<std::uint8_t, 20>;
using AesKey = std::array<std::uint8_t, 32>;

// Decrypts Steam's symmetric format: a 16-byte AES-256-ECB encrypted IV followed by AES-256-CBC data with PKCS#7
// padding. Throws on a malformed ciphertext or padding.
Bytes SymmetricDecrypt(ByteSpan encrypted, const AesKey& key);

Sha1Hash Sha1(ByteSpan data);

// Lowercase hexadecimal digits of the hash.
std::string ToHex(const Sha1Hash& hash);

}  // namespace pxsteamdl::detail
