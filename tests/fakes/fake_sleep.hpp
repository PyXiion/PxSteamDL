// SPDX-License-Identifier: LGPL-3.0-or-later
// A stand-in for src/sleep.cpp: the unit tests link this file instead, so backoff pauses take no time.
#pragma once

#include <chrono>
#include <vector>

#include "sleep.hpp"

namespace pxsteamdl::detail::testing {

// The durations SleepFor() was asked to wait since the last ForgetSleeps(), in call order.
std::vector<std::chrono::milliseconds> RecordedSleeps();
void ForgetSleeps();

}  // namespace pxsteamdl::detail::testing
