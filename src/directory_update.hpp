// SPDX-License-Identifier: LGPL-3.0-or-later
// Staging and rollback of a complete item's directory.
#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>

#include "directory_lock.hpp"

namespace pxsteamdl::detail {

class DirectoryUpdate {
 public:
  DirectoryUpdate(const std::filesystem::path& destination, const std::stop_token& stop);
  ~DirectoryUpdate();
  DirectoryUpdate(const DirectoryUpdate&) = delete;
  DirectoryUpdate& operator=(const DirectoryUpdate&) = delete;

  const std::filesystem::path& staging() const { return m_staging; }

  // Checks all parents without following symlinks. A file/directory conflict in the previous copy is not an error.
  bool hasRegularFile(std::string_view name) const;
  // Reuses a verified file by hard link, falling back to copying. Permission changes never modify the old inode.
  void reuseFile(std::string_view name, bool executable);
  // Legacy items replace one file and keep their other entries, as before.
  void copyPrevious();

  // Cancellation is checked before installation. Once started, the two renames finish or restore the previous
  // directory. There is a brief window in which destination is absent; uncoordinated readers are not locked out.
  void commit(const std::stop_token& stop);

 private:
  DirectoryLock m_lock;
  std::filesystem::path m_destination;
  std::filesystem::path m_workspace;
  std::filesystem::path m_staging;
};

}  // namespace pxsteamdl::detail
