// SPDX-License-Identifier: LGPL-3.0-or-later
// Byte buffers, bounds-checked little-endian reads and error helpers shared by the implementation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pxsteamdl/error.hpp"

namespace pxsteamdl::detail {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;

// Result::error of an item stopped through Options::stop; part of the public contract.
inline constexpr char kCancelled[] = "cancelled";

// Every failure the library raises is an Error, which carries the kind a Result reports.
// Fail() without a kind is for data that does not make sense (the common case in the decoders).
[[noreturn]] inline void Fail(ErrorKind kind, const std::string& message) { throw Error(kind, message); }
[[noreturn]] inline void Fail(const std::string& message) { Fail(ErrorKind::kData, message); }
[[noreturn]] inline void FailCancelled() { Fail(ErrorKind::kCancelled, kCancelled); }

// An error that may disappear on its own (a dropped connection, a busy server): worth retrying after a pause.
class TransientError : public Error {
 public:
  explicit TransientError(const std::string& message) : Error(ErrorKind::kNetwork, message) {}
};

[[noreturn]] inline void FailTransient(const std::string& message) { throw TransientError(message); }

// The kind of an exception caught at the boundary of an item: its own for an Error, a filesystem error for what
// std::filesystem throws, otherwise kOther.
inline ErrorKind KindOf(const std::exception& e) {
  if (const auto* error = dynamic_cast<const Error*>(&e)) return error->kind();
  if (dynamic_cast<const std::filesystem::filesystem_error*>(&e)) return ErrorKind::kFilesystem;
  return ErrorKind::kOther;
}

inline ByteSpan AsBytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

inline std::string AsString(ByteSpan bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

// Returns size bytes at pos; throws if they are not all within bytes.
inline ByteSpan Slice(ByteSpan bytes, std::size_t pos, std::size_t size) {
  if (pos > bytes.size() || size > bytes.size() - pos) Fail("truncated binary data");
  return bytes.subspan(pos, size);
}

inline std::uint16_t ReadLe16(ByteSpan bytes, std::size_t pos) {
  ByteSpan raw = Slice(bytes, pos, 2);
  return static_cast<std::uint16_t>(raw[0] | (raw[1] << 8));
}

inline std::uint32_t ReadLe32(ByteSpan bytes, std::size_t pos) {
  ByteSpan raw = Slice(bytes, pos, 4);
  return std::uint32_t{raw[0]} | (std::uint32_t{raw[1]} << 8) | (std::uint32_t{raw[2]} << 16) |
         (std::uint32_t{raw[3]} << 24);
}

inline void AppendLe32(Bytes& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

}  // namespace pxsteamdl::detail
