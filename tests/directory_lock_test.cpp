// SPDX-License-Identifier: LGPL-3.0-or-later
#include "directory_lock.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <stop_token>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common.hpp"
#include "paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pxsteamdl::detail {
namespace {

namespace fs = std::filesystem;
constexpr auto kWaitTimeout = std::chrono::seconds(5);
constexpr auto kBlockedCheck = std::chrono::milliseconds(100);

std::string Pid() {
#ifdef _WIN32
  return std::to_string(GetCurrentProcessId());
#else
  return std::to_string(getpid());
#endif
}

std::string ReadPid(const fs::path& path) {
  std::string text;
  std::ifstream(path) >> text;
  return text;
}

class LockHolder {
 public:
  LockHolder(const fs::path& destination, const fs::path& ready) {
#ifdef _WIN32
    std::wstring command = L"\"" + Utf8Path(PXSTEAMDL_LOCK_HOLDER_PATH).wstring() + L"\" \"" + destination.wstring() +
                           L"\" \"" + ready.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
      Fail(ErrorKind::kOther, "cannot start lock holder");
    }
    m_process = process.hProcess;
    CloseHandle(process.hThread);
#else
    m_pid = fork();
    if (m_pid < 0) Fail(ErrorKind::kOther, "cannot fork lock holder");
    if (m_pid == 0) {
      execl(PXSTEAMDL_LOCK_HOLDER_PATH, PXSTEAMDL_LOCK_HOLDER_PATH, destination.c_str(), ready.c_str(),
            static_cast<char*>(nullptr));
      _exit(127);
    }
#endif
  }
  ~LockHolder() { stop(); }
  LockHolder(const LockHolder&) = delete;
  LockHolder& operator=(const LockHolder&) = delete;

  void stop() {
#ifdef _WIN32
    if (m_process) {
      TerminateProcess(m_process, 1);
      constexpr DWORD kChildExitTimeoutMs = 5000;
      WaitForSingleObject(m_process, kChildExitTimeoutMs);
      CloseHandle(m_process);
      m_process = nullptr;
    }
#else
    if (m_pid > 0) {
      kill(m_pid, SIGKILL);
      while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {
      }
      m_pid = -1;
    }
#endif
  }

 private:
#ifdef _WIN32
  HANDLE m_process = nullptr;
#else
  pid_t m_pid = -1;
#endif
};

class DirectoryLockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_root = fs::temp_directory_path() / ("pxsteamdl-lock-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(m_root);
    m_destination = m_root / "111";
    m_lockPath = m_root / ".111.lock";
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(m_root, ignored);
  }
  fs::path m_root;
  fs::path m_destination;
  fs::path m_lockPath;
};

TEST_F(DirectoryLockTest, WritesThePidAndKeepsTheFileOnRelease) {
  {
    DirectoryLock lock(m_destination, {});
    EXPECT_EQ(ReadPid(m_lockPath), Pid());
    EXPECT_FALSE(fs::exists(m_destination));
  }
  EXPECT_TRUE(fs::is_regular_file(m_lockPath));
  EXPECT_EQ(ReadPid(m_lockPath), Pid());
}

TEST_F(DirectoryLockTest, ReplacesStalePidAndDoesNotTrustAnUnlockedLivePid) {
  std::ofstream(m_lockPath) << "999999999\n";
  { DirectoryLock lock(m_destination, {}); }
  EXPECT_EQ(ReadPid(m_lockPath), Pid());
  // Even the current process's live PID must not turn an unlocked file into a lock.
  DirectoryLock lock(m_destination, {});
  EXPECT_EQ(ReadPid(m_lockPath), Pid());
}

TEST_F(DirectoryLockTest, WaitsForAnotherOwnerInTheSameProcess) {
  auto first = std::make_unique<DirectoryLock>(m_destination, std::stop_token{});
  std::promise<void> acquired;
  auto result = acquired.get_future();
  std::jthread waiter([&](const std::stop_token& stop) {
    try {
      DirectoryLock lock(m_destination, stop);
      acquired.set_value();
    } catch (...) {
      acquired.set_exception(std::current_exception());
    }
  });
  EXPECT_EQ(result.wait_for(kBlockedCheck), std::future_status::timeout);
  first.reset();
  ASSERT_EQ(result.wait_for(kWaitTimeout), std::future_status::ready);
  EXPECT_NO_THROW(result.get());
}

TEST_F(DirectoryLockTest, WaitingCanBeCancelledWithoutChangingTheOwnerPid) {
  DirectoryLock first(m_destination, {});
  std::promise<ErrorKind> completed;
  auto result = completed.get_future();
  std::jthread waiter([&](const std::stop_token& stop) {
    try {
      DirectoryLock lock(m_destination, stop);
      completed.set_value(ErrorKind::kNone);
    } catch (const Error& error) {
      completed.set_value(error.kind());
    } catch (...) {
      completed.set_exception(std::current_exception());
    }
  });
  EXPECT_EQ(result.wait_for(kBlockedCheck), std::future_status::timeout);
  waiter.request_stop();
  ASSERT_EQ(result.wait_for(kWaitTimeout), std::future_status::ready);
  EXPECT_EQ(result.get(), ErrorKind::kCancelled);
  EXPECT_EQ(ReadPid(m_lockPath), Pid());
}

TEST_F(DirectoryLockTest, ForcedProcessExitReleasesTheLockAndReplacesItsPid) {
  fs::path ready = m_root / "ready";
  LockHolder holder(m_destination, ready);
  auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
  while (!fs::exists(ready) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(fs::exists(ready));
  EXPECT_NE(ReadPid(m_lockPath), Pid());
  std::promise<void> acquired;
  auto result = acquired.get_future();
  std::jthread waiter([&](const std::stop_token& stop) {
    try {
      DirectoryLock lock(m_destination, stop);
      acquired.set_value();
    } catch (...) {
      acquired.set_exception(std::current_exception());
    }
  });
  EXPECT_EQ(result.wait_for(kBlockedCheck), std::future_status::timeout);
  holder.stop();
  ASSERT_EQ(result.wait_for(kWaitTimeout), std::future_status::ready);
  EXPECT_NO_THROW(result.get());
  EXPECT_EQ(ReadPid(m_lockPath), Pid());
}

#ifndef _WIN32
TEST_F(DirectoryLockTest, RejectsASymlinkWithoutTruncatingItsTarget) {
  fs::path target = m_root / "target";
  std::ofstream(target) << "keep me";
  fs::create_symlink(target, m_lockPath);
  EXPECT_THROW(DirectoryLock lock(m_destination, {}), Error);
  EXPECT_EQ(ReadPid(target), "keep");
}
#endif

}  // namespace
}  // namespace pxsteamdl::detail
