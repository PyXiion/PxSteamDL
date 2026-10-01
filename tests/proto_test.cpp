// SPDX-License-Identifier: LGPL-3.0-or-later
#include "proto.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace pxsteamdl::detail {
namespace {

TEST(ProtoTest, RoundTripsEveryEncodedType) {
  Bytes message = Concat(
      {EncodeUint(1, 300), EncodeFixed64(2, 0x0123456789ABCDEFULL), EncodeString(3, "hi"), EncodeUint(4, ~0ULL)});
  ProtoReader reader(message);

  auto field = reader.next();
  ASSERT_TRUE(field);
  EXPECT_TRUE(field->is(1, WireType::kVarint));
  EXPECT_EQ(field->integer, 300u);

  field = reader.next();
  ASSERT_TRUE(field);
  EXPECT_TRUE(field->is(2, WireType::kFixed64));
  EXPECT_EQ(field->integer, 0x0123456789ABCDEFULL);

  field = reader.next();
  ASSERT_TRUE(field);
  EXPECT_TRUE(field->is(3, WireType::kLengthDelimited));
  EXPECT_EQ(AsString(field->bytes), "hi");

  field = reader.next();
  ASSERT_TRUE(field);
  EXPECT_EQ(field->integer, ~0ULL);

  EXPECT_FALSE(reader.next());
}

TEST(ProtoTest, EncodesVarintsLikeProtobuf) { EXPECT_EQ(EncodeUint(1, 150), (Bytes{0x08, 0x96, 0x01})); }

TEST(ProtoTest, ReadsFixed32) {
  Bytes message{(7 << 3) | 5, 0x78, 0x56, 0x34, 0x12};
  ProtoReader reader(message);
  auto field = reader.next();
  ASSERT_TRUE(field);
  EXPECT_TRUE(field->is(7, WireType::kFixed32));
  EXPECT_EQ(field->integer, 0x12345678u);
}

TEST(ProtoTest, RejectsTruncatedVarint) {
  Bytes message{0x08, 0x96};
  ProtoReader reader(message);
  EXPECT_THROW(reader.next(), std::runtime_error);
}

TEST(ProtoTest, RejectsTruncatedLengthDelimitedField) {
  Bytes message = EncodeString(1, "hello");
  message.pop_back();
  ProtoReader reader(message);
  EXPECT_THROW(reader.next(), std::runtime_error);
}

TEST(ProtoTest, RejectsUnsupportedWireType) {
  Bytes message{(1 << 3) | 3};  // start group
  ProtoReader reader(message);
  EXPECT_THROW(reader.next(), std::runtime_error);
}

}  // namespace
}  // namespace pxsteamdl::detail
