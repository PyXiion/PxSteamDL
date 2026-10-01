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

namespace pxsteamdl::detail {

// Returns the HTTPS-capable SteamCache/CDN hosts that may serve app_id; throws if there are none.
std::vector<std::string> FetchCdnHosts(std::uint32_t app_id);

// GETs path from the hosts in turn, starting at hosts[host % size] and leaving host at the one that answered.
// decode(Bytes) turns the body into the result and throws to reject it, which moves on to the next host like an HTTP
// error does. Throws kCancelled once stop is requested, or the last error after a few failed attempts.
template <class Decode>
auto FetchFromCdn(const std::vector<std::string>& hosts, const std::string& path, std::size_t& host,
                  const std::stop_token& stop, Decode decode) {
  constexpr std::size_t kMaxAttempts = 6;
  std::string last_error;
  std::size_t attempts = std::min(hosts.size() * 2, kMaxAttempts);
  for (std::size_t attempt = 0; attempt < attempts; ++attempt, ++host) {
    if (stop.stop_requested()) Fail(kCancelled);
    const std::string& name = hosts[host % hosts.size()];
    try {
      HttpResponse response = HttpRequest("https://" + name + path);
      if (response.status == 200) return decode(std::move(response.body));
      last_error = name + ": HTTP " + std::to_string(response.status);
    } catch (const std::exception& e) {
      last_error = name + ": " + e.what();
    }
  }
  Fail("CDN request failed for " + path + ": " + last_error);
}

}  // namespace pxsteamdl::detail
