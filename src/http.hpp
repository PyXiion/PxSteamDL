// SPDX-License-Identifier: LGPL-3.0-or-later
// Blocking HTTPS requests over libcurl with the compiled-in CA bundle.
#pragma once

#include <string>
#include <string_view>

#include "common.hpp"

namespace pxsteamdl::detail {

struct HttpResponse {
  long status = 0;
  Bytes body;
};

// Performs a GET or POST request; throws on a transport error (a TransientError if trying again may help), not on an
// HTTP error status. Reuses one connection cache per thread.
HttpResponse HttpRequest(std::string_view url, std::string_view method = "GET", std::string_view body = {},
                         std::string_view content_type = {});

// Whether the server may answer differently a little later: 429 (too many requests) and the 5xx statuses.
inline bool IsTransientHttpStatus(long status) { return status == 429 || (status >= 500 && status <= 599); }

// Throws unless status is 200: a TransientError for 429 and 5xx, a std::runtime_error otherwise.
inline void CheckHttpStatus(std::string_view what, long status) {
  if (status == 200) return;
  std::string message = std::string(what) + ": HTTP " + std::to_string(status);
  if (IsTransientHttpStatus(status)) FailTransient(message);
  Fail(message);
}

// Makes a curl easy handle (CURL*) trust the Mozilla CA bundle compiled into the library; mbedTLS has no access
// to the OS trust store. Must be set again after curl_easy_reset().
void SetCaBundle(void* curl);

}  // namespace pxsteamdl::detail
