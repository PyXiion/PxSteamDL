// SPDX-License-Identifier: LGPL-3.0-or-later
#include "eresult.hpp"

#include <cstdint>

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace pxsteamdl::detail
