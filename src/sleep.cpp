// SPDX-License-Identifier: LGPL-3.0-or-later
#include "sleep.hpp"

#include <condition_variable>
#include <mutex>

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

}  // namespace pxsteamdl::detail
