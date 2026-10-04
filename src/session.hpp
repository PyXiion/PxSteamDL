// SPDX-License-Identifier: LGPL-3.0-or-later
// Anonymous Steam CM session over a WebSocket.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "common.hpp"
#include "http.hpp"

namespace pxsteamdl::detail {

// Thread-safe: calls from several threads share one connection.
class Session {
 public:
  // The network settings are used for every request made through this session's owner as well; see httpConfig().
  explicit Session(HttpConfig config);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // The settings the session was made with; the HTTP requests of a download use them too.
  const HttpConfig& httpConfig() const;

  // Logs in anonymously; a no-op while connected. rpc() and request() call it to recover from a dropped
  // connection, so a long-lived session survives Steam closing the socket between downloads.
  void connect();

  // Calls a unified service method (e.g. "ContentServerDirectory.GetManifestRequestCode#1") and returns the
  // response body; throws unless Steam reports success.
  Bytes rpc(std::string_view method, ByteSpan body);

  // Sends a message of the given EMsg as a job and returns the body of the reply.
  Bytes request(std::uint32_t emsg, ByteSpan body);

 private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace pxsteamdl::detail
