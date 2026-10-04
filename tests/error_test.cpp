// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/error.hpp"

#include <filesystem>
#include <stdexcept>
#include <system_error>

#include <gtest/gtest.h>

#include "common.hpp"
#include "eresult.hpp"

namespace pxsteamdl {
namespace {

using detail::KindOf;

TEST(ErrorTest, NamesEveryKind) {
  EXPECT_EQ(ErrorKindName(ErrorKind::kNone), "none");
  EXPECT_EQ(ErrorKindName(ErrorKind::kCancelled), "cancelled");
  EXPECT_EQ(ErrorKindName(ErrorKind::kNotFound), "not found");
  EXPECT_EQ(ErrorKindName(ErrorKind::kRejected), "rejected");
  EXPECT_EQ(ErrorKindName(ErrorKind::kNetwork), "network");
  EXPECT_EQ(ErrorKindName(ErrorKind::kData), "data");
  EXPECT_EQ(ErrorKindName(ErrorKind::kFilesystem), "filesystem");
  EXPECT_EQ(ErrorKindName(ErrorKind::kOther), "other");
}

TEST(ErrorTest, CarriesItsKindAndMessage) {
  Error error(ErrorKind::kNetwork, "no route");
  EXPECT_EQ(error.kind(), ErrorKind::kNetwork);
  EXPECT_STREQ(error.what(), "no route");
  const std::runtime_error& base = error;  // callers that only know std::runtime_error still work
  EXPECT_STREQ(base.what(), "no route");
}

TEST(ErrorTest, ClassifiesExceptions) {
  EXPECT_EQ(KindOf(Error(ErrorKind::kRejected, "x")), ErrorKind::kRejected);
  EXPECT_EQ(KindOf(detail::TransientError("x")), ErrorKind::kNetwork);
  EXPECT_EQ(KindOf(std::filesystem::filesystem_error("x", std::make_error_code(std::errc::no_such_file_or_directory))),
            ErrorKind::kFilesystem);
  EXPECT_EQ(KindOf(std::runtime_error("x")), ErrorKind::kOther);
  EXPECT_EQ(KindOf(std::bad_alloc()), ErrorKind::kOther);
}

TEST(ErrorTest, FailHelpers) {
  try {
    detail::Fail("garbled");
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.kind(), ErrorKind::kData);  // the default: data that does not make sense
  }
  try {
    detail::FailCancelled();
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.kind(), ErrorKind::kCancelled);
    EXPECT_STREQ(e.what(), "cancelled");
  }
}

TEST(ErrorTest, EResultsMapToKinds) {
  auto kind_of = [](std::int64_t code) {
    try {
      detail::FailEResult("call", code);
    } catch (const Error& e) {
      return e.kind();
    }
    return ErrorKind::kNone;
  };
  EXPECT_EQ(kind_of(9), ErrorKind::kNotFound);
  EXPECT_EQ(kind_of(15), ErrorKind::kRejected);
  EXPECT_EQ(kind_of(2), ErrorKind::kRejected);
  EXPECT_EQ(kind_of(84), ErrorKind::kNetwork);
  EXPECT_EQ(kind_of(16), ErrorKind::kNetwork);
}

}  // namespace
}  // namespace pxsteamdl
