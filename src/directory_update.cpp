// SPDX-License-Identifier: LGPL-3.0-or-later
#include "directory_update.hpp"

#include <exception>
#include <random>
#include <string>
#include <system_error>

#include "common.hpp"
#include "paths.hpp"
#include "pending_file.hpp"

namespace pxsteamdl::detail {

namespace {

namespace fs = std::filesystem;

fs::path MakeWorkspace(const fs::path& destination) {
  thread_local std::mt19937_64 random(std::random_device{}());
  fs::path parent = destination.parent_path();
  if (parent.empty()) parent = ".";
  for (;;) {
    fs::path path = parent / Utf8Path("." + ToUtf8(destination.filename()) + ".pxsteamdl." + std::to_string(random()));
    if (fs::create_directory(path)) return path;
  }
}

}  // namespace

DirectoryUpdate::DirectoryUpdate(const fs::path& destination, const std::stop_token& stop)
    : m_lock(destination, stop), m_destination(destination) {
  auto status = fs::symlink_status(destination);
  if (fs::is_symlink(status)) Fail(ErrorKind::kFilesystem, "destination must not be a symlink");
  if (fs::exists(status) && !fs::is_directory(status)) Fail(ErrorKind::kFilesystem, "destination must be a directory");
  m_workspace = MakeWorkspace(destination);
  try {
    m_staging = m_workspace / "new";
    fs::create_directory(m_staging);
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(m_workspace, ignored);
    throw;
  }
}

DirectoryUpdate::~DirectoryUpdate() {
  std::error_code ignored;
  if (!m_workspace.empty()) fs::remove_all(m_workspace, ignored);
}

bool DirectoryUpdate::hasRegularFile(std::string_view name) const {
  fs::path relative = Utf8Path(name);
  fs::path path = m_destination;
  if (!fs::is_directory(fs::symlink_status(path))) return false;
  for (auto part = relative.begin(); part != relative.end();) {
    path /= *part++;
    auto status = fs::symlink_status(path);
    if (part == relative.end()) return fs::is_regular_file(status);
    if (!fs::is_directory(status)) return false;
  }
  return false;
}

void DirectoryUpdate::reuseFile(std::string_view name, bool executable) {
  fs::path source = m_destination / Utf8Path(name);
  fs::path target = m_staging / Utf8Path(name);
  bool add_execute = executable && (fs::status(source).permissions() & kExecPerms) != kExecPerms;
  std::error_code error;
  if (!add_execute) {
    fs::create_hard_link(source, target, error);
    if (!error) return;
  }
  fs::copy_file(source, target);
  if (add_execute) fs::permissions(target, kExecPerms, fs::perm_options::add);
}

void DirectoryUpdate::copyPrevious() {
  if (!fs::exists(m_destination)) return;
  for (const auto& entry : fs::directory_iterator(m_destination)) {
    fs::copy(entry.path(), m_staging / entry.path().filename(),
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
  }
}

void DirectoryUpdate::commit(const std::stop_token& stop) {
  if (stop.stop_requested()) FailCancelled();
  fs::path previous = m_workspace / "old";
  bool had_previous = fs::exists(fs::symlink_status(m_destination));
  if (had_previous) fs::rename(m_destination, previous);
  try {
    fs::rename(m_staging, m_destination);
  } catch (const std::exception& e) {
    if (had_previous) {
      std::error_code error;
      fs::rename(previous, m_destination, error);
      if (error) {
        // Keep the only old copy, even if restoring it fails (e.g. the filesystem went away).
        std::string backup = ToUtf8(previous);
        m_workspace.clear();
        Fail(ErrorKind::kFilesystem, std::string(e.what()) + "; cannot restore previous copy: " + error.message() +
                                         "; previous copy kept at " + backup);
      }
    }
    throw;
  }
}

}  // namespace pxsteamdl::detail
