// SPDX-License-Identifier: LGPL-3.0-or-later
#include "http.hpp"

#include <memory>
#include <mutex>
#include <string>

#include <curl/curl.h>

#ifdef _WIN32
#include <mbedtls/threading.h>
#include <windows.h>
#endif

namespace pxsteamdl::detail {

// Generated from the pinned Mozilla CA bundle by CMakeLists.txt; NUL-terminated so mbedTLS parses it in place.
extern const unsigned char kCaBundle[];
extern const std::size_t kCaBundleSize;

namespace {

constexpr long kConnectTimeoutSeconds = 10;
// A transfer slower than 1 byte/s for this long is aborted.
constexpr long kStallTimeoutSeconds = 30;

#ifdef _WIN32
// MBEDTLS_THREADING_ALT mutexes (cmake/mbedtls): an SRWLOCK is one zero-initialized pointer and needs no cleanup.
PSRWLOCK SrwLock(mbedtls_threading_mutex_t* mutex) { return reinterpret_cast<PSRWLOCK>(&mutex->lock); }
void MutexInit(mbedtls_threading_mutex_t* mutex) { InitializeSRWLock(SrwLock(mutex)); }
void MutexFree(mbedtls_threading_mutex_t*) {}
int MutexLock(mbedtls_threading_mutex_t* mutex) {
    AcquireSRWLockExclusive(SrwLock(mutex));
    return 0;
}
int MutexUnlock(mbedtls_threading_mutex_t* mutex) {
    ReleaseSRWLockExclusive(SrwLock(mutex));
    return 0;
}
#endif

void InitCurl() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        // Must precede every other mbedTLS call; curl_global_init starts PSA crypto.
        mbedtls_threading_set_alt(MutexInit, MutexFree, MutexLock, MutexUnlock);
#endif
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) Fail("curl_global_init failed");
    });
}

std::size_t AppendBody(char* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<Bytes*>(user);
    body->insert(body->end(), data, data + size * count);
    return size * count;
}

}  // namespace

void SetCaBundle(CURL* curl) {
    curl_blob blob{const_cast<unsigned char*>(kCaBundle), kCaBundleSize, CURL_BLOB_NOCOPY};
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
}

HttpResponse HttpRequest(std::string_view url, std::string_view method, std::string_view body,
                         std::string_view content_type) {
    InitCurl();
    thread_local std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
    CURL* curl = handle.get();
    if (!curl) Fail("curl_easy_init failed");
    curl_easy_reset(curl);  // keeps the connection cache, clears the options

    std::string url_string(url);
    std::string method_string(method);
    std::string header;
    curl_slist* headers = nullptr;
    if (!content_type.empty()) {
        header = "Content-Type: " + std::string(content_type);
        headers = curl_slist_append(nullptr, header.c_str());
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers_guard(headers, &curl_slist_free_all);

    HttpResponse response;
    char error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url_string.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    SetCaBundle(curl);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kStallTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (method_string == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else if (method_string != "GET") {
        Fail("HttpRequest: unsupported method " + method_string);
    }

    CURLcode status = curl_easy_perform(curl);
    if (status != CURLE_OK) {
        Fail(method_string + " " + url_string + " failed: " + (error[0] ? error : curl_easy_strerror(status)));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

}  // namespace pxsteamdl::detail
