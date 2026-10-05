// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fake_steam.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <zlib.h>
#include <nlohmann/json.hpp>

#include "cm_packet.hpp"
#include "crypto.hpp"
#include "depot_builder.hpp"
#include "eresult.hpp"
#include "fake_http.hpp"
#include "fake_session.hpp"
#include "proto.hpp"

namespace pxsteamdl::detail::testing {

namespace {

constexpr std::uint64_t kRequestCode = 777;
constexpr std::string_view kServersMarker = "/IContentServerDirectoryService/GetServersForSteamPipe/v1/";
constexpr std::string_view kLegacyHost = "legacy.test";
constexpr std::string_view kManifestRequestCodeMethod = "ContentServerDirectory.GetManifestRequestCode#1";

HttpResponse Ok(std::string_view body) {
  HttpResponse response;
  response.status = 200;
  response.body.assign(body.begin(), body.end());
  return response;
}

HttpResponse Ok(Bytes body) {
  HttpResponse response;
  response.status = 200;
  response.body = std::move(body);
  return response;
}

HttpResponse StatusOnly(long code) {
  HttpResponse response;
  response.status = code;
  return response;
}

// The IDs of a GetPublishedFileDetails form: publishedfileids%5B0%5D=ID&...
std::vector<std::uint64_t> ParseIds(std::string_view form) {
  std::vector<std::uint64_t> ids;
  constexpr std::string_view kKey = "%5D=";
  for (std::size_t at = form.find(kKey); at != std::string_view::npos; at = form.find(kKey, at)) {
    at += kKey.size();
    std::uint64_t id = 0;
    while (at < form.size() && form[at] >= '0' && form[at] <= '9')
      id = id * 10 + static_cast<std::uint64_t>(form[at++] - '0');
    ids.push_back(id);
  }
  return ids;
}

// Splits "https://host/path" into host and "/path".
void SplitUrl(std::string_view url, std::string& host, std::string& path) {
  constexpr std::string_view kScheme = "https://";
  if (!url.starts_with(kScheme)) Fail("unexpected URL: " + std::string(url));
  url.remove_prefix(kScheme.size());
  std::size_t slash = url.find('/');
  host = std::string(url.substr(0, slash));
  path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
}

std::string LastSegment(std::string_view path) { return std::string(path.substr(path.rfind('/') + 1)); }

// Classifies a request by its URL.
HttpCall Classify(std::string_view url, std::string_view body) {
  HttpCall call;
  call.url = std::string(url);
  if (url == kFakeDetailsUrl) {
    call.kind = RequestKind::kDetails;
    for (std::uint64_t id : ParseIds(body)) call.subject += (call.subject.empty() ? "" : ",") + std::to_string(id);
    return call;
  }
  if (url.find(kServersMarker) != std::string_view::npos) {
    call.kind = RequestKind::kServers;
    return call;
  }
  std::string path;
  SplitUrl(url, call.host, path);
  if (call.host == kLegacyHost) {
    call.kind = RequestKind::kLegacyFile;
    call.subject = path.substr(1, path.find('/', 1) - 1);
  } else if (path.find("/manifest/") != std::string::npos) {
    call.kind = RequestKind::kManifest;
    call.subject = path;
  } else if (path.find("/chunk/") != std::string::npos) {
    call.kind = RequestKind::kChunk;
    call.subject = LastSegment(path);
  } else {
    Fail("unexpected URL: " + std::string(url));
  }
  return call;
}

}  // namespace

FakeSteam::FakeSteam() {
  SetHttpHandler(
      [this](std::string_view url, std::string_view /*method*/, std::string_view body) { return answer(url, body); });
  SessionHandlers handlers;
  handlers.request = [this](std::uint32_t emsg, ByteSpan /*body*/) { return answerRequest(emsg); };
  handlers.rpc = [this](std::string_view method, ByteSpan /*body*/) { return answerRpc(method); };
  SetSessionHandlers(std::move(handlers));
}

FakeSteam::~FakeSteam() {
  ClearHttpHandler();
  ClearSessionHandlers();
}

std::size_t FakeSteam::chunkCount(const FakeItem& item, const std::string& path) {
  for (const FakeFile& file : item.files) {
    if (file.path == path) return (file.content.size() + item.chunk_size - 1) / item.chunk_size;
  }
  Fail("no such file in the item: " + path);
}

void FakeSteam::addItem(const FakeItem& item) {
  Depot depot;
  depot.title = item.title;
  std::vector<ManifestEntry> entries;
  for (const std::string& directory : item.directories) {
    ManifestEntry entry;
    entry.name = directory;
    entry.flags = depot_file_flag::kDirectory;
    entries.push_back(std::move(entry));
  }
  for (const FakeLink& link : item.links) {
    ManifestEntry entry;
    entry.name = link.path;
    entry.flags = depot_file_flag::kSymlink;
    entry.link_target = link.target;
    entries.push_back(std::move(entry));
  }
  for (const FakeFile& file : item.files) {
    ManifestEntry entry;
    entry.name = file.path;
    entry.size = file.content.size();
    entry.flags = file.executable ? depot_file_flag::kExecutable : 0;
    // Steam sends an all-zero hash for empty files.
    if (!file.content.empty()) entry.sha = Sha1(AsBytes(file.content));
    for (std::size_t offset = 0; offset < file.content.size(); offset += item.chunk_size) {
      std::string piece = file.content.substr(offset, item.chunk_size);
      Bytes plain = ToBytes(piece);
      Bytes encrypted = SymmetricEncrypt(MakeZip(plain, true), TestKey());
      ManifestChunk chunk;
      chunk.sha = Sha1(plain);
      chunk.checksum = static_cast<std::uint32_t>(adler32_z(0, plain.data(), plain.size()));
      chunk.offset = offset;
      chunk.original_size = static_cast<std::uint32_t>(plain.size());
      chunk.compressed_size = static_cast<std::uint32_t>(encrypted.size());
      entry.chunks.push_back(chunk);
      depot.chunks[ToHex(chunk.sha)] = std::move(encrypted);
    }
    if (file.manifest_sha) entry.sha = *file.manifest_sha;
    entries.push_back(std::move(entry));
  }

  std::lock_guard lock(m_mutex);
  depot.manifest_id = m_nextManifestId++;
  depot.manifest = BuildManifest(entries, kFakeAppId, depot.manifest_id);
  m_depots[item.id] = std::move(depot);
}

void FakeSteam::addLegacyItem(const FakeLegacyItem& item) {
  std::lock_guard lock(m_mutex);
  m_legacy[item.id] = item;
}

void FakeSteam::setHttpHook(std::function<std::optional<HttpResponse>(const HttpCall&)> hook) {
  std::lock_guard lock(m_mutex);
  m_httpHook = std::move(hook);
}

void FakeSteam::setSessionHook(std::function<void(SessionCall)> hook) {
  std::lock_guard lock(m_mutex);
  m_sessionHook = std::move(hook);
}

std::vector<HttpCall> FakeSteam::calls() const {
  std::lock_guard lock(m_mutex);
  return m_calls;
}

int FakeSteam::callCount(RequestKind kind) const {
  std::lock_guard lock(m_mutex);
  return static_cast<int>(
      std::count_if(m_calls.begin(), m_calls.end(), [kind](const HttpCall& call) { return call.kind == kind; }));
}

int FakeSteam::callsToHost(const std::string& host, RequestKind kind) const {
  std::lock_guard lock(m_mutex);
  return static_cast<int>(std::count_if(m_calls.begin(), m_calls.end(),
                                        [&](const HttpCall& call) { return call.kind == kind && call.host == host; }));
}

HttpResponse FakeSteam::answer(std::string_view url, std::string_view body) {
  HttpCall call = Classify(url, body);
  std::function<std::optional<HttpResponse>(const HttpCall&)> hook;
  {
    std::lock_guard lock(m_mutex);
    m_calls.push_back(call);
    hook = m_httpHook;
  }
  if (hook) {
    if (std::optional<HttpResponse> replacement = hook(call)) return *replacement;
  }
  return respond(call, body);
}

HttpResponse FakeSteam::respond(const HttpCall& call, std::string_view body) const {
  std::lock_guard lock(m_mutex);
  switch (call.kind) {
    case RequestKind::kDetails: {
      nlohmann::json details = nlohmann::json::array();
      for (std::uint64_t id : ParseIds(body)) {
        nlohmann::json entry = {{"publishedfileid", std::to_string(id)}, {"result", 9}};
        if (auto depot = m_depots.find(id); depot != m_depots.end()) {
          entry = {{"publishedfileid", std::to_string(id)},
                   {"result", 1},
                   {"hcontent_file", std::to_string(depot->second.manifest_id)},
                   {"consumer_app_id", kFakeAppId},
                   {"title", depot->second.title}};
        } else if (auto legacy = m_legacy.find(id); legacy != m_legacy.end()) {
          entry = {{"publishedfileid", std::to_string(id)},
                   {"result", 1},
                   {"hcontent_file", "0"},
                   {"consumer_app_id", kFakeAppId},
                   {"title", legacy->second.title},
                   {"file_url",
                    "https://" + std::string(kLegacyHost) + "/" + std::to_string(id) + "/" + legacy->second.filename},
                   {"filename", legacy->second.filename}};
        }
        details.push_back(std::move(entry));
      }
      nlohmann::json root = {
          {"response", {{"result", 1}, {"resultcount", details.size()}, {"publishedfiledetails", details}}}};
      return Ok(root.dump());
    }
    case RequestKind::kServers: {
      nlohmann::json servers = nlohmann::json::array();
      for (const char* host : {kFakeHostA, kFakeHostB}) {
        servers.push_back({{"type", "CDN"}, {"https_support", "mandatory"}, {"host", host}});
      }
      return Ok(nlohmann::json{{"response", {{"servers", servers}}}}.dump());
    }
    case RequestKind::kManifest: {
      if (call.host != kFakeHostA && call.host != kFakeHostB) return StatusOnly(404);
      // /depot/<app>/manifest/<manifest>/<version>/<request code>
      std::string path = call.subject;
      if (LastSegment(path) != std::to_string(kRequestCode)) return StatusOnly(403);
      for (const auto& [id, depot] : m_depots) {
        if (path.find("/manifest/" + std::to_string(depot.manifest_id) + "/") != std::string::npos) {
          return Ok(Bytes(depot.manifest));
        }
      }
      return StatusOnly(404);
    }
    case RequestKind::kChunk: {
      if (call.host != kFakeHostA && call.host != kFakeHostB) return StatusOnly(404);
      for (const auto& [id, depot] : m_depots) {
        if (auto chunk = depot.chunks.find(call.subject); chunk != depot.chunks.end()) return Ok(Bytes(chunk->second));
      }
      return StatusOnly(404);
    }
    case RequestKind::kLegacyFile: {
      auto legacy = m_legacy.find(std::stoull(call.subject));
      return legacy == m_legacy.end() ? StatusOnly(404) : Ok(std::string_view(legacy->second.content));
    }
  }
  return StatusOnly(500);
}

Bytes FakeSteam::answerRequest(std::uint32_t emsg) {
  if (emsg != emsg::kClientGetDepotDecryptionKey) Fail("unexpected EMsg " + std::to_string(emsg));
  std::function<void(SessionCall)> hook;
  {
    std::lock_guard lock(m_mutex);
    hook = m_sessionHook;
  }
  ++m_sessionCalls[static_cast<int>(SessionCall::kDepotKey)];
  if (hook) hook(SessionCall::kDepotKey);
  std::int64_t eresult = m_depotKeyEResult;
  if (eresult != kEResultOk) return EncodeUint(1, static_cast<std::uint64_t>(eresult));
  AesKey key = TestKey();
  return Concat({EncodeUint(1, kEResultOk), EncodeBytes(3, key)});
}

Bytes FakeSteam::answerRpc(std::string_view method) {
  if (method != kManifestRequestCodeMethod) Fail("unexpected RPC " + std::string(method));
  std::function<void(SessionCall)> hook;
  {
    std::lock_guard lock(m_mutex);
    hook = m_sessionHook;
  }
  ++m_sessionCalls[static_cast<int>(SessionCall::kManifestRequestCode)];
  if (hook) hook(SessionCall::kManifestRequestCode);
  return EncodeUint(1, kRequestCode);
}

}  // namespace pxsteamdl::detail::testing
