// SPDX-License-Identifier: LGPL-3.0-or-later
#include "internal.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <mbedtls/threading.h>
#include <windows.h>
#endif

namespace pxsteamdl::detail {

// Generated from the pinned Mozilla CA bundle by CMakeLists.txt; NUL-terminated so mbedTLS parses it in place.
extern const unsigned char ca_bundle[];
extern const std::size_t ca_bundle_size;

namespace {

#ifdef _WIN32
// MBEDTLS_THREADING_ALT mutexes (cmake/mbedtls): an SRWLOCK is one zero-initialized pointer and needs no cleanup.
PSRWLOCK srw(mbedtls_threading_mutex_t* mutex) { return reinterpret_cast<PSRWLOCK>(&mutex->lock); }
void mutex_init(mbedtls_threading_mutex_t* mutex) { InitializeSRWLock(srw(mutex)); }
void mutex_free(mbedtls_threading_mutex_t*) {}
int mutex_lock(mbedtls_threading_mutex_t* mutex) {
    AcquireSRWLockExclusive(srw(mutex));
    return 0;
}
int mutex_unlock(mbedtls_threading_mutex_t* mutex) {
    ReleaseSRWLockExclusive(srw(mutex));
    return 0;
}
#endif

void init_curl() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        // Must precede every other mbedTLS call; curl_global_init starts PSA crypto.
        mbedtls_threading_set_alt(mutex_init, mutex_free, mutex_lock, mutex_unlock);
#endif
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("curl_global_init failed");
    });
}

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<Bytes*>(user);
    body->insert(body->end(), data, data + size * count);
    return size * count;
}

std::uint64_t parse_u64(const nlohmann::json& v) {
    if (v.is_number()) return v.get<std::uint64_t>();
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        if (s.empty()) return 0;
        return std::stoull(s);
    }
    return 0;
}

std::string string_field(const nlohmann::json& entry, const char* key) {
    auto it = entry.find(key);
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::uint64_t u64_field(const nlohmann::json& entry, const char* key) {
    auto it = entry.find(key);
    return it != entry.end() ? parse_u64(*it) : 0;
}

// One publishedfiledetails entry; a malformed entry fails only its own item.
Item parse_item(std::uint64_t id, const nlohmann::json& entry) {
    Item item;
    item.id = id;
    try {
        if (auto result = u64_field(entry, "result"); result != 1) {
            item.error = "Steam result " + std::to_string(result);
            return item;
        }
        item.manifest_id = u64_field(entry, "hcontent_file");
        item.app_id = static_cast<std::uint32_t>(u64_field(entry, "consumer_app_id"));
        item.title = string_field(entry, "title");
        item.file_url = string_field(entry, "file_url");
        item.filename = string_field(entry, "filename");
    } catch (const std::exception& e) {
        item.error = std::string("invalid item details: ") + e.what();
    }
    return item;
}

} // namespace

void set_ca_bundle(CURL* curl) {
    curl_blob blob{const_cast<unsigned char*>(ca_bundle), ca_bundle_size, CURL_BLOB_NOCOPY};
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
}

HttpResponse http_request(std::string_view url, std::string_view method, std::string_view body,
                          std::string_view content_type) {
    init_curl();
    thread_local std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
    CURL* curl = handle.get();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    curl_easy_reset(curl); // keeps connection cache, clears options

    std::string url_str(url), method_str(method), header;
    HttpResponse response;
    curl_slist* headers = nullptr;
    if (!content_type.empty()) {
        header = "Content-Type: " + std::string(content_type);
        headers = curl_slist_append(nullptr, header.c_str());
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers_guard(headers, &curl_slist_free_all);

    char error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    set_ca_bundle(curl);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (method_str == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else if (method_str != "GET") {
        throw std::runtime_error("http_request: unsupported method " + method_str);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        throw std::runtime_error(method_str + " " + url_str + " failed: " + (error[0] ? error : curl_easy_strerror(rc)));
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

std::vector<Item> fetch_items(std::span<const std::uint64_t> ids) {
    constexpr std::size_t batch = 100;
    std::vector<Item> items;
    items.reserve(ids.size());

    for (std::size_t first = 0; first < ids.size(); first += batch) {
        auto chunk = ids.subspan(first, std::min(batch, ids.size() - first));
        std::string form = "itemcount=" + std::to_string(chunk.size());
        for (std::size_t i = 0; i < chunk.size(); ++i)
            form += "&publishedfileids%5B" + std::to_string(i) + "%5D=" + std::to_string(chunk[i]);

        constexpr std::string_view url = "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
        HttpResponse response = http_request(url, "POST", form, "application/x-www-form-urlencoded");
        if (response.status != 200)
            throw std::runtime_error("GetPublishedFileDetails: HTTP " + std::to_string(response.status));

        auto root = nlohmann::json::parse(response.body.begin(), response.body.end());
        const auto& details = root.at("response").at("publishedfiledetails");
        if (!details.is_array())
            throw std::runtime_error("GetPublishedFileDetails: response lacks publishedfiledetails array");

        std::unordered_map<std::uint64_t, const nlohmann::json*> by_id;
        for (const auto& entry : details) {
            try {
                if (entry.is_object() && entry.contains("publishedfileid"))
                    by_id.emplace(parse_u64(entry["publishedfileid"]), &entry);
            } catch (const std::exception&) {
                // an unparsable ID matches no request
            }
        }
        for (std::uint64_t id : chunk) {
            auto found = by_id.find(id);
            if (found != by_id.end()) {
                items.push_back(parse_item(id, *found->second));
            } else {
                Item& item = items.emplace_back();
                item.id = id;
                item.error = "not returned by Steam";
            }
        }
    }
    return items;
}

} // namespace pxsteamdl::detail
