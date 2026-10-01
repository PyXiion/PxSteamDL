// SPDX-License-Identifier: LGPL-3.0-or-later
#include "crypto.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

#include "test_util.hpp"

namespace pxsteamdl::detail {
namespace {

using testing::SymmetricEncrypt;
using testing::TestKey;
using testing::ToBytes;

TEST(CryptoTest, DecryptsSymmetricCiphertext) {
    for (std::size_t size : {0u, 1u, 15u, 16u, 17u, 100u}) {
        Bytes plain(size, 0x5A);
        EXPECT_EQ(SymmetricDecrypt(SymmetricEncrypt(plain, TestKey()), TestKey()), plain) << size;
    }
}

TEST(CryptoTest, RejectsInvalidLength) {
    Bytes encrypted = SymmetricEncrypt(ToBytes("text"), TestKey());
    encrypted.pop_back();
    EXPECT_THROW(SymmetricDecrypt(encrypted, TestKey()), std::runtime_error);
    EXPECT_THROW(SymmetricDecrypt(Bytes(16), TestKey()), std::runtime_error);
}

TEST(CryptoTest, RejectsWrongKey) {
    AesKey other = TestKey();
    other[0] ^= 1;
    // With overwhelming probability the padding of a wrong decryption is invalid; this ciphertext's is.
    EXPECT_THROW(SymmetricDecrypt(SymmetricEncrypt(ToBytes("some text"), TestKey()), other), std::runtime_error);
}

TEST(CryptoTest, HashesAndFormatsSha1) {
    EXPECT_EQ(ToHex(Sha1(ToBytes("abc"))), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

}  // namespace
}  // namespace pxsteamdl::detail
