// SPDX-License-Identifier: LGPL-3.0-or-later
// Retrying requests that failed for a transient reason, with exponential backoff.
#pragma once

#include <chrono>
#include <stop_token>

#include "common.hpp"
#include "sleep.hpp"

namespace pxsteamdl::detail {

// Attempts made by Retry() before the last TransientError is passed on.
inline constexpr int kMaxAttempts = 4;

// The pause after the given number of failed attempts (1 for the first): 0.5 s, 1 s, 2 s, ... up to 8 s, each
// randomly shortened by up to half so that workers that failed together do not retry together.
std::chrono::milliseconds BackoffDelay(int failed_attempts);

// Calls attempt() until it returns, retrying after a TransientError with a pause in between. Any other exception
// ends it at once, and so does the last of kMaxAttempts failures. Throws kCancelled instead of waiting or trying
// again once stop is requested.
template <class Attempt>
auto Retry(const std::stop_token& stop, Attempt attempt) -> decltype(attempt()) {
  for (int failed_attempts = 1;; ++failed_attempts) {
    if (stop.stop_requested()) FailCancelled();
    try {
      return attempt();
    } catch (const TransientError&) {
      if (failed_attempts >= kMaxAttempts) throw;
      if (!SleepFor(BackoffDelay(failed_attempts), stop)) FailCancelled();
    }
  }
}

}  // namespace pxsteamdl::detail
