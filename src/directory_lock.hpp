// SPDX-License-Identifier: LGPL-3.0-or-later
// A process-wide and cross-process lock for one item's destination.
#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>

namespace pxsteamdl::detail {

// Locks destination's sibling .<name>.lock, waiting interruptibly. The kernel lock, not the recorded PID, determines
// ownership and is released even if the process crashes. Linux/macOS unlink the file before releasing a validated
// lock; waiters reopen obsolete inodes. Windows keeps the file. Do not mix with older persistent-lock clients.
class DirectoryLock {
 public:
  DirectoryLock(const std::filesystem::path& destination, const std::stop_token& stop);
  ~DirectoryLock();
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace pxsteamdl::detail
