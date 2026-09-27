// SPDX-License-Identifier: LGPL-3.0-or-later
#include "internal.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <lzma.h>
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
#include <set>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

struct File {
    std::string name;
    std::string link_target;
    std::uint64_t size = 0;
    std::uint32_t flags = 0;
    Hash sha{};
    std::vector<Chunk> chunks;
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
    if (encrypted.size() < 32 || (encrypted.size() - 16) % 16 || encrypted.size() > INT_MAX)
        fail("AES: invalid ciphertext length");
    using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) fail("AES: cipher initialization failed");
    std::array<std::uint8_t, 16> iv{};
    int count = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ecb(), nullptr, key.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1 ||
        EVP_DecryptUpdate(ctx.get(), iv.data(), &count, encrypted.data(), 16) != 1 || count != 16 ||
        EVP_DecryptFinal_ex(ctx.get(), iv.data() + count, &count) != 1)
        fail("AES: IV decryption failed");

    Bytes plain(encrypted.size() - 16);
    int written = 0, final = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx.get(), 1) != 1 ||
        EVP_DecryptUpdate(ctx.get(), plain.data(), &written, encrypted.data() + 16,
                          static_cast<int>(encrypted.size() - 16)) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), plain.data() + written, &final) != 1)
        fail("AES: CBC decryption or PKCS7 padding failed");
    plain.resize(written + final);
    return plain;
}

std::string decrypt_name(std::string_view encoded, const Key& key) {
    std::string base64;
    base64.reserve(encoded.size());
    for (char c : encoded) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        base64.push_back(c);
    }
    if (base64.empty() || base64.size() % 4 || base64.size() > INT_MAX) fail("manifest: invalid base64 filename");
    Bytes encrypted(base64.size() / 4 * 3);
    int length = EVP_DecodeBlock(encrypted.data(), reinterpret_cast<const unsigned char*>(base64.data()),
                                 static_cast<int>(base64.size()));
    if (length < 0) fail("manifest: invalid base64 filename");
    std::size_t padding = (base64.back() == '=') + (base64.size() > 1 && base64[base64.size() - 2] == '=');
    encrypted.resize(static_cast<std::size_t>(length) - padding);
    Bytes plain = decrypt(encrypted, key);
    while (!plain.empty() && plain.back() == 0) plain.pop_back();
    return {reinterpret_cast<const char*>(plain.data()), plain.size()};
}

void normalize_path(std::string& path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\0') != std::string::npos || fs::path(path).is_absolute())
        fail("manifest: unsafe path: " + path);
    std::size_t from = 0;
    for (std::size_t end; from < path.size(); from = end + 1) {
        end = path.find('/', from);
        if (end == std::string::npos) end = path.size();
        std::string_view part(path.data() + from, end - from);
        if (part.empty() || part == "." || part == "..") fail("manifest: unsafe path: " + path);
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
    if (!(file.flags & (0x40 | 0x200)) && !has_hash) fail("manifest: file missing SHA-1");
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
        if (file.flags & 0x200) {
            normalize_path(file.link_target);
            if (!file.chunks.empty()) fail("manifest: symlink contains chunks");
        } else if (file.flags & 0x40) {
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

Hash sha_fd(int fd, std::uint64_t size) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1) fail("SHA-1: initialization failed");
    std::array<std::uint8_t, 65536> buffer{};
    for (std::uint64_t offset = 0; offset < size;) {
        std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        auto n = pread(fd, buffer.data(), length, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("SHA-1: failed to read file");
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), n) != 1) fail("SHA-1: digest update failed");
        offset += static_cast<std::size_t>(n);
    }
    Hash hash{};
    unsigned length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &length) != 1 || length != hash.size())
        fail("SHA-1: finalization failed");
    return hash;
}

// Steam manifests carry an all-zero sha_content for empty files rather than SHA-1(""); size already proves their content.
// (DepotDownloader never checks whole-file hashes, only per-chunk Adler32.)
bool content_matches(int fd, const File& file) {
    return file.size == 0 || sha_fd(fd, file.size) == file.sha;
}

bool up_to_date(const fs::path& path, const File& file) {
    if (!fs::is_regular_file(fs::symlink_status(path))) return false;
    if (fs::file_size(path) != file.size) return false;
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) fail("cannot open file for SHA-1 verification: " + path.string() + ": " + std::strerror(errno));
    try {
        bool matches = content_matches(fd, file);
        close(fd);
        return matches;
    } catch (...) {
        close(fd);
        throw;
    }
}

struct Pending {
    const File* file;
    fs::path final;
    std::string temp;
    int fd = -1;

    Pending(const File& source, fs::path destination) : file(&source), final(std::move(destination)) {
        temp = final.string() + ".pxsteamdl.XXXXXX";
        fd = mkstemp(temp.data());
        if (fd < 0) fail("cannot create temporary file for " + final.string() + ": " + std::strerror(errno));
        if (ftruncate(fd, static_cast<off_t>(source.size)) != 0) {
            auto reason = std::string(std::strerror(errno));
            close(fd);
            unlink(temp.c_str());
            fail("cannot size temporary file for " + final.string() + ": " + reason);
        }
    }
    ~Pending() {
        if (fd >= 0) close(fd);
        if (!temp.empty()) unlink(temp.c_str());
    }
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;
};

void write_chunk(int fd, const Chunk& chunk, const Bytes& bytes) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        auto n = pwrite(fd, bytes.data() + pos, bytes.size() - pos, static_cast<off_t>(chunk.offset + pos));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("chunk: pwrite failed: " + std::string(std::strerror(errno)));
        pos += static_cast<std::size_t>(n);
    }
}

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

Bytes get_from_cdn(const std::vector<std::string>& hosts, std::string_view path, std::size_t start) {
    std::string last_error;
    std::size_t attempts = std::min<std::size_t>(hosts.size() * 2, 6);
    for (std::size_t i = 0; i < attempts; ++i) {
        const auto& host = hosts[(start + i) % hosts.size()];
        try {
            auto response = http_request("https://" + host + std::string(path));
            if (response.status == 200) return std::move(response.body);
            last_error = host + ": HTTP " + std::to_string(response.status);
        } catch (const std::exception& e) {
            last_error = host + ": " + e.what();
        }
    }
    fail("CDN request failed for " + std::string(path) + ": " + last_error);
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
        auto relative = child.lexically_relative(root).generic_string();
        if (!expected.contains(relative)) fs::remove_all(child);
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
    std::set<std::string> expected;
    std::vector<std::unique_ptr<Pending>> pending;
    std::vector<std::pair<Pending*, const Chunk*>> chunks;
    std::atomic<std::size_t> remaining{0};
    std::atomic<bool> failed{false};
    std::mutex mutex; // guards done, error and progress calls
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
};

struct Task {
    ItemState* state;
    Pending* output;
    const Chunk* chunk;
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
    std::deque<Task> queue_;
    std::size_t active_planners_ = 0;
    std::size_t open_items_ = 0; // planned but not finished; bounds open temporary files

    Key depot_key(std::uint32_t depot, std::uint32_t app_id) {
        std::lock_guard lock(cache_mutex_);
        if (auto it = keys_.find(depot); it != keys_.end()) return it->second;
        Bytes request = encode_uint(1, depot);
        auto app_field = encode_uint(2, app_id);
        request.insert(request.end(), app_field.begin(), app_field.end());
        Bytes reply = session_.request(5438, request);
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
        fs::path root(job.destination);
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

        Bytes code_request = encode_uint(1, item.app_id);
        auto depot_field = encode_uint(2, depot);
        auto gid_field = encode_uint(3, item.manifest_id);
        code_request.insert(code_request.end(), depot_field.begin(), depot_field.end());
        code_request.insert(code_request.end(), gid_field.begin(), gid_field.end());
        auto code_reply = session_.rpc("ContentServerDirectory.GetManifestRequestCode#1", code_request);
        std::uint64_t code = 0;
        Reader code_reader(code_reply);
        Field field;
        while (code_reader.next(field)) if (field.number == 1 && field.wire == 0) code = field.integer;
        if (!code) fail("manifest request code is zero");

        state.hosts = &cdn_hosts(item.app_id);
        std::string manifest_path = "/depot/" + std::to_string(depot) + "/manifest/" +
                                    std::to_string(item.manifest_id) + "/5/" + std::to_string(code);
        Bytes manifest_zip = get_from_cdn(*state.hosts, manifest_path, planner);
        state.files = parse_manifest(manifest_zip, state.key, depot, item.manifest_id);
        state.chunk_prefix = "/depot/" + std::to_string(depot) + "/chunk/";

        std::set<std::string> declared;
        std::set<std::string> dirs;
        for (const auto& file : state.files) {
            if (!declared.insert(file.name).second) fail("manifest: duplicate path: " + file.name);
            state.expected.insert(file.name);
            if (file.flags & 0x40) dirs.insert(file.name);
            for (auto parent = fs::path(file.name).parent_path(); !parent.empty(); parent = parent.parent_path()) {
                state.expected.insert(parent.generic_string());
                dirs.insert(parent.generic_string());
            }
        }
        for (const auto& file : state.files)
            if (dirs.contains(file.name) && !(file.flags & 0x40)) fail("manifest: file/directory path conflict: " + file.name);

        if (fs::is_symlink(fs::symlink_status(root))) fail("destination must not be a symlink");
        fs::create_directories(root);
        std::vector<std::string> sorted_dirs(dirs.begin(), dirs.end());
        std::sort(sorted_dirs.begin(), sorted_dirs.end(), [](const auto& a, const auto& b) {
            auto depth_a = std::count(a.begin(), a.end(), '/');
            auto depth_b = std::count(b.begin(), b.end(), '/');
            return depth_a == depth_b ? a < b : depth_a < depth_b;
        });
        for (const auto& dir : sorted_dirs) ensure_directory(root / dir);

        for (const auto& file : state.files) {
            if (file.flags & (0x40 | 0x200)) continue;
            auto path = root / file.name;
            if (up_to_date(path, file)) {
                if (file.flags & (0x20 | 0x80))
                    fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                                    fs::perm_options::add);
                continue;
            }
            state.pending.push_back(std::make_unique<Pending>(file, path));
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
        for (auto [output, chunk] : state.chunks) queue_.push_back({&state, output, chunk});
        queue_ready_.notify_all();
        return true;
    }

    void work(std::size_t worker) {
        std::size_t host = worker; // each worker sticks to one CDN host and moves on only after a failure
        for (;;) {
            Task task;
            {
                std::unique_lock lock(queue_mutex_);
                queue_ready_.wait(lock, [&] { return !queue_.empty() || active_planners_ == 0; });
                if (queue_.empty()) return;
                task = queue_.front();
                queue_.pop_front();
            }
            ItemState& state = *task.state;
            if (!state.failed) {
                try {
                    fetch_chunk(state, *task.output, *task.chunk, host);
                    std::lock_guard lock(state.mutex);
                    state.done += task.chunk->original;
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

    void fetch_chunk(const ItemState& state, const Pending& output, const Chunk& chunk, std::size_t& host) const {
        const auto& hosts = *state.hosts;
        auto path = state.chunk_prefix + hex_hash(chunk.sha);
        std::string last_error;
        std::size_t attempts = std::min<std::size_t>(hosts.size() * 2, 6);
        for (std::size_t attempt = 0; attempt < attempts; ++attempt, ++host) {
            if (stop_.stop_requested()) fail("cancelled");
            const auto& name = hosts[host % hosts.size()];
            try {
                auto response = http_request("https://" + name + path);
                if (response.status != 200) {
                    last_error = name + ": HTTP " + std::to_string(response.status);
                    continue;
                }
                write_chunk(output.fd, chunk, expand_chunk(response.body, state.key, chunk));
                return;
            } catch (const std::exception& e) {
                last_error = name + ": " + e.what();
            }
        }
        fail("chunk " + hex_hash(chunk.sha) + " failed: " + last_error);
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
            if (!content_matches(output->fd, *output->file)) fail("file SHA-1 mismatch: " + output->file->name);
            auto mode = static_cast<mode_t>(0644 | ((output->file->flags & (0x20 | 0x80)) ? 0111 : 0));
            if (fchmod(output->fd, mode) != 0) fail("chmod failed for " + output->file->name + ": " + std::strerror(errno));
            if (fs::is_directory(fs::symlink_status(output->final))) fs::remove_all(output->final);
            fs::rename(output->temp, output->final);
            output->temp.clear();
        }
        for (const auto& file : state.files) {
            if (!(file.flags & 0x200)) continue;
            auto path = state.root / file.name;
            if (fs::is_symlink(fs::symlink_status(path)) && fs::read_symlink(path) == fs::path(file.link_target)) continue;
            fs::remove_all(path);
            fs::create_symlink(file.link_target, path);
        }
        prune(state.root, state.root, state.expected);
    }

    static void download_legacy(const ItemJob& job, const fs::path& root) {
        const Item& item = *job.item;
        std::string filename = item.filename;
        std::replace(filename.begin(), filename.end(), '\\', '/');
        auto name = fs::path(filename).filename().string();
        if (name.empty() || name == "." || name == "..") fail("legacy item has no safe filename");
        if (fs::is_symlink(fs::symlink_status(root))) fail("destination must not be a symlink");
        fs::create_directories(root);
        auto response = http_request(item.file_url);
        if (response.status != 200) fail("legacy file: HTTP " + std::to_string(response.status));
        File legacy;
        legacy.size = response.body.size();
        Pending output(legacy, root / name);
        std::size_t offset = 0;
        while (offset < response.body.size()) {
            auto n = write(output.fd, response.body.data() + offset, response.body.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) fail("legacy file write failed: " + std::string(std::strerror(errno)));
            offset += static_cast<std::size_t>(n);
        }
        if (fchmod(output.fd, 0644) != 0) fail("legacy file chmod failed: " + std::string(std::strerror(errno)));
        fs::rename(output.temp, output.final);
        output.temp.clear();
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
