// SPDX-License-Identifier: LGPL-3.0-or-later
// Files assembled from chunks next to their destination and moved into place once verified.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

#include "common.hpp"
#include "manifest.hpp"

namespace pxsteamdl::detail {

class NativeFile;

inline constexpr auto kRegularPerms = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                      std::filesystem::perms::group_read | std::filesystem::perms::others_read;
inline constexpr auto kExecPerms =
    std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;

// Whether path is a regular file (not a symlink) whose size and SHA-1 match the manifest.
bool IsUpToDate(const std::filesystem::path& path, const ManifestFile& file);

// A file being assembled in a temporary sibling of its final path. The temporary file is open only while chunks of
// it are being written, so an item with thousands of changed files does not exhaust the descriptor limit.
// Destroying an uncommitted PendingFile removes the temporary file.
class PendingFile {
 public:
  // Creates the temporary file at its final size; writes is the number of write() calls that will follow.
  PendingFile(const ManifestFile& source, std::filesystem::path destination, std::size_t writes);
  ~PendingFile();
  PendingFile(const PendingFile&) = delete;
  PendingFile& operator=(const PendingFile&) = delete;

  const ManifestFile& file() const { return m_file; }

  // One of the writes announced to the constructor; safe to call concurrently for disjoint ranges.
  // The file is opened by the first write and closed after the last one, whether or not it succeeded.
  void write(std::uint64_t offset, ByteSpan data);

  // Verifies the whole-file hash, then moves the temporary file over the final path. Call after all writes.
  void commit(std::filesystem::perms perms);

 private:
  // Counts a write as done and closes the file after the last one.
  void finishWrite();
  void discard();

  const ManifestFile& m_file;
  std::filesystem::path m_final;
  std::filesystem::path m_temp;
  std::mutex m_mutex;  // guards opening and closing m_out, and m_unwritten
  std::unique_ptr<NativeFile> m_out;
  std::size_t m_unwritten;
};

}  // namespace pxsteamdl::detail
