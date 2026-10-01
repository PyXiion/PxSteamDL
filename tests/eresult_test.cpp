// SPDX-License-Identifier: LGPL-3.0-or-later
#include "eresult.hpp"

#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "common.hpp"

namespace pxsteamdl::detail {
namespace {

TEST(EResultTest, DescribesKnownCodes) {
  EXPECT_EQ(DescribeEResult(9), "not found (EResult 9)");
  EXPECT_EQ(EResultDescription(15), "access denied");
  EXPECT_EQ(EResultDescription(84), "rate limit exceeded");
  EXPECT_EQ(EResultDescription(kEResultOk), "success");
}

TEST(EResultTest, DescribesUnknownCodes) {
  EXPECT_EQ(DescribeEResult(4), "unknown result (EResult 4)");
  EXPECT_EQ(DescribeEResult(-1), "unknown result (EResult -1)");
  // Out of the enum's range, even if the low bits name a known code.
  EXPECT_EQ(EResultDescription((std::int64_t{1} << 32) + 9), "unknown result");
}

TEST(EResultTest, ClassifiesTransientCodes) {
  for (std::int64_t code : {3, 10, 16, 20, 48, 84, 103}) EXPECT_TRUE(IsTransientEResult(code)) << code;
  for (std::int64_t code : {1, 2, 9, 15, 17, 43, 0, 12345}) EXPECT_FALSE(IsTransientEResult(code)) << code;
  EXPECT_FALSE(IsTransientEResult((std::int64_t{1} << 32) + 84));
}

TEST(EResultTest, FailEResultThrowsTheRightKindOfError) {
  try {
    FailEResult("RPC failed", 84);
    FAIL() << "expected an exception";
  } catch (const TransientError& e) {
    EXPECT_STREQ(e.what(), "RPC failed: rate limit exceeded (EResult 84)");
  }
  try {
    FailEResult("RPC failed", 15, "no such app");
    FAIL() << "expected an exception";
  } catch (const TransientError&) {
    FAIL() << "access denied must not be transient";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), "RPC failed: access denied (EResult 15) (no such app)");
  }
}

}  // namespace
}  // namespace pxsteamdl::detail
