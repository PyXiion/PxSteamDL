// SPDX-License-Identifier: LGPL-3.0-or-later
#include "sleep.hpp"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace pxsteamdl::detail {
namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

TEST(SleepTest, WaitsForTheDuration) {
  auto start = steady_clock::now();
  EXPECT_TRUE(SleepFor(milliseconds(80), {}));
  EXPECT_GE(steady_clock::now() - start, milliseconds(75));
}

TEST(SleepTest, ReturnsAtOnceIfStopWasRequested) {
  std::stop_source stop;
  stop.request_stop();
  auto start = steady_clock::now();
  EXPECT_FALSE(SleepFor(std::chrono::seconds(30), stop.get_token()));
  EXPECT_LT(steady_clock::now() - start, std::chrono::seconds(5));
}

TEST(SleepTest, WakesUpWhenStopIsRequested) {
  std::stop_source stop;
  std::thread requester([&] {
    std::this_thread::sleep_for(milliseconds(50));
    stop.request_stop();
  });
  auto start = steady_clock::now();
  EXPECT_FALSE(SleepFor(std::chrono::seconds(30), stop.get_token()));
  EXPECT_LT(steady_clock::now() - start, std::chrono::seconds(5));
  requester.join();
}

TEST(SleepTest, StopThatIsNeverRequestedDoesNotShortenTheWait) {
  std::stop_source stop;
  auto start = steady_clock::now();
  EXPECT_TRUE(SleepFor(milliseconds(50), stop.get_token()));
  EXPECT_GE(steady_clock::now() - start, milliseconds(45));
}

}  // namespace
}  // namespace pxsteamdl::detail
