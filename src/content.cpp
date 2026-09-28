// SPDX-License-Identifier: LGPL-3.0-or-later
#include "internal.hpp"

#include <lzma.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <nlohmann/json.hpp>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pxsteamdl::detail {
namespace {

namespace fs = std::filesystem;
using Hash = std::array<std::uint8_t, 20>;
using Key = std::array<std::uint8_t, 32>;

struct Chunk {
    Hash sha{};
    std::uint32_t crc = 0;
    std::uint64_t offset = 0;
    std::uint32_t original = 0;
    std::uint32_t compressed = 0;
};

// EDepotFileFlag bits used here.
namespace file_flag {
constexpr std::uint32_t executable = 0x20;
constexpr std::uint32_t directory = 0x40;
constexpr std::uint32_t custom_executable = 0x80;
constexpr std::uint32_t symlink = 0x200;
} // namespace file_flag

struct File {
    std::string name;
    std::string link_target;
    std::uint64_t size = 0;
    std::uint32_t flags = 0;
    Hash sha{};
    std::vector<Chunk> chunks;

    bool is_directory() const { return flags & file_flag::directory; }
    bool is_symlink() const { return flags & file_flag::symlink; }
    bool is_regular() const { return !is_directory() && !is_symlink(); }
    bool is_executable() const { return flags & (file_flag::executable | file_flag::custom_executable); }
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint16_t le16(std::span<const std::uint8_t> bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 2) fail("truncated ZIP data");
    return std::uint16_t(bytes[pos]) | (std::uint16_t(bytes[pos + 1]) << 8);
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 4) fail("truncated binary data");
    return std::uint32_t(bytes[pos]) | (std::uint32_t(bytes[pos + 1]) << 8) |
           (std::uint32_t(bytes[pos + 2]) << 16) | (std::uint32_t(bytes[pos + 3]) << 24);
}

std::span<const std::uint8_t> slice(std::span<const std::uint8_t> bytes, std::size_t pos, std::size_t size) {
    if (pos > bytes.size() || size > bytes.size() - pos) fail("truncated binary data");
    return bytes.subspan(pos, size);
}

Bytes unzip_single(std::span<const std::uint8_t> zip) {
    // Sizes in local headers can be zero when a data descriptor follows the data.
    if (zip.size() < 22) fail("ZIP: missing end of central directory");
    std::size_t eocd = zip.size();
    std::size_t start = zip.size() > 22 + 65535 ? zip.size() - 22 - 65535 : 0;
    for (std::size_t p = zip.size() - 22;; --p) {
        if (le32(zip, p) == 0x06054b50 && p + 22 + le16(zip, p + 20) == zip.size()) {
            eocd = p;
            break;
        }
        if (p == start) break;
    }
    if (eocd == zip.size() || le16(zip, eocd + 4) || le16(zip, eocd + 6) ||
        le16(zip, eocd + 8) != 1 || le16(zip, eocd + 10) != 1)
        fail("ZIP: expected exactly one entry on one disk");

    auto central_size = le32(zip, eocd + 12);
    auto central = le32(zip, eocd + 16);
    auto cd = slice(zip, central, central_size);
    if (cd.size() < 46 || le32(cd, 0) != 0x02014b50) fail("ZIP: invalid central directory");
    auto flags = le16(cd, 8);
    auto method = le16(cd, 10);
    auto crc = le32(cd, 16);
    auto compressed = le32(cd, 20);
    auto original = le32(cd, 24);
    auto entry_size = std::size_t{46} + le16(cd, 28) + le16(cd, 30) + le16(cd, 32);
    if (entry_size != cd.size() || compressed == UINT32_MAX || original == UINT32_MAX ||
        (flags & 1) || (method != 0 && method != 8))
        fail("ZIP: unsupported entry");

    auto local = le32(cd, 42);
    auto header = slice(zip, local, 30);
    if (le32(header, 0) != 0x04034b50 || le16(header, 8) != method || (le16(header, 6) & 1))
        fail("ZIP: invalid local header");
    auto offset = std::size_t{local} + 30 + le16(header, 26) + le16(header, 28);
    auto packed = slice(zip, offset, compressed);
    Bytes result(original);
    if (method == 0) {
        if (compressed != original) fail("ZIP: stored entry size mismatch");
        std::copy(packed.begin(), packed.end(), result.begin());
    } else {
        if (packed.size() > UINT_MAX || result.size() > UINT_MAX) fail("ZIP: entry too large");
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) fail("ZIP: inflate init failed");
        stream.next_in = const_cast<Bytef*>(packed.data());
        stream.avail_in = static_cast<uInt>(packed.size());
        std::uint8_t empty = 0;
        stream.next_out = result.empty() ? &empty : result.data();
        stream.avail_out = result.empty() ? 1 : static_cast<uInt>(result.size());
        int status = inflate(&stream, Z_FINISH);
        auto written = stream.total_out;
        auto consumed = stream.total_in;
        inflateEnd(&stream);
        if (status != Z_STREAM_END || written != original || consumed != compressed)
            fail("ZIP: deflate data or length is invalid");
    }
    if (crc32_z(0, result.data(), result.size()) != crc) fail("ZIP: CRC mismatch");
    return result;
}

Bytes decrypt(std::span<const std::uint8_t> encrypted, const Key& key) {
    if (encrypted.size() < 32 || (encrypted.size() - 16) % 16) fail("AES: invalid ciphertext length");
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    std::array<std::uint8_t, 16> iv{};
    Bytes plain(encrypted.size() - 16);
    int ecb = mbedtls_aes_setkey_dec(&aes, key.data(), 256);
    if (ecb == 0) ecb = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, encrypted.data(), iv.data());
    int cbc = ecb == 0 ? mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, plain.size(), iv.data(),
                                               encrypted.data() + 16, plain.data())
                       : 0;
    mbedtls_aes_free(&aes);
    if (ecb != 0) fail("AES: IV decryption failed");
    std::uint8_t padding = plain.back();
    if (cbc != 0 || padding == 0 || padding > 16 ||
        !std::all_of(plain.end() - padding, plain.end(), [padding](std::uint8_t b) { return b == padding; }))
        fail("AES: CBC decryption or PKCS7 padding failed");
    plain.resize(plain.size() - padding);
    return plain;
}

std::string decrypt_name(std::string_view encoded, const Key& key) {
    std::string base64;
    base64.reserve(encoded.size());
    for (char c : encoded) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        base64.push_back(c);
    }
    if (base64.empty() || base64.size() % 4) fail("manifest: invalid base64 filename");
    Bytes encrypted(base64.size() / 4 * 3);
    std::size_t length = 0;
    if (mbedtls_base64_decode(encrypted.data(), encrypted.size(), &length,
                              reinterpret_cast<const unsigned char*>(base64.data()), base64.size()) != 0)
        fail("manifest: invalid base64 filename");
    encrypted.resize(length);
    Bytes plain = decrypt(encrypted, key);
    while (!plain.empty() && plain.back() == 0) plain.pop_back();
    return {reinterpret_cast<const char*>(plain.data()), plain.size()};
}

// Manifest names are UTF-8; converting through std::string would use the ANSI code page on Windows.
fs::path utf8_path(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string utf8(const fs::path& path) {
    auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// Non-empty names Win32 would reinterpret: stream/drive separators, wildcards, device names, and trailing dots or
// spaces (silently stripped, so "a." would alias "a"). Always false elsewhere.
bool windows_unsafe([[maybe_unused]] std::string_view part) {
#ifdef _WIN32
    if (part.find_first_of("<>:\"|?*") != std::string_view::npos || part.back() == '.' || part.back() == ' ' ||
        std::any_of(part.begin(), part.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
        return true;
    std::string stem(part.substr(0, part.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9';
#else
    return false;
#endif
}

// Key under which a manifest path is compared with others and with what is on disk. Windows and macOS file
// systems are case-insensitive by default, so "Textures/a.png" and "textures/a.png" name the same file there;
// ASCII folding covers the paths mods use in practice.
std::string path_key(std::string path) {
#if defined(_WIN32) || defined(__APPLE__)
    std::transform(path.begin(), path.end(), path.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
#endif
    return path;
}

void normalize_path(std::string& path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\0') != std::string::npos || utf8_path(path).has_root_path())
        fail("manifest: unsafe path: " + path);
    std::size_t from = 0;
    for (std::size_t end; from < path.size(); from = end + 1) {
        end = path.find('/', from);
        if (end == std::string::npos) end = path.size();
        std::string_view part(path.data() + from, end - from);
        if (part.empty() || part == "." || part == "..") fail("manifest: unsafe path: " + path);
        if (windows_unsafe(part)) fail("manifest: path not representable on Windows: " + path);
        if (end == path.size()) break;
    }
}

Chunk parse_chunk(std::span<const std::uint8_t> bytes) {
    Chunk chunk;
    bool has_sha = false, has_original = false, has_compressed = false;
    Reader reader(bytes);
    Field field;
    while (reader.next(field)) {
        if (field.number == 1 && field.wire == 2) {
            if (field.bytes.size() != chunk.sha.size()) fail("manifest: invalid chunk SHA-1");
            std::copy(field.bytes.begin(), field.bytes.end(), chunk.sha.begin());
            has_sha = true;
        } else if (field.number == 2 && field.wire == 5) chunk.crc = static_cast<std::uint32_t>(field.integer);
        else if (field.number == 3 && field.wire == 0) chunk.offset = field.integer;
        else if (field.number == 4 && field.wire == 0) {
            if (field.integer > UINT32_MAX) fail("manifest: chunk too large");
            chunk.original = static_cast<std::uint32_t>(field.integer);
            has_original = true;
        } else if (field.number == 5 && field.wire == 0) {
            if (field.integer > UINT32_MAX) fail("manifest: chunk too large");
            chunk.compressed = static_cast<std::uint32_t>(field.integer);
            has_compressed = true;
        }
    }
    if (!has_sha || !has_original || !has_compressed || chunk.compressed < 32)
        fail("manifest: incomplete chunk");
    return chunk;
}

File parse_file(std::span<const std::uint8_t> bytes) {
    File file;
    bool has_hash = false;
    Reader reader(bytes);
    Field field;
    while (reader.next(field)) {
        if (field.number == 1 && field.wire == 2)
            file.name.assign(reinterpret_cast<const char*>(field.bytes.data()), field.bytes.size());
        else if (field.number == 2 && field.wire == 0) file.size = field.integer;
        else if (field.number == 3 && field.wire == 0) file.flags = static_cast<std::uint32_t>(field.integer);
        else if (field.number == 5 && field.wire == 2) {
            if (field.bytes.size() != file.sha.size()) fail("manifest: invalid file SHA-1");
            std::copy(field.bytes.begin(), field.bytes.end(), file.sha.begin());
            has_hash = true;
        } else if (field.number == 6 && field.wire == 2) file.chunks.push_back(parse_chunk(field.bytes));
        else if (field.number == 7 && field.wire == 2)
            file.link_target.assign(reinterpret_cast<const char*>(field.bytes.data()), field.bytes.size());
    }
    if (file.is_regular() && !has_hash) fail("manifest: file missing SHA-1");
    return file;
}

std::vector<File> parse_manifest(std::span<const std::uint8_t> zip, const Key& key,
                                 std::uint32_t depot, std::uint64_t manifest_id) {
    Bytes binary = unzip_single(zip);
    std::span<const std::uint8_t> bytes(binary);
    std::vector<File> files;
    std::size_t offset = 0;
    bool payload = false, metadata = false, signature = false, end = false, encrypted = false;
    std::uint32_t actual_depot = 0;
    std::uint64_t actual_gid = 0;
    while (offset < bytes.size()) {
        auto magic = le32(bytes, offset);
        offset += 4;
        if (magic == 0x32c415ab) {
            end = offset == bytes.size();
            break;
        }
        auto size = le32(bytes, offset);
        offset += 4;
        auto section = slice(bytes, offset, size);
        offset += size;
        Reader reader(section);
        Field field;
        if (magic == 0x71f617d0 && !payload) {
            payload = true;
            while (reader.next(field))
                if (field.number == 1 && field.wire == 2) files.push_back(parse_file(field.bytes));
        } else if (magic == 0x1f4812be && !metadata) {
            metadata = true;
            while (reader.next(field)) {
                if (field.number == 1 && field.wire == 0) actual_depot = static_cast<std::uint32_t>(field.integer);
                else if (field.number == 2 && field.wire == 0) actual_gid = field.integer;
                else if (field.number == 4 && field.wire == 0) encrypted = field.integer != 0;
            }
        } else if (magic == 0x1b81b817 && !signature) {
            signature = true;
            while (reader.next(field)) { }
        } else fail("manifest: invalid or repeated section");
    }
    if (!payload || !metadata || !signature || !end || actual_depot != depot || actual_gid != manifest_id)
        fail("manifest: incomplete sections or wrong depot/manifest ID");

    for (auto& file : files) {
        if (encrypted) {
            file.name = decrypt_name(file.name, key);
            if (!file.link_target.empty()) file.link_target = decrypt_name(file.link_target, key);
        }
        normalize_path(file.name);
        if (file.is_symlink()) {
            normalize_path(file.link_target);
            if (!file.chunks.empty()) fail("manifest: symlink contains chunks");
        } else if (file.is_directory()) {
            if (!file.chunks.empty()) fail("manifest: directory contains chunks");
        } else {
            if (file.size > static_cast<std::uint64_t>(INT64_MAX)) fail("manifest: file too large");
            std::sort(file.chunks.begin(), file.chunks.end(),
                      [](const Chunk& a, const Chunk& b) { return a.offset < b.offset; });
            std::uint64_t next = 0;
            for (const auto& chunk : file.chunks) {
                if (chunk.offset != next || chunk.original > file.size - next)
                    fail("manifest: file chunks do not cover expected size: " + file.name);
                next += chunk.original;
            }
            if (next != file.size) fail("manifest: file chunks do not cover expected size: " + file.name);
        }
    }
    return files;
}

std::string hex_hash(const Hash& hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(40, '0');
    for (std::size_t i = 0; i < hash.size(); ++i) {
        result[2 * i] = digits[hash[i] >> 4];
        result[2 * i + 1] = digits[hash[i] & 15];
    }
    return result;
}

Bytes expand_chunk(std::span<const std::uint8_t> encrypted, const Key& key, const Chunk& chunk) {
    if (encrypted.size() != chunk.compressed) fail("chunk: compressed size mismatch");
    Bytes plain = decrypt(encrypted, key);
    std::span<const std::uint8_t> data(plain);
    Bytes result(chunk.original);
    if (data.size() >= 23 && std::memcmp(data.data(), "VSZa", 4) == 0) {
        if (std::memcmp(data.data() + data.size() - 3, "zsv", 3) != 0 ||
            le32(data, data.size() - 11) != chunk.original) fail("chunk: invalid VZstd footer");
        auto written = ZSTD_decompress(result.data(), result.size(), data.data() + 8, data.size() - 23);
        if (ZSTD_isError(written) || written != result.size()) fail("chunk: Zstd decompression failed");
    } else if (data.size() >= 22 && std::memcmp(data.data(), "VZa", 3) == 0) {
        if (data[data.size() - 2] != 'z' || data.back() != 'v' ||
            le32(data, data.size() - 6) != chunk.original) fail("chunk: invalid VZip footer");
        Bytes alone(13 + data.size() - 22);
        std::copy_n(data.begin() + 7, 5, alone.begin());
        for (int i = 0; i < 8; ++i) alone[5 + i] = static_cast<std::uint8_t>(std::uint64_t(chunk.original) >> (8 * i));
        std::copy(data.begin() + 12, data.end() - 10, alone.begin() + 13);
        lzma_stream stream = LZMA_STREAM_INIT;
        if (lzma_alone_decoder(&stream, UINT64_MAX) != LZMA_OK) fail("chunk: LZMA initialization failed");
        stream.next_in = alone.data();
        stream.avail_in = alone.size();
        std::uint8_t empty = 0;
        stream.next_out = result.empty() ? &empty : result.data();
        stream.avail_out = result.empty() ? 1 : result.size();
        auto status = lzma_code(&stream, LZMA_FINISH);
        auto written = stream.total_out;
        lzma_end(&stream);
        if (status != LZMA_STREAM_END || written != result.size()) fail("chunk: LZMA decompression failed");
    } else if (data.size() >= 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0) {
        result = unzip_single(data);
        if (result.size() != chunk.original) fail("chunk: ZIP length mismatch");
    } else fail("chunk: unknown compression format");
    if (adler32_z(0, result.data(), result.size()) != chunk.crc) fail("chunk: Adler32 mismatch");
    return result;
}

// Positional I/O on a native file handle: worker threads write distinct chunk ranges of one file concurrently.
class NativeFile {
public:
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    ~NativeFile() { close(); }

    // On failure the reason is available through last_error() until the next system call.
#ifdef _WIN32
    // Existing file for reading; FILE_FLAG_OPEN_REPARSE_POINT keeps a swapped-in symlink from being followed.
    bool open_read(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    // Existing file for writing, same symlink protection.
    bool open_write(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    // New file; fails (already_exists()) if the path exists.
    bool create(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    bool resize(std::uint64_t size) {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        return SetFileInformationByHandle(handle_, FileEndOfFileInfo, &info, sizeof info);
    }
    bool write_at(std::uint64_t offset, std::span<const std::uint8_t> data) const {
        while (!data.empty()) {
            OVERLAPPED position = at(offset);
            DWORD written = 0;
            auto length = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
            if (!WriteFile(handle_, data.data(), length, &written, &position) || written == 0) return false;
            data = data.subspan(written);
            offset += written;
        }
        return true;
    }
    // Returns the number of bytes read, 0 at end of file, or -1.
    std::int64_t read_at(std::uint64_t offset, std::span<std::uint8_t> buffer) const {
        OVERLAPPED position = at(offset);
        DWORD read = 0;
        auto length = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 1u << 30));
        if (ReadFile(handle_, buffer.data(), length, &read, &position)) return read;
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    bool is_open() const { return handle_ != INVALID_HANDLE_VALUE; }
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    static std::string last_error() { return std::system_category().message(static_cast<int>(GetLastError())); }
    static bool already_exists() { return GetLastError() == ERROR_FILE_EXISTS; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;

    static OVERLAPPED at(std::uint64_t offset) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        return position;
    }
#else
    bool open_read(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        return fd_ >= 0;
    }
    bool open_write(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
        return fd_ >= 0;
    }
    bool create(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        return fd_ >= 0;
    }
    bool resize(std::uint64_t size) { return ftruncate(fd_, static_cast<off_t>(size)) == 0; }
    bool write_at(std::uint64_t offset, std::span<const std::uint8_t> data) const {
        while (!data.empty()) {
            auto n = pwrite(fd_, data.data(), data.size(), static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            data = data.subspan(static_cast<std::size_t>(n));
            offset += static_cast<std::uint64_t>(n);
        }
        return true;
    }
    std::int64_t read_at(std::uint64_t offset, std::span<std::uint8_t> buffer) const {
        for (;;) {
            auto n = pread(fd_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
            if (n >= 0 || errno != EINTR) return n;
        }
    }
    bool is_open() const { return fd_ >= 0; }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    static std::string last_error() { return std::generic_category().message(errno); }
    static bool already_exists() { return errno == EEXIST; }

private:
    int fd_ = -1;
#endif
};

Hash sha_file(const NativeFile& file, std::uint64_t size) {
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    std::array<std::uint8_t, 65536> buffer{};
    Hash hash{};
    bool ok = mbedtls_sha1_starts(&ctx) == 0;
    for (std::uint64_t offset = 0; ok && offset < size;) {
        auto length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        auto n = file.read_at(offset, std::span(buffer.data(), length));
        ok = n > 0 && mbedtls_sha1_update(&ctx, buffer.data(), static_cast<std::size_t>(n)) == 0;
        offset += static_cast<std::uint64_t>(n);
    }
    ok = ok && mbedtls_sha1_finish(&ctx, hash.data()) == 0;
    mbedtls_sha1_free(&ctx);
    if (!ok) fail("SHA-1: failed to read file");
    return hash;
}

// Steam manifests carry an all-zero sha_content for empty files rather than SHA-1(""); size already proves their content.
// (DepotDownloader never checks whole-file hashes, only per-chunk Adler32.)
bool content_matches(const NativeFile& contents, const File& file) {
    return file.size == 0 || sha_file(contents, file.size) == file.sha;
}

bool content_matches(const fs::path& path, const File& file) {
    NativeFile contents;
    if (!contents.open_read(path)) {
        auto reason = NativeFile::last_error();
        fail("cannot open file for SHA-1 verification: " + utf8(path) + ": " + reason);
    }
    return content_matches(contents, file);
}

bool up_to_date(const fs::path& path, const File& file) {
    auto status = fs::symlink_status(path);
    return fs::is_regular_file(status) && fs::file_size(path) == file.size && content_matches(path, file);
}

constexpr auto regular_perms = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                               fs::perms::others_read;
constexpr auto exec_perms = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;

// A file being assembled in a temporary sibling of its final path. The temporary file is open only while chunks of
// it are being written, so an item with thousands of changed files does not exhaust the descriptor limit.
class Pending {
public:
    const File& file;

    Pending(const File& source, fs::path destination, std::size_t writes)
        : file(source), final_(std::move(destination)), unwritten_(writes) {
        thread_local std::mt19937_64 random(std::random_device{}());
        NativeFile out;
        for (;;) {
            temp_ = final_;
            temp_ += ".pxsteamdl." + std::to_string(random());
            if (out.create(temp_)) break;
            if (!NativeFile::already_exists()) {
                auto reason = NativeFile::last_error();
                temp_.clear();
                fail("cannot create temporary file for " + utf8(final_) + ": " + reason);
            }
        }
        if (!out.resize(source.size)) {
            auto reason = NativeFile::last_error();
            discard();
            fail("cannot size temporary file for " + utf8(final_) + ": " + reason);
        }
    }
    ~Pending() { discard(); }
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;

    // One of the writes announced to the constructor; safe to call concurrently for disjoint ranges.
    // The file is opened by the first write and closed after the last one, whether or not it succeeded.
    void write(std::uint64_t offset, std::span<const std::uint8_t> data) {
        struct Done {
            Pending& pending;
            ~Done() {
                std::lock_guard lock(pending.mutex_);
                if (--pending.unwritten_ == 0) pending.out_.close();
            }
        } done{*this};
        {
            std::lock_guard lock(mutex_);
            if (!out_.is_open() && !out_.open_write(temp_)) {
                auto reason = NativeFile::last_error();
                fail("cannot open temporary file for " + utf8(final_) + ": " + reason);
            }
        }
        // The handle stays open until this write is counted done.
        if (!out_.write_at(offset, data)) {
            auto reason = NativeFile::last_error();
            fail("write failed: " + utf8(final_) + ": " + reason);
        }
    }

    // Verifies the whole-file hash, then moves the temporary file over the final path. Call after all writes.
    void commit(fs::perms perms) {
        if (!content_matches(temp_, file)) fail("file SHA-1 mismatch: " + file.name);
        fs::permissions(temp_, perms);
        if (fs::is_directory(fs::symlink_status(final_))) fs::remove_all(final_);
        fs::rename(temp_, final_);
        temp_.clear();
    }

private:
    fs::path final_;
    fs::path temp_;
    std::mutex mutex_; // guards out_ opening and closing, and unwritten_
    NativeFile out_;
    std::size_t unwritten_;

    void discard() {
        out_.close();
        std::error_code ignored;
        if (!temp_.empty()) fs::remove(temp_, ignored);
        temp_.clear();
    }
};

std::vector<std::string> cdn_servers(std::uint32_t app_id) {
    auto response = http_request("https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=0&max_servers=20");
    if (response.status != 200) fail("CDN server list: HTTP " + std::to_string(response.status));
    try {
        auto json = nlohmann::json::parse(response.body.begin(), response.body.end());
        std::vector<std::string> hosts;
        for (const auto& server : json.at("response").at("servers")) {
            auto type = server.value("type", "");
            auto support = server.value("https_support", "");
            if ((type != "SteamCache" && type != "CDN") ||
                (support != "mandatory" && support != "preferred" && support != "optional") ||
                !server.contains("host") || !server.at("host").is_string()) continue;
            if (server.contains("allowed_app_ids") && server.at("allowed_app_ids").is_array() &&
                !server.at("allowed_app_ids").empty()) {
                bool allowed = false;
                for (const auto& id : server.at("allowed_app_ids"))
                    if (id.is_number_unsigned() && id.get<std::uint32_t>() == app_id) allowed = true;
                if (!allowed) continue;
            }
            auto host = server.at("host").get<std::string>();
            if (host.empty() ||
                host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
                continue;
            hosts.push_back(std::move(host));
        }
        if (hosts.empty()) fail("CDN server list: no HTTPS SteamCache/CDN servers");
        return hosts;
    } catch (const nlohmann::json::exception& e) {
        fail("CDN server list: invalid JSON: " + std::string(e.what()));
    }
}

// Tries the hosts in turn, starting at host and leaving it at the one that answered. decode throws to reject a bad
// response, which moves on to the next host like an HTTP error does.
template <class Decode>
auto fetch_from_cdn(const std::vector<std::string>& hosts, const std::string& path, std::size_t& host,
                    const std::stop_token& stop, Decode decode) {
    std::string last_error;
    std::size_t attempts = std::min<std::size_t>(hosts.size() * 2, 6);
    for (std::size_t attempt = 0; attempt < attempts; ++attempt, ++host) {
        if (stop.stop_requested()) fail("cancelled");
        const auto& name = hosts[host % hosts.size()];
        try {
            auto response = http_request("https://" + name + path);
            if (response.status == 200) return decode(std::move(response.body));
            last_error = name + ": HTTP " + std::to_string(response.status);
        } catch (const std::exception& e) {
            last_error = name + ": " + e.what();
        }
    }
    fail("CDN request failed for " + path + ": " + last_error);
}

void ensure_directory(const fs::path& path) {
    auto status = fs::symlink_status(path);
    if (fs::is_directory(status)) return;
    if (fs::exists(status)) fs::remove_all(path);
    fs::create_directory(path);
}

void prune(const fs::path& root, const fs::path& dir, const std::set<std::string>& expected) {
    std::vector<fs::path> children;
    for (const auto& entry : fs::directory_iterator(dir)) children.push_back(entry.path());
    for (const auto& child : children) {
        if (!expected.contains(path_key(utf8(child.lexically_relative(root))))) fs::remove_all(child);
        else if (fs::is_directory(fs::symlink_status(child))) prune(root, child, expected);
    }
}

struct ItemState {
    std::size_t index;
    ItemJob* job;
    fs::path root;
    Key key{};
    const std::vector<std::string>* hosts = nullptr;
    std::string chunk_prefix;
    std::vector<File> files;
    std::set<std::string> expected; // path_key of every path the item consists of
    std::vector<std::unique_ptr<Pending>> pending;
    std::vector<std::pair<Pending*, const Chunk*>> chunks;
    std::size_t next_chunk = 0; // next to hand out; guarded by Downloader::queue_mutex_
    std::atomic<std::size_t> remaining{0};
    std::atomic<bool> failed{false};
    std::mutex mutex; // guards done, error and progress calls
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
};

// One download() call: planners resolve items into chunk tasks, a shared worker pool fetches chunks of all
// items, so a large item keeps every worker busy instead of only its own share.
class Downloader {
public:
    Downloader(Session& session, std::span<ItemJob> jobs, unsigned planners, unsigned workers, std::stop_token stop)
        : session_(session), jobs_(jobs), states_(jobs.size()),
          planners_(std::clamp<std::size_t>(planners, 1, jobs.size())), workers_(std::max(1u, workers)),
          stop_(std::move(stop)) {}

    void run() {
        active_planners_ = planners_;
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < workers_; ++i) threads.emplace_back([this, i] { work(i); });
        for (std::size_t i = 0; i < planners_; ++i) threads.emplace_back([this, i] { plan_items(i); });
    }

private:
    Session& session_;
    std::span<ItemJob> jobs_;
    std::vector<std::unique_ptr<ItemState>> states_;
    std::size_t planners_;
    std::size_t workers_;
    std::stop_token stop_;
    std::atomic<std::size_t> next_item_{0};

    std::mutex cache_mutex_;
    std::map<std::uint32_t, Key> keys_;
    std::map<std::uint32_t, std::vector<std::string>> hosts_;

    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::condition_variable item_slot_free_;
    // Items with chunks left to hand out. Workers rotate over the first planners_ of them, so parallel_items items
    // download side by side and the next one starts as soon as one of them has no chunks left, instead of queueing
    // behind every chunk of the others.
    std::deque<ItemState*> ready_;
    std::size_t active_planners_ = 0;
    std::size_t open_items_ = 0; // planned but not finished; bounds open temporary files

    Key depot_key(std::uint32_t depot, std::uint32_t app_id) {
        std::lock_guard lock(cache_mutex_);
        if (auto it = keys_.find(depot); it != keys_.end()) return it->second;
        Bytes reply = session_.request(emsg::client_get_depot_decryption_key,
                                       concat({encode_uint(1, depot), encode_uint(2, app_id)}));
        Key key{};
        std::uint64_t eresult = 0;
        bool has_key = false;
        Reader reader(reply);
        Field field;
        while (reader.next(field)) {
            if (field.number == 1 && field.wire == 0) eresult = field.integer;
            else if (field.number == 3 && field.wire == 2) {
                if (field.bytes.size() != key.size()) fail("depot key response: invalid key length");
                std::copy(field.bytes.begin(), field.bytes.end(), key.begin());
                has_key = true;
            }
        }
        if (eresult != 1 || !has_key) fail("depot key request failed: eresult " + std::to_string(eresult));
        return keys_.emplace(depot, key).first->second;
    }

    const std::vector<std::string>& cdn_hosts(std::uint32_t app_id) {
        std::lock_guard lock(cache_mutex_);
        if (auto it = hosts_.find(app_id); it != hosts_.end()) return it->second;
        return hosts_.emplace(app_id, cdn_servers(app_id)).first->second;
    }

    void plan_items(std::size_t planner) {
        for (std::size_t i; (i = next_item_++) < jobs_.size();) {
            {
                std::unique_lock lock(queue_mutex_);
                item_slot_free_.wait(lock, [&] { return open_items_ < 2 * planners_; });
                ++open_items_;
            }
            bool queued = false;
            try {
                queued = plan(i, planner);
            } catch (const std::exception& e) {
                jobs_[i].error = e.what();
                states_[i].reset();
            }
            if (!queued) close_item();
        }
        std::lock_guard lock(queue_mutex_);
        if (--active_planners_ == 0) queue_ready_.notify_all();
    }

    void close_item() {
        std::lock_guard lock(queue_mutex_);
        --open_items_;
        item_slot_free_.notify_all();
    }

    // Returns whether chunk tasks were queued; otherwise the item is already complete.
    bool plan(std::size_t index, std::size_t planner) {
        ItemJob& job = jobs_[index];
        if (stop_.stop_requested()) fail("cancelled");
        const Item& item = *job.item;
        if (job.destination.empty()) fail("download destination is empty");
        const fs::path& root = job.destination;
        if (!item.file_url.empty() && item.manifest_id == 0) {
            download_legacy(job, root);
            return false;
        }
        if (!item.manifest_id || !item.app_id) fail("item has no SteamPipe manifest or app ID");
        auto depot = item.app_id;
        states_[index] = std::make_unique<ItemState>();
        ItemState& state = *states_[index];
        state.index = index;
        state.job = &job;
        state.root = root;
        state.key = depot_key(depot, item.app_id);

        auto code_reply = session_.rpc(
            "ContentServerDirectory.GetManifestRequestCode#1",
            concat({encode_uint(1, item.app_id), encode_uint(2, depot), encode_uint(3, item.manifest_id)}));
        std::uint64_t code = 0;
        Reader code_reader(code_reply);
        Field field;
        while (code_reader.next(field)) if (field.number == 1 && field.wire == 0) code = field.integer;
        if (!code) fail("manifest request code is zero");

        state.hosts = &cdn_hosts(item.app_id);
        std::string manifest_path = "/depot/" + std::to_string(depot) + "/manifest/" +
                                    std::to_string(item.manifest_id) + "/5/" + std::to_string(code);
        std::size_t host = planner;
        state.files = fetch_from_cdn(*state.hosts, manifest_path, host, stop_, [&](const Bytes& zip) {
            return parse_manifest(zip, state.key, depot, item.manifest_id);
        });
        state.chunk_prefix = "/depot/" + std::to_string(depot) + "/chunk/";

        std::set<std::string> declared; // path keys of the manifest entries
        std::set<std::string> dirs;     // directories to create
        std::set<std::string> dir_keys;
        auto add_directory = [&](const std::string& name) {
            dirs.insert(name);
            dir_keys.insert(path_key(name));
            state.expected.insert(path_key(name));
        };
        for (const auto& file : state.files) {
            if (!declared.insert(path_key(file.name)).second) fail("manifest: duplicate path: " + file.name);
            state.expected.insert(path_key(file.name));
            if (file.is_directory()) add_directory(file.name);
            for (auto parent = utf8_path(file.name).parent_path(); !parent.empty(); parent = parent.parent_path())
                add_directory(utf8(parent));
        }
        for (const auto& file : state.files)
            if (!file.is_directory() && dir_keys.contains(path_key(file.name)))
                fail("manifest: file/directory path conflict: " + file.name);

        if (fs::is_symlink(fs::symlink_status(root))) fail("destination must not be a symlink");
        fs::create_directories(root);
        std::vector<std::string> sorted_dirs(dirs.begin(), dirs.end());
        std::sort(sorted_dirs.begin(), sorted_dirs.end(), [](const auto& a, const auto& b) {
            auto depth_a = std::count(a.begin(), a.end(), '/');
            auto depth_b = std::count(b.begin(), b.end(), '/');
            return depth_a == depth_b ? a < b : depth_a < depth_b;
        });
        for (const auto& dir : sorted_dirs) ensure_directory(root / utf8_path(dir));

        for (const auto& file : state.files) {
            if (!file.is_regular()) continue;
            auto path = root / utf8_path(file.name);
            if (up_to_date(path, file)) {
                if (file.is_executable()) fs::permissions(path, exec_perms, fs::perm_options::add);
                continue;
            }
            state.pending.push_back(std::make_unique<Pending>(file, path, file.chunks.size()));
            auto* output = state.pending.back().get();
            for (const auto& chunk : file.chunks) {
                state.chunks.emplace_back(output, &chunk);
                state.total += chunk.original;
            }
        }

        if (state.chunks.empty()) {
            if (job.progress) job.progress(0, 0);
            finish(state);
            return false;
        }
        if (stop_.stop_requested()) fail("cancelled");
        state.remaining = state.chunks.size();
        std::lock_guard lock(queue_mutex_);
        ready_.push_back(&state);
        queue_ready_.notify_all();
        return true;
    }

    void work(std::size_t worker) {
        std::size_t host = worker; // each worker sticks to one CDN host and moves on only after a failure
        for (;;) {
            ItemState* item;
            std::pair<Pending*, const Chunk*> task;
            {
                std::unique_lock lock(queue_mutex_);
                queue_ready_.wait(lock, [&] { return !ready_.empty() || active_planners_ == 0; });
                if (ready_.empty()) return;
                item = ready_.front();
                ready_.pop_front();
                task = item->chunks[item->next_chunk++];
                // Back into the rotation window; the state lives until its last handed-out chunk completes.
                if (item->next_chunk < item->chunks.size())
                    ready_.insert(ready_.begin() + std::min(planners_ - 1, ready_.size()), item);
            }
            ItemState& state = *item;
            auto [output, chunk] = task;
            if (!state.failed) {
                try {
                    fetch_chunk(state, *output, *chunk, host);
                    std::lock_guard lock(state.mutex);
                    state.done += chunk->original;
                    if (state.job->progress) state.job->progress(state.done, state.total);
                } catch (const std::exception& e) {
                    std::lock_guard lock(state.mutex);
                    if (!state.failed) state.error = e.what();
                    state.failed = true;
                }
            }
            if (--state.remaining == 0) {
                finish(state);
                close_item();
            }
        }
    }

    void fetch_chunk(const ItemState& state, Pending& output, const Chunk& chunk, std::size_t& host) const {
        Bytes data = fetch_from_cdn(*state.hosts, state.chunk_prefix + hex_hash(chunk.sha), host, stop_,
                                    [&](const Bytes& body) { return expand_chunk(body, state.key, chunk); });
        output.write(chunk.offset, data); // a local write error is not worth retrying on another host
    }

    // Runs on the thread that completed the item's last chunk (or its planner when nothing was needed).
    void finish(ItemState& state) {
        if (!state.failed) {
            try {
                finalize(state);
            } catch (const std::exception& e) {
                state.error = e.what();
                state.failed = true;
            }
        }
        if (state.failed) state.job->error = state.error;
        states_[state.index].reset();
    }

    static void finalize(ItemState& state) {
        for (auto& output : state.pending) {
            output->commit(output->file.is_executable() ? regular_perms | exec_perms : regular_perms);
        }
        for (const auto& file : state.files) {
            if (!file.is_symlink()) continue;
            auto path = state.root / utf8_path(file.name);
            auto target = utf8_path(file.link_target).make_preferred();
            if (fs::is_symlink(fs::symlink_status(path)) && fs::read_symlink(path) == target) continue;
            fs::remove_all(path);
            std::error_code error;
            // Windows distinguishes directory symlinks; elsewhere both calls are the same.
            if (fs::is_directory(path.parent_path() / target)) fs::create_directory_symlink(target, path, error);
            else fs::create_symlink(target, path, error);
            if (error) {
                auto message = "cannot create symlink " + file.name + " -> " + file.link_target + ": " + error.message();
#ifdef _WIN32
                message += " (Windows allows symlinks only with Developer Mode enabled or as administrator)";
#endif
                fail(message);
            }
        }
        prune(state.root, state.root, state.expected);
    }

    static void download_legacy(const ItemJob& job, const fs::path& root) {
        const Item& item = *job.item;
        std::string filename = item.filename;
        std::replace(filename.begin(), filename.end(), '\\', '/');
        auto name = utf8_path(filename).filename();
        if (name.empty() || name == "." || name == ".." || windows_unsafe(utf8(name)))
            fail("legacy item has no safe filename");
        if (fs::is_symlink(fs::symlink_status(root))) fail("destination must not be a symlink");
        fs::create_directories(root);
        auto response = http_request(item.file_url);
        if (response.status != 200) fail("legacy file: HTTP " + std::to_string(response.status));
        File legacy;
        legacy.name = utf8(name);
        legacy.size = response.body.size();
        // commit() verifies what reached the disk against this.
        if (mbedtls_sha1(response.body.data(), response.body.size(), legacy.sha.data()) != 0) fail("SHA-1 failed");
        Pending output(legacy, root / name, 1);
        output.write(0, response.body);
        output.commit(regular_perms);
        if (job.progress) job.progress(legacy.size, legacy.size);
    }
};

} // namespace

void download_items(Session& session, std::span<ItemJob> jobs, unsigned parallel_items, unsigned threads_per_item,
                    std::stop_token stop) {
    if (jobs.empty()) return;
    Downloader(session, jobs, parallel_items, std::max(1u, parallel_items) * std::max(1u, threads_per_item),
               std::move(stop)).run();
}

} // namespace pxsteamdl::detail
