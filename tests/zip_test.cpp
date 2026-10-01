// SPDX-License-Identifier: LGPL-3.0-or-later
#include "zip.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "test_util.hpp"

namespace pxsteamdl::detail {
namespace {

using testing::MakeZip;
using testing::ToBytes;

const Bytes kContent = ToBytes(std::string(1000, 'a') + "some text that compresses");

TEST(ZipTest, ExtractsStoredEntry) { EXPECT_EQ(UnzipSingleEntry(MakeZip(kContent, false)), kContent); }

TEST(ZipTest, ExtractsDeflatedEntry) { EXPECT_EQ(UnzipSingleEntry(MakeZip(kContent, true)), kContent); }

TEST(ZipTest, ExtractsEmptyEntry) {
    EXPECT_TRUE(UnzipSingleEntry(MakeZip({}, false)).empty());
    EXPECT_TRUE(UnzipSingleEntry(MakeZip({}, true)).empty());
}

TEST(ZipTest, RejectsCrcMismatch) {
    Bytes zip = MakeZip(kContent, false);
    zip[31] ^= 1;  // first content byte, after the 30-byte local header and the 1-byte name
    EXPECT_THROW(UnzipSingleEntry(zip), std::runtime_error);
}

TEST(ZipTest, RejectsTruncatedArchive) {
    Bytes zip = MakeZip(kContent, true);
    EXPECT_THROW(UnzipSingleEntry(ByteSpan(zip).first(zip.size() - 1)), std::runtime_error);
    EXPECT_THROW(UnzipSingleEntry(ByteSpan(zip).first(10)), std::runtime_error);
}

TEST(ZipTest, RejectsEncryptedEntry) {
    Bytes zip = MakeZip(kContent, false);
    std::size_t central = ReadLe32(zip, zip.size() - 6);
    zip[central + 8] |= 1;
    EXPECT_THROW(UnzipSingleEntry(zip), std::runtime_error);
}

}  // namespace
}  // namespace pxsteamdl::detail
