// SPDX-License-Identifier: LGPL-3.0-or-later
// Small coroutine helpers shared by the downloader and the session.
#pragma once

#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>

#include <asio.hpp>

namespace pxsteamdl::detail {

// A condition variable for coroutines on one thread: wait() until another coroutine calls notify(). Nothing is
// remembered, so a waiter re-checks its condition after every wake-up, as with std::condition_variable.
class Signal {
 public:
  explicit Signal(const asio::any_io_executor& executor) : m_timer(executor, std::chrono::steady_clock::time_point::max()) {}
  asio::awaitable<void> wait() {
    std::error_code woken;
    co_await m_timer.async_wait(asio::redirect_error(asio::use_awaitable, woken));
  }
  void notify() { m_timer.cancel(); }

 private:
  asio::steady_timer m_timer;
};

// A mutex for coroutines on one thread: lock() waits without blocking the thread.
class AsyncMutex {
 public:
  explicit AsyncMutex(const asio::any_io_executor& executor) : m_free(executor) {}
  asio::awaitable<void> lock() {
    while (m_locked) co_await m_free.wait();
    m_locked = true;
  }
  void unlock() {
    m_locked = false;
    m_free.notify();
  }
  // Unlocks at the end of the scope that took the lock.
  class Guard {
   public:
    explicit Guard(AsyncMutex& mutex) : m_mutex(mutex) {}
    ~Guard() { m_mutex.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    AsyncMutex& m_mutex;
  };

 private:
  bool m_locked = false;
  Signal m_free;
};

// Runs work() on pool and continues on the calling coroutine's executor; an exception it throws comes out here.
template <class Pool, class Work>
auto OnPool(Pool& pool, Work work) -> asio::awaitable<std::invoke_result_t<Work>> {
  co_return co_await asio::co_spawn(
      pool, [&work]() -> asio::awaitable<std::invoke_result_t<Work>> { co_return work(); }, asio::use_awaitable);
}

}  // namespace pxsteamdl::detail
