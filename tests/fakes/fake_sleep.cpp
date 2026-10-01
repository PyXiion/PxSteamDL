// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fake_sleep.hpp"

#include <mutex>

namespace pxsteamdl::detail {

namespace testing {
namespace {

std::mutex g_mutex;
std::vector<std::chrono::milliseconds> g_sleeps;

}  // namespace

std::vector<std::chrono::milliseconds> RecordedSleeps() {
  std::lock_guard lock(g_mutex);
  return g_sleeps;
}

void ForgetSleeps() {
  std::lock_guard lock(g_mutex);
  g_sleeps.clear();
}

}  // namespace testing

bool SleepFor(std::chrono::milliseconds duration, const std::stop_token& stop) {
  {
    std::lock_guard lock(testing::g_mutex);
    testing::g_sleeps.push_back(duration);
  }
  return !stop.stop_requested();
}

}  // namespace pxsteamdl::detail
