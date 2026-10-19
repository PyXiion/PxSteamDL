// SPDX-License-Identifier: LGPL-3.0-or-later
// Anonymous Steam CM session over a WebSocket.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include "common.hpp"
#include "http.hpp"

namespace pxsteamdl::detail {

// The connection lives on an io thread (executor()); the coroutine methods must run there, which is where the
// downloader's coroutines are. connect() may be called from any thread.
class Session {
 public:
  // The network settings are used for every request made through this session's owner as well; see httpConfig().
  explicit Session(HttpConfig config);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // The settings the session was made with, plus the io thread; the HTTP requests of a download use them too.
  const HttpConfig& httpConfig() const;

  // Where the session's coroutines run.
  asio::any_io_executor executor() const;

  // Logs in anonymously; a no-op while connected. Blocks the calling thread.
  void connect();

  // The same as a coroutine. rpc() and request() call it to recover from a dropped connection, so a long-lived session
  // survives Steam closing the socket between downloads.
  asio::awaitable<void> connectAsync();

  // Calls a unified service method (e.g. "ContentServerDirectory.GetManifestRequestCode#1") and returns the
  // response body; throws unless Steam reports success.
  asio::awaitable<Bytes> rpc(std::string method, Bytes body);

  // Sends a message of the given EMsg as a job and returns the body of the reply.
  asio::awaitable<Bytes> request(std::uint32_t emsg, Bytes body);

 private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace pxsteamdl::detail
