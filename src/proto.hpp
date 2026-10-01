// SPDX-License-Identifier: LGPL-3.0-or-later
// Minimal protobuf wire-format encoding and decoding, enough for the Steam messages used here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

#include "common.hpp"

namespace pxsteamdl::detail {

enum class WireType : std::uint32_t {
  kVarint = 0,
  kFixed64 = 1,
  kLengthDelimited = 2,
  kFixed32 = 5,
};

struct ProtoField {
  std::uint32_t number = 0;
  WireType wire_type = WireType::kVarint;
  // Value of a varint or fixed-width field.
  std::uint64_t integer = 0;
  // Contents of a length-delimited field; points into the decoded input.
  ByteSpan bytes;

  bool is(std::uint32_t field_number, WireType type) const { return number == field_number && wire_type == type; }
};

// Iterates over the fields of one encoded message.
class ProtoReader {
 public:
  explicit ProtoReader(ByteSpan input) : m_input(input) {}

  // Returns the next field, or nullopt at the end of the input; throws on malformed input.
  std::optional<ProtoField> next();

 private:
  std::uint64_t readVarint();
  std::uint64_t readFixed(std::size_t size);
  ByteSpan take(std::size_t size);

  ByteSpan m_input;
  std::size_t m_offset = 0;
};

Bytes EncodeUint(std::uint32_t field, std::uint64_t value);
Bytes EncodeFixed64(std::uint32_t field, std::uint64_t value);
Bytes EncodeBytes(std::uint32_t field, ByteSpan value);
Bytes EncodeString(std::uint32_t field, std::string_view value);

// Concatenates encoded fields into one message.
Bytes Concat(std::initializer_list<ByteSpan> parts);

}  // namespace pxsteamdl::detail
