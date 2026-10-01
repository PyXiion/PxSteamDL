// SPDX-License-Identifier: LGPL-3.0-or-later
#include <string>

#include <gtest/gtest.h>

#include "pxsteamdl/pxsteamdl.hpp"

namespace pxsteamdl {
namespace {

TEST(VersionTest, StringMatchesTheNumbers) {
  std::string expected = std::to_string(PXSTEAMDL_VERSION_MAJOR) + "." + std::to_string(PXSTEAMDL_VERSION_MINOR) + "." +
                         std::to_string(PXSTEAMDL_VERSION_PATCH);
  EXPECT_EQ(std::string(PXSTEAMDL_VERSION_STRING), expected);
  EXPECT_EQ(Version(), expected);
}

}  // namespace
}  // namespace pxsteamdl
