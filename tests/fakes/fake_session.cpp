// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fake_session.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace pxsteamdl::detail {

namespace testing {
namespace {

std::mutex g_mutex;
std::shared_ptr<SessionHandlers> g_handlers;
std::atomic<int> g_connect_count{0};

std::shared_ptr<SessionHandlers> CurrentHandlers() {
  std::lock_guard lock(g_mutex);
  return g_handlers;
}

}  // namespace

void SetSessionHandlers(SessionHandlers handlers) {
  std::lock_guard lock(g_mutex);
  g_handlers = std::make_shared<SessionHandlers>(std::move(handlers));
}

void ClearSessionHandlers() {
  std::lock_guard lock(g_mutex);
  g_handlers.reset();
  g_connect_count = 0;
}

int ConnectCount() { return g_connect_count; }

}  // namespace testing

class Session::Impl {};

Session::Session() : m_impl(std::make_unique<Impl>()) {}

Session::~Session() = default;

void Session::connect() { ++testing::g_connect_count; }

Bytes Session::rpc(std::string_view method, ByteSpan body) {
  auto handlers = testing::CurrentHandlers();
  if (!handlers || !handlers->rpc) Fail("unexpected RPC " + std::string(method));
  return handlers->rpc(method, body);
}

Bytes Session::request(std::uint32_t emsg, ByteSpan body) {
  auto handlers = testing::CurrentHandlers();
  if (!handlers || !handlers->request) Fail("unexpected request, EMsg " + std::to_string(emsg));
  return handlers->request(emsg, body);
}

}  // namespace pxsteamdl::detail
