// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fake_session.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <asio.hpp>

#include "fake_pool.hpp"

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

// The coroutines of a download run on this io thread, as they do on the real session's.
class Session::Impl {
 public:
  explicit Impl(HttpConfig config) : m_config(std::move(config)), m_work(asio::make_work_guard(m_io)) {
    m_thread = std::thread([this] { m_io.run(); });
  }
  ~Impl() {
    m_work.reset();
    m_thread.join();
  }
  HttpConfig m_config;
  asio::io_context m_io{1};
  asio::executor_work_guard<asio::io_context::executor_type> m_work;
  std::thread m_thread;
};

Session::Session(HttpConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {}

Session::~Session() = default;

const HttpConfig& Session::httpConfig() const { return m_impl->m_config; }

asio::any_io_executor Session::executor() const { return m_impl->m_io.get_executor(); }

void Session::connect() { ++testing::g_connect_count; }

asio::awaitable<void> Session::connectAsync() { co_return; }

asio::awaitable<Bytes> Session::rpc(std::string method, Bytes body) {
  auto handlers = testing::CurrentHandlers();
  if (!handlers || !handlers->rpc) Fail("unexpected RPC " + method);
  // Handlers may block, so they run on the fakes' pool.
  co_return co_await asio::co_spawn(
      testing::FakePool(),
      [&]() -> asio::awaitable<Bytes> { co_return handlers->rpc(method, body); }, asio::use_awaitable);
}

asio::awaitable<Bytes> Session::request(std::uint32_t emsg, Bytes body) {
  auto handlers = testing::CurrentHandlers();
  if (!handlers || !handlers->request) Fail("unexpected request, EMsg " + std::to_string(emsg));
  co_return co_await asio::co_spawn(
      testing::FakePool(),
      [&]() -> asio::awaitable<Bytes> { co_return handlers->request(emsg, body); }, asio::use_awaitable);
}

}  // namespace pxsteamdl::detail
