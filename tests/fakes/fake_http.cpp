// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fake_http.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace pxsteamdl::detail {

namespace testing {
namespace {

std::mutex g_mutex;
std::shared_ptr<HttpHandler> g_handler;
HttpConfig g_last_config;

}  // namespace

void SetHttpHandler(HttpHandler handler) {
  std::lock_guard lock(g_mutex);
  g_handler = std::make_shared<HttpHandler>(std::move(handler));
}

HttpConfig LastHttpConfig() {
  std::lock_guard lock(g_mutex);
  return g_last_config;
}

void ClearHttpHandler() {
  std::lock_guard lock(g_mutex);
  g_handler.reset();
}

}  // namespace testing

HttpResponse HttpRequest(std::string_view url, const HttpConfig& config, std::string_view method, std::string_view body,
                         std::string_view /*content_type*/) {
  std::shared_ptr<testing::HttpHandler> handler;
  {
    std::lock_guard lock(testing::g_mutex);
    handler = testing::g_handler;
    testing::g_last_config = config;
  }
  if (!handler) Fail("unexpected HTTP request: " + std::string(method) + " " + std::string(url));
  return (*handler)(url, method, body);  // not under the lock: handlers run concurrently
}

}  // namespace pxsteamdl::detail
