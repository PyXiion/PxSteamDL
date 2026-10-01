// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cdn.hpp"

#include <string_view>

#include <nlohmann/json.hpp>

namespace pxsteamdl::detail {

namespace {

constexpr char kServersUrl[] =
    "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=0&max_servers=20";

bool IsUsableServer(const nlohmann::json& server) {
  std::string type = server.value("type", "");
  std::string https = server.value("https_support", "");
  return (type == "SteamCache" || type == "CDN") &&
         (https == "mandatory" || https == "preferred" || https == "optional") && server.contains("host") &&
         server.at("host").is_string();
}

// A server without an allowed_app_ids list serves every app.
bool ServesApp(const nlohmann::json& server, std::uint32_t app_id) {
  if (!server.contains("allowed_app_ids")) return true;
  const auto& allowed = server.at("allowed_app_ids");
  if (!allowed.is_array() || allowed.empty()) return true;
  return std::any_of(allowed.begin(), allowed.end(), [app_id](const nlohmann::json& id) {
    return id.is_number_unsigned() && id.get<std::uint32_t>() == app_id;
  });
}

// Host names end up in URLs: accept letters, digits, dots and dashes only.
bool IsSafeHostName(std::string_view host) {
  constexpr std::string_view kAllowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
  return !host.empty() && host.find_first_not_of(kAllowed) == std::string_view::npos;
}

}  // namespace

std::vector<std::string> FetchCdnHosts(std::uint32_t app_id, const std::stop_token& stop) {
  HttpResponse response = Retry(stop, [] {
    HttpResponse reply = HttpRequest(kServersUrl);
    CheckHttpStatus("CDN server list", reply.status);
    return reply;
  });
  try {
    auto root = nlohmann::json::parse(response.body.begin(), response.body.end());
    std::vector<std::string> hosts;
    for (const auto& server : root.at("response").at("servers")) {
      if (!IsUsableServer(server) || !ServesApp(server, app_id)) continue;
      auto host = server.at("host").get<std::string>();
      if (IsSafeHostName(host)) hosts.push_back(std::move(host));
    }
    if (hosts.empty()) Fail("CDN server list: no HTTPS SteamCache/CDN servers");
    return hosts;
  } catch (const nlohmann::json::exception& e) {
    Fail("CDN server list: invalid JSON: " + std::string(e.what()));
  }
}

}  // namespace pxsteamdl::detail
