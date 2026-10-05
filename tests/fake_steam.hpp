// SPDX-License-Identifier: LGPL-3.0-or-later
// A pretend Steam for end-to-end tests of Client: Web API, CDN and CM session answers built from items the test
// declares. It works through the fakes that replace src/http.cpp and src/session.cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "crypto.hpp"
#include "http.hpp"

namespace pxsteamdl::detail::testing {

inline constexpr std::uint32_t kFakeAppId = 294100;
inline constexpr char kFakeDetailsUrl[] =
    "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
inline constexpr char kFakeHostA[] = "cdn1.test";
inline constexpr char kFakeHostB[] = "cdn2.test";

struct FakeFile {
  std::string path;
  std::string content;
  bool executable = false;
  // Overrides the whole-file hash without changing the correct chunk hashes/checksums.
  std::optional<Sha1Hash> manifest_sha = std::nullopt;
};

struct FakeLink {
  std::string path;
  std::string target;
};

// A workshop item served as a SteamPipe depot.
struct FakeItem {
  std::uint64_t id = 0;
  std::string title;
  std::vector<FakeFile> files;
  std::vector<FakeLink> links;
  std::vector<std::string> directories;
  // Files are cut into chunks of this size, so a small file can still have several.
  std::size_t chunk_size = 64;
};

// An item from before SteamPipe: a single file at a direct URL.
struct FakeLegacyItem {
  std::uint64_t id = 0;
  std::string title;
  std::string filename;
  std::string content;
};

enum class RequestKind { kDetails, kServers, kManifest, kChunk, kLegacyFile };

struct HttpCall {
  RequestKind kind = RequestKind::kDetails;
  std::string url;
  std::string host;
  // The chunk's SHA-1 in hex, for kChunk; the item IDs asked for, comma-separated, for kDetails.
  std::string subject;
};

enum class SessionCall { kDepotKey, kManifestRequestCode };

// Installs itself as the HTTP and session handlers on construction and removes itself on destruction.
class FakeSteam {
 public:
  FakeSteam();
  ~FakeSteam();
  FakeSteam(const FakeSteam&) = delete;
  FakeSteam& operator=(const FakeSteam&) = delete;

  // Adds the item, or replaces it with new content under a new manifest ID, as if the author had updated it.
  void addItem(const FakeItem& item);
  void addLegacyItem(const FakeLegacyItem& item);

  // Called before each HTTP request is answered. A returned response is sent instead of the real answer; throwing
  // fails the request like a transport error. Set it before the download starts.
  void setHttpHook(std::function<std::optional<HttpResponse>(const HttpCall&)> hook);
  // Called before each session call is answered; throw to fail it. Set it before the download starts.
  void setSessionHook(std::function<void(SessionCall)> hook);
  // The EResult of depot key replies; the key is sent only if it is 1.
  void setDepotKeyEResult(std::int64_t eresult) { m_depotKeyEResult = eresult; }

  // The HTTP requests answered or failed so far, in order of arrival.
  std::vector<HttpCall> calls() const;
  int callCount(RequestKind kind) const;
  int callsToHost(const std::string& host, RequestKind kind) const;
  int sessionCallCount(SessionCall call) const { return m_sessionCalls[static_cast<int>(call)]; }

  // How many chunks the file of the item is cut into.
  static std::size_t chunkCount(const FakeItem& item, const std::string& path);

 private:
  struct Depot {
    std::uint64_t manifest_id = 0;
    Bytes manifest;
    // Encrypted chunks by SHA-1 in hex.
    std::map<std::string, Bytes> chunks;
    std::string title;
  };

  HttpResponse answer(std::string_view url, std::string_view body);
  // The normal answer to a call, which the hook did not replace.
  HttpResponse respond(const HttpCall& call, std::string_view body) const;
  Bytes answerRequest(std::uint32_t emsg);
  Bytes answerRpc(std::string_view method);

  mutable std::mutex m_mutex;  // guards everything below but the atomics
  std::map<std::uint64_t, Depot> m_depots;
  std::map<std::uint64_t, FakeLegacyItem> m_legacy;
  std::uint64_t m_nextManifestId = 1000;
  std::vector<HttpCall> m_calls;
  std::function<std::optional<HttpResponse>(const HttpCall&)> m_httpHook;
  std::function<void(SessionCall)> m_sessionHook;
  std::atomic<std::int64_t> m_depotKeyEResult{1};
  std::atomic<int> m_sessionCalls[2] = {0, 0};
};

}  // namespace pxsteamdl::detail::testing
