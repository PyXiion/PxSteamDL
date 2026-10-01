// SPDX-License-Identifier: LGPL-3.0-or-later
// Blocking HTTPS requests over libcurl with the compiled-in CA bundle.
#pragma once

#include <string_view>

#include "common.hpp"

namespace pxsteamdl::detail {

struct HttpResponse {
    long status = 0;
    Bytes body;
};

// Performs a GET or POST request; throws on a transport error, not on an HTTP error status.
// Reuses one connection cache per thread.
HttpResponse HttpRequest(std::string_view url, std::string_view method = "GET", std::string_view body = {},
                         std::string_view content_type = {});

// Makes a curl easy handle (CURL*) trust the Mozilla CA bundle compiled into the library; mbedTLS has no access
// to the OS trust store. Must be set again after curl_easy_reset().
void SetCaBundle(void* curl);

}  // namespace pxsteamdl::detail
