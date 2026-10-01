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
// On failure the reason is available through LastError() until the next system call.
class NativeFile {
public:
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    ~NativeFile() { Close(); }

#ifdef _WIN32
    // Existing file for reading; FILE_FLAG_OPEN_REPARSE_POINT keeps a swapped-in symlink from being followed.
    bool OpenRead(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    // Existing file for writing, same symlink protection.
    bool OpenWrite(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    // New file; fails (AlreadyExists()) if the path exists.
    bool Create(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    bool Resize(std::uint64_t size) {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        return SetFileInformationByHandle(handle_, FileEndOfFileInfo, &info, sizeof info);
    }
    bool WriteAt(std::uint64_t offset, ByteSpan data) const {
        while (!data.empty()) {
            OVERLAPPED position = At(offset);
            DWORD written = 0;
            auto length = static_cast<DWORD>(std::min<std::size_t>(data.size(), kMaxIoSize));
            if (!WriteFile(handle_, data.data(), length, &written, &position) || written == 0) return false;
            data = data.subspan(written);
            offset += written;
        }
        return true;
    }
    // Returns the number of bytes read, 0 at end of file, or -1.
    std::int64_t ReadAt(std::uint64_t offset, std::span<std::uint8_t> buffer) const {
        OVERLAPPED position = At(offset);
        DWORD read = 0;
        auto length = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), kMaxIoSize));
        if (ReadFile(handle_, buffer.data(), length, &read, &position)) return read;
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    static std::string LastError() { return std::system_category().message(static_cast<int>(GetLastError())); }
    static bool AlreadyExists() { return GetLastError() == ERROR_FILE_EXISTS; }

private:
    static constexpr std::size_t kMaxIoSize = 1u << 30;

    static OVERLAPPED At(std::uint64_t offset) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        return position;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    bool OpenRead(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        return fd_ >= 0;
    }
    bool OpenWrite(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
        return fd_ >= 0;
    }
    bool Create(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        return fd_ >= 0;
    }
    bool Resize(std::uint64_t size) { return ftruncate(fd_, static_cast<off_t>(size)) == 0; }
    bool WriteAt(std::uint64_t offset, ByteSpan data) const {
        while (!data.empty()) {
            auto written = pwrite(fd_, data.data(), data.size(), static_cast<off_t>(offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            data = data.subspan(static_cast<std::size_t>(written));
            offset += static_cast<std::uint64_t>(written);
        }
        return true;
    }
    // Returns the number of bytes read, 0 at end of file, or -1.
    std::int64_t ReadAt(std::uint64_t offset, std::span<std::uint8_t> buffer) const {
        for (;;) {
            auto read = pread(fd_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
            if (read >= 0 || errno != EINTR) return read;
        }
    }
    bool IsOpen() const { return fd_ >= 0; }
    void Close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    static std::string LastError() { return std::generic_category().message(errno); }
    static bool AlreadyExists() { return errno == EEXIST; }

private:
    int fd_ = -1;
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
        std::int64_t read = file.ReadAt(offset, std::span(buffer.data(), length));
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
    if (!contents.OpenRead(path)) {
        std::string reason = NativeFile::LastError();
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
    : file_(source), final_(std::move(destination)), out_(std::make_unique<NativeFile>()), unwritten_(writes) {
    thread_local std::mt19937_64 random(std::random_device{}());
    NativeFile created;
    for (;;) {
        temp_ = final_;
        temp_ += ".pxsteamdl." + std::to_string(random());
        if (created.Create(temp_)) break;
        if (!NativeFile::AlreadyExists()) {
            std::string reason = NativeFile::LastError();
            temp_.clear();
            Fail("cannot create temporary file for " + ToUtf8(final_) + ": " + reason);
        }
    }
    if (!created.Resize(source.size)) {
        std::string reason = NativeFile::LastError();
        Discard();
        Fail("cannot size temporary file for " + ToUtf8(final_) + ": " + reason);
    }
}

PendingFile::~PendingFile() { Discard(); }

void PendingFile::Write(std::uint64_t offset, ByteSpan data) {
    struct WriteDone {
        PendingFile& file;
        ~WriteDone() { file.FinishWrite(); }
    } done{*this};
    {
        std::lock_guard lock(mutex_);
        if (!out_->IsOpen() && !out_->OpenWrite(temp_)) {
            std::string reason = NativeFile::LastError();
            Fail("cannot open temporary file for " + ToUtf8(final_) + ": " + reason);
        }
    }
    // The handle stays open until this write is counted done.
    if (!out_->WriteAt(offset, data)) {
        std::string reason = NativeFile::LastError();
        Fail("write failed: " + ToUtf8(final_) + ": " + reason);
    }
}

void PendingFile::Commit(fs::perms perms) {
    if (!ContentMatches(temp_, file_)) Fail("file SHA-1 mismatch: " + file_.name);
    fs::permissions(temp_, perms);
    if (fs::is_directory(fs::symlink_status(final_))) fs::remove_all(final_);
    fs::rename(temp_, final_);
    temp_.clear();
}

void PendingFile::FinishWrite() {
    std::lock_guard lock(mutex_);
    if (--unwritten_ == 0) out_->Close();
}

void PendingFile::Discard() {
    out_->Close();
    std::error_code ignored;
    if (!temp_.empty()) fs::remove(temp_, ignored);
    temp_.clear();
}

}  // namespace pxsteamdl::detail
