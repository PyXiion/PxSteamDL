// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace pxsteamdl {

// Why an operation or an item failed, coarse enough to decide what to do about it. The message that goes with it
// (Result::error, Error::what()) is for people and may change between versions; the kind is part of the API.
enum class ErrorKind {
  // No error.
  kNone,
  // Stopped on request (Options::stop). The message is exactly "cancelled".
  kCancelled,
  // Steam does not have the item: deleted, never existed, or not visible to an anonymous user.
  kNotFound,
  // Steam or a CDN refused: the item is private or not downloadable anonymously, a key or a download was denied,
  // an HTTP 4xx came back.
  kRejected,
  // The network or the service failed (no connection, timeouts, HTTP 429/5xx, Steam busy) and still did after
  // the automatic retries. Trying again later may work.
  kNetwork,
  // Steam sent data that cannot be used: a malformed answer, a checksum or decoding failure, a manifest with an
  // unsafe path.
  kData,
  // A local file or directory could not be written, read or replaced.
  kFilesystem,
  // Anything else, including bugs.
  kOther,
};

// "none", "cancelled", "not found", "rejected", "network", "data", "filesystem" or "other".
std::string_view ErrorKindName(ErrorKind kind);

// What Client's constructor throws on failure.
class Error : public std::runtime_error {
 public:
  Error(ErrorKind kind, const std::string& message) : std::runtime_error(message), m_kind(kind) {}

  ErrorKind kind() const noexcept { return m_kind; }

 private:
  ErrorKind m_kind;
};

}  // namespace pxsteamdl
