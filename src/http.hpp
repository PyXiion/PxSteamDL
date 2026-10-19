// SPDX-License-Identifier: LGPL-3.0-or-later
// HTTP(S) requests over asio with the compiled-in CA bundle: coroutines for the downloader, a blocking wrapper for the
// rest.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "common.hpp"

namespace pxsteamdl::detail {

// Network settings of a Client (ClientOptions), used by every request and by the CM connection.
class Net;

struct HttpConfig {
  // Not supported by the asio backend: nullopt and empty both mean no proxy, anything else is refused.
  std::optional<std::string> proxy;
  long connect_timeout_seconds = 10;
  // A transfer that moves no byte for this long is aborted.
  long stall_timeout_seconds = 30;
  // The io thread and connection pool; set by Session, acquired on first use if empty.
  std::shared_ptr<Net> net;
};

struct HttpResponse {
  long status = 0;
  Bytes body;
};

// Performs a GET or POST request; throws on a transport error (a TransientError if trying again may help), not on an
// HTTP error status. Follows redirects and reuses keep-alive connections. The coroutine must run on the io thread
// (Session::executor()); its arguments are copied, so temporaries are fine.
asio::awaitable<HttpResponse> HttpRequestAsync(std::string url, HttpConfig config, std::string method = "GET",
                                               std::string body = {}, std::string content_type = {});

// The same, blocking the calling thread.
HttpResponse HttpRequest(std::string_view url, const HttpConfig& config, std::string_view method = "GET",
                         std::string_view body = {}, std::string_view content_type = {});

// Whether the server may answer differently a little later: 429 (too many requests) and the 5xx statuses.
inline bool IsTransientHttpStatus(long status) { return status == 429 || (status >= 500 && status <= 599); }

// Throws unless status is 200: a TransientError for 429 and 5xx, an Error of kind kRejected otherwise.
inline void CheckHttpStatus(std::string_view what, long status) {
  if (status == 200) return;
  std::string message = std::string(what) + ": HTTP " + std::to_string(status);
  if (IsTransientHttpStatus(status)) FailTransient(message);
  Fail(ErrorKind::kRejected, message);
}

}  // namespace pxsteamdl::detail
