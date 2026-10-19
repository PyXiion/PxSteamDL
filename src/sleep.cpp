// SPDX-License-Identifier: LGPL-3.0-or-later
#include "sleep.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>

#include <asio.hpp>

namespace pxsteamdl::detail {

bool SleepFor(std::chrono::milliseconds duration, const std::stop_token& stop) {
  std::mutex mutex;
  std::condition_variable wake;
  // Registered before the lock is taken: if stop was requested already, the callback runs right here.
  std::stop_callback on_stop(stop, [&] {
    std::lock_guard lock(mutex);  // the sleeper is either waiting (mutex released) or about to check the predicate
    wake.notify_all();
  });
  std::unique_lock lock(mutex);
  return !wake.wait_for(lock, duration, [&] { return stop.stop_requested(); });
}

asio::awaitable<bool> SleepForAsync(std::chrono::milliseconds duration, std::stop_token stop) {
  auto executor = co_await asio::this_coro::executor;
  auto timer = std::make_shared<asio::steady_timer>(executor, duration);
  // The callback may run on any thread, so it only posts the cancellation to the executor.
  std::stop_callback on_stop(stop, [timer, executor] { asio::post(executor, [timer] { timer->cancel(); }); });
  std::error_code cancelled;
  co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, cancelled));
  co_return !stop.stop_requested();
}

}  // namespace pxsteamdl::detail
