// SPDX-License-Identifier: LGPL-3.0-or-later
// SteamPipe CDN servers.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "http.hpp"
#include "retry.hpp"

namespace pxsteamdl::detail {

// Returns the HTTPS-capable SteamCache/CDN hosts that may serve app_id; throws if there are none. Retries the request
// like Retry() does.
std::vector<std::string> FetchCdnHosts(std::uint32_t app_id, const HttpConfig& config, const std::stop_token& stop);

// GETs path from the hosts in turn, starting at hosts[host % size] and leaving host at the one that answered.
// decode(Bytes) turns the body into the result and throws to reject it, which moves on to the next host like an HTTP
// error does. After a transient failure (429, 5xx, a network error) it also pauses, with a growing delay, since
// every host may be in the same trouble; other failures (say, 404 on one host) move on at once. Throws kCancelled
// once stop is requested, or after a few failed attempts an Error with the kind of the last failure.
template <class Decode>
auto FetchFromCdn(const std::vector<std::string>& hosts, const std::string& path, std::size_t& host,
                  const HttpConfig& config, const std::stop_token& stop, Decode decode) {
  constexpr std::size_t kMaxCdnAttempts = 6;
  std::string last_error;
  ErrorKind last_kind = ErrorKind::kRejected;
  std::size_t attempts = std::min(hosts.size() * 2, kMaxCdnAttempts);
  for (std::size_t attempt = 0; attempt < attempts; ++attempt, ++host) {
    if (stop.stop_requested()) FailCancelled();
    const std::string& name = hosts[host % hosts.size()];
    bool transient = false;
    try {
      HttpResponse response = HttpRequest("https://" + name + path, config);
      if (response.status == 200) return decode(std::move(response.body));
      last_error = name + ": HTTP " + std::to_string(response.status);
      transient = IsTransientHttpStatus(response.status);
      last_kind = transient ? ErrorKind::kNetwork : ErrorKind::kRejected;
    } catch (const TransientError& e) {
      last_error = name + ": " + e.what();
      last_kind = ErrorKind::kNetwork;
      transient = true;
    } catch (const std::exception& e) {
      last_error = name + ": " + e.what();
      last_kind = KindOf(e);
    }
    if (transient && attempt + 1 < attempts && !SleepFor(BackoffDelay(static_cast<int>(attempt) + 1), stop)) {
      FailCancelled();
    }
  }
  Fail(last_kind, "CDN request failed for " + path + ": " + last_error);
}

}  // namespace pxsteamdl::detail
