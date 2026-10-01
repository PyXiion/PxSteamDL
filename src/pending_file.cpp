// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pending_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <mbedtls/sha1.h>

#include "crypto.hpp"
#include "paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pxsteamdl::detail {

namespace fs = std::filesystem;

// Positional I/O on a native file handle: worker threads write distinct chunk ranges of one file concurrently.
// On failure the reason is available through lastError() until the next system call.
class NativeFile {
 public:
  NativeFile() = default;
  NativeFile(const NativeFile&) = delete;
  NativeFile& operator=(const NativeFile&) = delete;
  ~NativeFile() { close(); }

#ifdef _WIN32
  // Existing file for reading; FILE_FLAG_OPEN_REPARSE_POINT keeps a swapped-in symlink from being followed.
  bool openRead(const fs::path& path) {
    m_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return m_handle != INVALID_HANDLE_VALUE;
  }
  // Existing file for writing, same symlink protection.
  bool openWrite(const fs::path& path) {
    m_handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return m_handle != INVALID_HANDLE_VALUE;
  }
  // New file; fails (alreadyExists()) if the path exists.
  bool create(const fs::path& path) {
    m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    return m_handle != INVALID_HANDLE_VALUE;
  }
  bool resize(std::uint64_t size) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(m_handle, FileEndOfFileInfo, &info, sizeof info);
  }
  bool writeAt(std::uint64_t offset, ByteSpan data) const {
    while (!data.empty()) {
      OVERLAPPED position = at(offset);
      DWORD written = 0;
      auto length = static_cast<DWORD>(std::min<std::size_t>(data.size(), kMaxIoSize));
      if (!WriteFile(m_handle, data.data(), length, &written, &position) || written == 0) return false;
      data = data.subspan(written);
      offset += written;
    }
    return true;
  }
  // Returns the number of bytes read, 0 at end of file, or -1.
  std::int64_t readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) const {
    OVERLAPPED position = at(offset);
    DWORD read = 0;
    auto length = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), kMaxIoSize));
    if (ReadFile(m_handle, buffer.data(), length, &read, &position)) return read;
    return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
  }
  bool isOpen() const { return m_handle != INVALID_HANDLE_VALUE; }
  void close() {
    if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;
  }
  static std::string lastError() { return std::system_category().message(static_cast<int>(GetLastError())); }
  static bool alreadyExists() { return GetLastError() == ERROR_FILE_EXISTS; }

 private:
  static constexpr std::size_t kMaxIoSize = 1u << 30;

  static OVERLAPPED at(std::uint64_t offset) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return position;
  }

  HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
  bool openRead(const fs::path& path) {
    m_fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    return m_fd >= 0;
  }
  bool openWrite(const fs::path& path) {
    m_fd = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    return m_fd >= 0;
  }
  bool create(const fs::path& path) {
    m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    return m_fd >= 0;
  }
  bool resize(std::uint64_t size) { return ftruncate(m_fd, static_cast<off_t>(size)) == 0; }
  bool writeAt(std::uint64_t offset, ByteSpan data) const {
    while (!data.empty()) {
      auto written = pwrite(m_fd, data.data(), data.size(), static_cast<off_t>(offset));
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) return false;
      data = data.subspan(static_cast<std::size_t>(written));
      offset += static_cast<std::uint64_t>(written);
    }
    return true;
  }
  // Returns the number of bytes read, 0 at end of file, or -1.
  std::int64_t readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) const {
    for (;;) {
      auto read = pread(m_fd, buffer.data(), buffer.size(), static_cast<off_t>(offset));
      if (read >= 0 || errno != EINTR) return read;
    }
  }
  bool isOpen() const { return m_fd >= 0; }
  void close() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
  }
  static std::string lastError() { return std::generic_category().message(errno); }
  static bool alreadyExists() { return errno == EEXIST; }

 private:
  int m_fd = -1;
#endif
};

namespace {

Sha1Hash Sha1File(const NativeFile& file, std::uint64_t size) {
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  std::array<std::uint8_t, 65536> buffer{};
  Sha1Hash hash{};
  bool ok = mbedtls_sha1_starts(&context) == 0;
  for (std::uint64_t offset = 0; ok && offset < size;) {
    auto length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
    std::int64_t read = file.readAt(offset, std::span(buffer.data(), length));
    ok = read > 0 && mbedtls_sha1_update(&context, buffer.data(), static_cast<std::size_t>(read)) == 0;
    offset += static_cast<std::uint64_t>(read);
  }
  ok = ok && mbedtls_sha1_finish(&context, hash.data()) == 0;
  mbedtls_sha1_free(&context);
  if (!ok) Fail("SHA-1: failed to read file");
  return hash;
}

// Steam manifests carry an all-zero SHA-1 for empty files rather than SHA-1(""); the size already proves their
// content. (DepotDownloader never checks whole-file hashes, only per-chunk Adler-32.)
bool ContentMatches(const fs::path& path, const ManifestFile& file) {
  NativeFile contents;
  if (!contents.openRead(path)) {
    std::string reason = NativeFile::lastError();
    Fail("cannot open file for SHA-1 verification: " + ToUtf8(path) + ": " + reason);
  }
  return file.size == 0 || Sha1File(contents, file.size) == file.sha;
}

}  // namespace

bool IsUpToDate(const fs::path& path, const ManifestFile& file) {
  return fs::is_regular_file(fs::symlink_status(path)) && fs::file_size(path) == file.size &&
         ContentMatches(path, file);
}

PendingFile::PendingFile(const ManifestFile& source, fs::path destination, std::size_t writes)
    : m_file(source), m_final(std::move(destination)), m_out(std::make_unique<NativeFile>()), m_unwritten(writes) {
  thread_local std::mt19937_64 random(std::random_device{}());
  NativeFile created;
  for (;;) {
    m_temp = m_final;
    m_temp += ".pxsteamdl." + std::to_string(random());
    if (created.create(m_temp)) break;
    if (!NativeFile::alreadyExists()) {
      std::string reason = NativeFile::lastError();
      m_temp.clear();
      Fail("cannot create temporary file for " + ToUtf8(m_final) + ": " + reason);
    }
  }
  if (!created.resize(source.size)) {
    std::string reason = NativeFile::lastError();
    discard();
    Fail("cannot size temporary file for " + ToUtf8(m_final) + ": " + reason);
  }
}

PendingFile::~PendingFile() { discard(); }

void PendingFile::write(std::uint64_t offset, ByteSpan data) {
  struct WriteDone {
    PendingFile& file;
    ~WriteDone() { file.finishWrite(); }
  } done{*this};
  {
    std::lock_guard lock(m_mutex);
    if (!m_out->isOpen() && !m_out->openWrite(m_temp)) {
      std::string reason = NativeFile::lastError();
      Fail("cannot open temporary file for " + ToUtf8(m_final) + ": " + reason);
    }
  }
  // The handle stays open until this write is counted done.
  if (!m_out->writeAt(offset, data)) {
    std::string reason = NativeFile::lastError();
    Fail("write failed: " + ToUtf8(m_final) + ": " + reason);
  }
}

void PendingFile::commit(fs::perms perms) {
  if (!ContentMatches(m_temp, m_file)) Fail("file SHA-1 mismatch: " + m_file.name);
  fs::permissions(m_temp, perms);
  if (fs::is_directory(fs::symlink_status(m_final))) fs::remove_all(m_final);
  fs::rename(m_temp, m_final);
  m_temp.clear();
}

void PendingFile::finishWrite() {
  std::lock_guard lock(m_mutex);
  if (--m_unwritten == 0) m_out->close();
}

void PendingFile::discard() {
  m_out->close();
  std::error_code ignored;
  if (!m_temp.empty()) fs::remove(m_temp, ignored);
  m_temp.clear();
}

}  // namespace pxsteamdl::detail
