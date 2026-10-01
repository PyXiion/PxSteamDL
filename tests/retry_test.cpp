// SPDX-License-Identifier: LGPL-3.0-or-later
#include "retry.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "fake_sleep.hpp"
#include "http.hpp"
#include "test_util.hpp"

namespace pxsteamdl::detail {
namespace {

using std::chrono::milliseconds;
using testing::ExpectDelayBetween;
using testing::ForgetSleeps;
using testing::RecordedSleeps;

class RetryTest : public ::testing::Test {
 protected:
  void SetUp() override { ForgetSleeps(); }
};

TEST_F(RetryTest, ReturnsTheResultWithoutPausing) {
  EXPECT_EQ(Retry({}, [] { return 7; }), 7);
  EXPECT_TRUE(RecordedSleeps().empty());
}

TEST_F(RetryTest, RetriesTransientErrorsWithGrowingPauses) {
  int calls = 0;
  int result = Retry({}, [&] {
    if (++calls < 3) FailTransient("busy");
    return calls;
  });
  EXPECT_EQ(result, 3);
  auto sleeps = RecordedSleeps();
  ASSERT_EQ(sleeps.size(), 2u);
  ExpectDelayBetween(sleeps[0], milliseconds(500));
  ExpectDelayBetween(sleeps[1], milliseconds(1000));
}

TEST_F(RetryTest, GivesUpAfterTheLastAttempt) {
  int calls = 0;
  EXPECT_THROW(Retry({},
                     [&]() -> int {
                       ++calls;
                       FailTransient("still busy " + std::to_string(calls));
                     }),
               TransientError);
  EXPECT_EQ(calls, kMaxAttempts);
  auto sleeps = RecordedSleeps();
  ASSERT_EQ(sleeps.size(), static_cast<std::size_t>(kMaxAttempts - 1));
  ExpectDelayBetween(sleeps[2], milliseconds(2000));
}

TEST_F(RetryTest, PassesOnTheLastErrorUnchanged) {
  try {
    Retry({}, []() -> int { FailTransient("HTTP 503"); });
    FAIL() << "expected an exception";
  } catch (const TransientError& e) {
    EXPECT_STREQ(e.what(), "HTTP 503");
  }
}

TEST_F(RetryTest, DoesNotRetryOtherErrors) {
  int calls = 0;
  EXPECT_THROW(Retry({},
                     [&]() -> int {
                       ++calls;
                       Fail("not found");
                     }),
               std::runtime_error);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(RecordedSleeps().empty());
}

TEST_F(RetryTest, CancelsBeforeTheFirstAttempt) {
  std::stop_source stop;
  stop.request_stop();
  int calls = 0;
  try {
    Retry(stop.get_token(), [&] { return ++calls; });
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), kCancelled);
  }
  EXPECT_EQ(calls, 0);
}

TEST_F(RetryTest, CancelsInsteadOfWaiting) {
  std::stop_source stop;
  int calls = 0;
  try {
    Retry(stop.get_token(), [&]() -> int {
      ++calls;
      stop.request_stop();
      FailTransient("busy");
    });
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), kCancelled);
  }
  EXPECT_EQ(calls, 1);
}

TEST(BackoffDelayTest, DoublesUpToTheCap) {
  for (int round = 0; round < 50; ++round) {
    ExpectDelayBetween(BackoffDelay(1), milliseconds(500));
    ExpectDelayBetween(BackoffDelay(2), milliseconds(1000));
    ExpectDelayBetween(BackoffDelay(3), milliseconds(2000));
    ExpectDelayBetween(BackoffDelay(4), milliseconds(4000));
    ExpectDelayBetween(BackoffDelay(5), milliseconds(8000));
    ExpectDelayBetween(BackoffDelay(6), milliseconds(8000));
    ExpectDelayBetween(BackoffDelay(1000), milliseconds(8000));
  }
}

TEST(BackoffDelayTest, IsNotTheSameForEveryone) {
  bool differs = false;
  for (int i = 0; i < 50 && !differs; ++i) differs = BackoffDelay(5) != BackoffDelay(5);
  EXPECT_TRUE(differs);
}

TEST(HttpStatusTest, ClassifiesStatuses) {
  EXPECT_NO_THROW(CheckHttpStatus("request", 200));
  for (long status : {429L, 500L, 502L, 503L, 504L}) {
    EXPECT_THROW(CheckHttpStatus("request", status), TransientError) << status;
  }
  for (long status : {301L, 400L, 403L, 404L}) {
    try {
      CheckHttpStatus("request", status);
      FAIL() << "expected an exception";
    } catch (const TransientError&) {
      FAIL() << status << " must not be transient";
    } catch (const std::runtime_error&) {
    }
  }
}

TEST(HttpStatusTest, NamesTheRequestAndStatus) {
  try {
    CheckHttpStatus("CDN server list", 503);
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), "CDN server list: HTTP 503");
  }
}

}  // namespace
}  // namespace pxsteamdl::detail
