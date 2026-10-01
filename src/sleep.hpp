// SPDX-License-Identifier: LGPL-3.0-or-later
// Interruptible pauses. Kept in its own file so that tests can link a fake that does not really wait.
#pragma once

#include <chrono>
#include <stop_token>

namespace pxsteamdl::detail {

// Waits for duration; returns false, possibly early, if stop was requested.
bool SleepFor(std::chrono::milliseconds duration, const std::stop_token& stop);

}  // namespace pxsteamdl::detail
