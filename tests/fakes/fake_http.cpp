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

}  // namespace

void SetHttpHandler(HttpHandler handler) {
  std::lock_guard lock(g_mutex);
  g_handler = std::make_shared<HttpHandler>(std::move(handler));
}

void ClearHttpHandler() {
  std::lock_guard lock(g_mutex);
  g_handler.reset();
}

}  // namespace testing

HttpResponse HttpRequest(std::string_view url, std::string_view method, std::string_view body,
                         std::string_view /*content_type*/) {
  std::shared_ptr<testing::HttpHandler> handler;
  {
    std::lock_guard lock(testing::g_mutex);
    handler = testing::g_handler;
  }
  if (!handler) Fail("unexpected HTTP request: " + std::string(method) + " " + std::string(url));
  return (*handler)(url, method, body);  // not under the lock: handlers run concurrently
}

}  // namespace pxsteamdl::detail
