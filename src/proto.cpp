// SPDX-License-Identifier: LGPL-3.0-or-later
#include "proto.hpp"

namespace pxsteamdl::detail {

namespace {

constexpr int kVarintMaxShift = 64;
constexpr std::uint8_t kVarintContinuation = 0x80;
constexpr std::uint8_t kVarintPayloadMask = 0x7F;
constexpr int kWireTypeBits = 3;
constexpr std::uint64_t kWireTypeMask = 7;

void PutVarint(Bytes& out, std::uint64_t value) {
    while (value >= kVarintContinuation) {
        out.push_back(static_cast<std::uint8_t>(value | kVarintContinuation));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void PutLittleEndian(Bytes& out, std::uint64_t value, int size) {
    for (int i = 0; i < size; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Bytes Tag(std::uint32_t field, WireType type) {
    Bytes out;
    PutVarint(out, (std::uint64_t{field} << kWireTypeBits) | static_cast<std::uint32_t>(type));
    return out;
}

}  // namespace

Bytes EncodeUint(std::uint32_t field, std::uint64_t value) {
    Bytes out = Tag(field, WireType::kVarint);
    PutVarint(out, value);
    return out;
}

Bytes EncodeFixed64(std::uint32_t field, std::uint64_t value) {
    Bytes out = Tag(field, WireType::kFixed64);
    PutLittleEndian(out, value, 8);
    return out;
}

Bytes EncodeBytes(std::uint32_t field, ByteSpan value) {
    Bytes out = Tag(field, WireType::kLengthDelimited);
    PutVarint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

Bytes EncodeString(std::uint32_t field, std::string_view value) { return EncodeBytes(field, AsBytes(value)); }

Bytes Concat(std::initializer_list<ByteSpan> parts) {
    Bytes out;
    for (ByteSpan part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

std::optional<ProtoField> ProtoReader::Next() {
    if (offset_ >= input_.size()) return std::nullopt;
    std::uint64_t key = ReadVarint();
    ProtoField field;
    field.number = static_cast<std::uint32_t>(key >> kWireTypeBits);
    field.wire_type = static_cast<WireType>(key & kWireTypeMask);
    switch (field.wire_type) {
        case WireType::kVarint:
            field.integer = ReadVarint();
            break;
        case WireType::kFixed64:
            field.integer = ReadFixed(8);
            break;
        case WireType::kLengthDelimited:
            field.bytes = Take(ReadVarint());
            break;
        case WireType::kFixed32:
            field.integer = ReadFixed(4);
            break;
        default:
            Fail("protobuf: unsupported wire type");
    }
    return field;
}

std::uint64_t ProtoReader::ReadVarint() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < kVarintMaxShift; shift += 7) {
        if (offset_ >= input_.size()) Fail("protobuf: truncated varint");
        std::uint8_t byte = input_[offset_++];
        value |= static_cast<std::uint64_t>(byte & kVarintPayloadMask) << shift;
        if (!(byte & kVarintContinuation)) return value;
    }
    Fail("protobuf: varint too long");
}

std::uint64_t ProtoReader::ReadFixed(std::size_t size) {
    ByteSpan raw = Take(size);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value |= std::uint64_t{raw[i]} << (8 * i);
    return value;
}

ByteSpan ProtoReader::Take(std::size_t size) {
    if (input_.size() - offset_ < size) Fail("protobuf: truncated field");
    ByteSpan part = input_.subspan(offset_, size);
    offset_ += size;
    return part;
}

}  // namespace pxsteamdl::detail
