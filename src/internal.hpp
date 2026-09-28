// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pxsteamdl::detail {

using Bytes = std::vector<std::uint8_t>;

struct HttpResponse {
    long status = 0;
    Bytes body;
};

HttpResponse http_request(std::string_view url, std::string_view method = "GET",
                          std::string_view body = {}, std::string_view content_type = {});

// Makes a curl easy handle (CURL*) trust the Mozilla CA bundle compiled into the library; mbedTLS has no access
// to the OS trust store. Must be set again after curl_easy_reset().
void set_ca_bundle(void* curl);

struct Item {
    std::uint64_t id = 0;
    std::uint64_t manifest_id = 0;
    std::uint32_t app_id = 0;
    std::string title;
    std::string file_url;
    std::string filename;
    std::string error;
};

std::vector<Item> fetch_items(std::span<const std::uint64_t> ids);

// Steam CM message types (SteamKit2 EMsg).
namespace emsg {
inline constexpr std::uint32_t multi = 1;
inline constexpr std::uint32_t service_method_call = 151;
inline constexpr std::uint32_t client_heartbeat = 703;
inline constexpr std::uint32_t client_log_off = 706;
inline constexpr std::uint32_t client_logon_response = 751;
inline constexpr std::uint32_t client_logged_off = 757;
inline constexpr std::uint32_t client_get_depot_decryption_key = 5438;
inline constexpr std::uint32_t client_logon = 5514;
} // namespace emsg

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Logs in anonymously; a no-op while connected. rpc() and request() call it to recover from a dropped
    // connection, so a long-lived session survives Steam closing the socket between downloads.
    void connect();
    Bytes rpc(std::string_view method, std::span<const std::uint8_t> body);
    Bytes request(std::uint32_t type, std::span<const std::uint8_t> body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ItemJob {
    const Item* item;
    std::filesystem::path destination;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
    std::string error; // set when the item fails
};

// Downloads all jobs: up to parallel_items items are resolved concurrently, and their chunks share a pool of
// parallel_items * threads_per_item workers. Once stop is requested, unfinished jobs fail with "cancelled".
void download_items(Session& session, std::span<ItemJob> jobs, unsigned parallel_items, unsigned threads_per_item,
                    std::stop_token stop);

Bytes encode_uint(std::uint32_t field, std::uint64_t value);
Bytes encode_fixed32(std::uint32_t field, std::uint32_t value);
Bytes encode_fixed64(std::uint32_t field, std::uint64_t value);
Bytes encode_bytes(std::uint32_t field, std::span<const std::uint8_t> value);
Bytes encode_string(std::uint32_t field, std::string_view value);
Bytes concat(std::initializer_list<std::span<const std::uint8_t>> parts);

struct Field {
    std::uint32_t number;
    std::uint32_t wire;
    std::uint64_t integer = 0;
    std::span<const std::uint8_t> bytes;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> input) : input_(input) {}
    bool next(Field& field);
private:
    std::span<const std::uint8_t> input_;
    std::size_t offset_ = 0;
};

} // namespace pxsteamdl::detail
