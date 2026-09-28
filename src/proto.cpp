// SPDX-License-Identifier: LGPL-3.0-or-later
#include "internal.hpp"

namespace pxsteamdl::detail {

namespace {

void put_varint(Bytes& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_le(Bytes& out, std::uint64_t value, int size) {
    for (int i = 0; i < size; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Bytes tagged(std::uint32_t field, std::uint32_t wire) {
    Bytes out;
    put_varint(out, (std::uint64_t{field} << 3) | wire);
    return out;
}

} // namespace

Bytes encode_uint(std::uint32_t field, std::uint64_t value) {
    Bytes out = tagged(field, 0);
    put_varint(out, value);
    return out;
}

Bytes encode_fixed32(std::uint32_t field, std::uint32_t value) {
    Bytes out = tagged(field, 5);
    put_le(out, value, 4);
    return out;
}

Bytes encode_fixed64(std::uint32_t field, std::uint64_t value) {
    Bytes out = tagged(field, 1);
    put_le(out, value, 8);
    return out;
}

Bytes encode_bytes(std::uint32_t field, std::span<const std::uint8_t> value) {
    Bytes out = tagged(field, 2);
    put_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

Bytes encode_string(std::uint32_t field, std::string_view value) {
    return encode_bytes(field, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

Bytes concat(std::initializer_list<std::span<const std::uint8_t>> parts) {
    Bytes out;
    for (auto part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

bool Reader::next(Field& field) {
    if (offset_ >= input_.size()) return false;
    auto varint = [&] {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (offset_ >= input_.size()) throw std::runtime_error("protobuf: truncated varint");
            std::uint8_t byte = input_[offset_++];
            value |= std::uint64_t{byte & 0x7Fu} << shift;
            if (!(byte & 0x80)) return value;
        }
        throw std::runtime_error("protobuf: varint too long");
    };
    auto take = [&](std::size_t size) {
        if (input_.size() - offset_ < size) throw std::runtime_error("protobuf: truncated field");
        auto part = input_.subspan(offset_, size);
        offset_ += size;
        return part;
    };
    auto fixed = [&](int size) {
        std::uint64_t value = 0;
        auto raw = take(size);
        for (int i = 0; i < size; ++i) value |= std::uint64_t{raw[i]} << (8 * i);
        return value;
    };

    std::uint64_t key = varint();
    field = {static_cast<std::uint32_t>(key >> 3), static_cast<std::uint32_t>(key & 7), 0, {}};
    switch (field.wire) {
    case 0: field.integer = varint(); break;
    case 1: field.integer = fixed(8); break;
    case 2: field.bytes = take(varint()); break;
    case 5: field.integer = fixed(4); break;
    default: throw std::runtime_error("protobuf: unsupported wire type");
    }
    return true;
}

} // namespace pxsteamdl::detail
