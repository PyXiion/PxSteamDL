// SPDX-License-Identifier: LGPL-3.0-or-later
#include "directory_lock.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <system_error>

#include "common.hpp"
#include "paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pxsteamdl::detail {

namespace {

constexpr auto kLockPollInterval = std::chrono::milliseconds(50);

// Unlike network retry pauses, this synchronizes real filesystem operations even in the offline test build.
void WaitForLock(const std::stop_token& stop) {
  std::mutex mutex;
  std::condition_variable wake;
  std::stop_callback on_stop(stop, [&] {
    std::lock_guard lock(mutex);
    wake.notify_all();
  });
  std::unique_lock lock(mutex);
  if (wake.wait_for(lock, kLockPollInterval, [&] { return stop.stop_requested(); })) FailCancelled();
}

}  // namespace

class DirectoryLock::Impl {
 public:
  ~Impl() {
#ifdef _WIN32
    if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#else
    if (m_fd >= 0) ::close(m_fd);
#endif
  }

  void acquire(const std::filesystem::path& destination, const std::stop_token& stop) {
    if (stop.stop_requested()) FailCancelled();
    std::filesystem::path parent = destination.parent_path();
    if (parent.empty()) parent = ".";
    std::filesystem::create_directories(parent);
    m_path = parent / Utf8Path("." + ToUtf8(destination.filename()) + ".lock");
    open();
    for (;;) {
      if (stop.stop_requested()) FailCancelled();
      if (tryLock()) break;
      WaitForLock(stop);
    }
    if (stop.stop_requested()) FailCancelled();
    recordPid();
  }

 private:
  [[noreturn]] void fail(const std::string& action, int error) const {
#ifdef _WIN32
    std::string reason = std::system_category().message(error);
#else
    std::string reason = std::generic_category().message(error);
#endif
    Fail(ErrorKind::kFilesystem, action + " " + ToUtf8(m_path) + ": " + reason);
  }

#ifdef _WIN32
  void open() {
    m_handle = CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) fail("cannot open lock file", static_cast<int>(GetLastError()));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(m_handle, &info))
      fail("cannot inspect lock file", static_cast<int>(GetLastError()));
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) {
      Fail(ErrorKind::kFilesystem, "lock file must be a regular file: " + ToUtf8(m_path));
    }
  }

  bool tryLock() {
    OVERLAPPED position{};
    // Lock a byte beyond the PID text so other processes can still read it on Windows (mandatory byte locks).
    constexpr DWORD kPidLockOffset = 65536;
    position.Offset = kPidLockOffset;
    if (LockFileEx(m_handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_LOCK_VIOLATION) return false;
    fail("cannot lock file", static_cast<int>(error));
  }

  void recordPid() {
    std::string pid = std::to_string(GetCurrentProcessId()) + "\n";
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(m_handle, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(m_handle)) {
      fail("cannot truncate lock file", static_cast<int>(GetLastError()));
    }
    DWORD written = 0;
    if (!WriteFile(m_handle, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr) || written != pid.size()) {
      fail("cannot write lock PID", static_cast<int>(GetLastError()));
    }
  }

  HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
  void open() {
    m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (m_fd < 0) fail("cannot open lock file", errno);
    struct stat info {};
    if (fstat(m_fd, &info) != 0) fail("cannot inspect lock file", errno);
    if (!S_ISREG(info.st_mode)) Fail(ErrorKind::kFilesystem, "lock file must be a regular file: " + ToUtf8(m_path));
  }

  bool tryLock() {
    if (flock(m_fd, LOCK_EX | LOCK_NB) == 0) return true;
    int error = errno;
    if (error == EWOULDBLOCK || error == EAGAIN || error == EINTR) return false;
    fail("cannot lock file", error);
  }

  void recordPid() {
    std::string pid = std::to_string(getpid()) + "\n";
    if (ftruncate(m_fd, 0) != 0) fail("cannot truncate lock file", errno);
    std::size_t offset = 0;
    while (offset < pid.size()) {
      auto written = pwrite(m_fd, pid.data() + offset, pid.size() - offset, static_cast<off_t>(offset));
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) fail("cannot write lock PID", written < 0 ? errno : EIO);
      offset += static_cast<std::size_t>(written);
    }
  }

  int m_fd = -1;
#endif

  std::filesystem::path m_path;
};

DirectoryLock::DirectoryLock(const std::filesystem::path& destination, const std::stop_token& stop)
    : m_impl(std::make_unique<Impl>()) {
  m_impl->acquire(destination, stop);
}

DirectoryLock::~DirectoryLock() = default;

}  // namespace pxsteamdl::detail
