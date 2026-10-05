// SPDX-License-Identifier: LGPL-3.0-or-later
// A stand-in for src/http.cpp: the unit tests link this file instead, so requests never reach the network.
#pragma once

#include <functional>
#include <string_view>

#include "http.hpp"

namespace pxsteamdl::detail::testing {

// Answers a request (the HttpConfig is not passed on, see LastHttpConfig), or throws to fail it like a transport error
// would (TransientError or std::runtime_error).
using HttpHandler = std::function<HttpResponse(std::string_view url, std::string_view method, std::string_view body)>;

// Installs the handler that HttpRequest() calls, possibly from several threads at once. Without one, every request
// throws.
void SetHttpHandler(HttpHandler handler);
void ClearHttpHandler();

// The config of the latest HttpRequest(), to check that the settings of a Client reach every request.
HttpConfig LastHttpConfig();

}  // namespace pxsteamdl::detail::testing
