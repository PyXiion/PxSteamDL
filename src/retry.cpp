// SPDX-License-Identifier: LGPL-3.0-or-later
#include "retry.hpp"

#include <algorithm>
#include <random>

namespace pxsteamdl::detail {

namespace {

constexpr std::chrono::milliseconds kInitialDelay{500};
constexpr std::chrono::milliseconds kMaxDelay{8000};

}  // namespace

std::chrono::milliseconds BackoffDelay(int failed_attempts) {
  thread_local std::mt19937 random(std::random_device{}());
  // Capped before shifting: the exponent only matters up to the cap.
  int doublings = std::clamp(failed_attempts - 1, 0, 10);
  std::chrono::milliseconds full = std::min(kInitialDelay * (1 << doublings), kMaxDelay);
  std::uniform_int_distribution<std::chrono::milliseconds::rep> jitter(0, full.count() / 2);
  return full - std::chrono::milliseconds(jitter(random));
}

}  // namespace pxsteamdl::detail
