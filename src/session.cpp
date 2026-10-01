// SPDX-License-Identifier: LGPL-3.0-or-later
#include "session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>

#include "cm_packet.hpp"
#include "eresult.hpp"
#include "http.hpp"
#include "proto.hpp"
#include "retry.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace pxsteamdl::detail {

namespace {

constexpr char kCmListUrl[] = "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0";
// CM endpoints tried before giving up.
constexpr std::size_t kMaxCmAttempts = 5;
constexpr std::uint64_t kAnonymousSteamId = 0x01A0000000000000ULL;

constexpr long kConnectTimeoutSeconds = 10;
constexpr auto kLogonTimeout = std::chrono::seconds(10);
constexpr auto kCallTimeout = std::chrono::seconds(30);
constexpr int kSendPollTimeoutMs = 5000;
// How long the reader waits for data before it checks whether to stop.
constexpr int kReadPollTimeoutMs = 50;
constexpr int kDefaultHeartbeatSeconds = 9;
constexpr int kMinHeartbeatSeconds = 1;
constexpr int kMaxHeartbeatSeconds = 60;

// CMsgClientLogon field numbers and the values sent for an anonymous logon.
namespace logon_field {
constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kCellId = 3;
constexpr std::uint32_t kClientLanguage = 6;
constexpr std::uint32_t kClientOsType = 7;
}  // namespace logon_field
constexpr std::uint32_t kProtocolVersion = 65581;
// EOSType Linux 6.x (-184) as an unsigned 32-bit value.
constexpr std::uint32_t kOsTypeLinux6x = 4294967112u;

// CMsgClientLogonResponse field numbers.
namespace logon_response_field {
constexpr std::uint32_t kEResult = 1;
constexpr std::uint32_t kHeartbeatSeconds = 3;
}  // namespace logon_response_field

// Waits up to timeout_ms for the socket to become readable (or writable); returns false on timeout or error.
bool WaitSocket(curl_socket_t socket, bool write, int timeout_ms) {
#ifdef _WIN32
  WSAPOLLFD entry{socket, static_cast<SHORT>(write ? POLLOUT : POLLIN), 0};
  return WSAPoll(&entry, 1, timeout_ms) > 0;
#else
  pollfd entry{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
  return poll(&entry, 1, timeout_ms) > 0;
#endif
}

curl_socket_t ActiveSocket(CURL* curl) {
  curl_socket_t socket = CURL_SOCKET_BAD;
  curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
  return socket;
}

enum class ReceiveStatus {
  kMessage,  // a complete message was moved into message
  kMore,     // read again right away
  kAgain,    // nothing buffered: wait for the socket
  kClosed,
};

// One non-blocking read from a CONNECT_ONLY WebSocket. Data frames are reassembled in partial; a completed message
// is moved into message. Control frames (curl answers pings itself) are skipped.
ReceiveStatus Receive(CURL* curl, Bytes& partial, Bytes& message) {
  std::uint8_t buffer[65536];
  std::size_t read = 0;
  const curl_ws_frame* meta = nullptr;
  CURLcode status = curl_ws_recv(curl, buffer, sizeof buffer, &read, &meta);
  if (status == CURLE_AGAIN) return ReceiveStatus::kAgain;
  if (status != CURLE_OK || !meta || (meta->flags & CURLWS_CLOSE)) return ReceiveStatus::kClosed;
  if (!(meta->flags & (CURLWS_BINARY | CURLWS_TEXT))) return ReceiveStatus::kMore;
  partial.insert(partial.end(), buffer, buffer + read);
  if (meta->bytesleft != 0 || (meta->flags & CURLWS_CONT)) return ReceiveStatus::kMore;
  message = std::move(partial);
  partial.clear();
  return ReceiveStatus::kMessage;
}

std::vector<std::string> FetchCmEndpoints() {
  HttpResponse response = Retry({}, [] {
    HttpResponse reply = HttpRequest(kCmListUrl);
    CheckHttpStatus("GetCMListForConnect", reply.status);
    return reply;
  });
  auto root = nlohmann::json::parse(response.body.begin(), response.body.end());
  const auto& servers = root.at("response").at("serverlist");
  if (!servers.is_array()) Fail("GetCMListForConnect: invalid serverlist");

  std::vector<std::string> endpoints;
  for (const auto& server : servers) {
    if (server.value("type", "") != "websockets" || !server.contains("endpoint") || !server["endpoint"].is_string()) {
      continue;
    }
    auto endpoint = server["endpoint"].get<std::string>();
    if (!endpoint.empty()) endpoints.push_back(std::move(endpoint));
  }
  if (endpoints.empty()) Fail("No websocket CM endpoints found");
  return endpoints;
}

Bytes LogonBody() {
  return Concat({
      EncodeUint(logon_field::kProtocolVersion, kProtocolVersion),
      EncodeUint(logon_field::kCellId, 0),
      EncodeString(logon_field::kClientLanguage, "english"),
      EncodeUint(logon_field::kClientOsType, kOsTypeLinux6x),
  });
}

struct LogonResult {
  std::int64_t eresult = 0;
  int heartbeat_seconds = kDefaultHeartbeatSeconds;
};

LogonResult ParseLogonResponse(const Packet& packet) {
  LogonResult result;
  result.eresult = packet.eresult;
  ProtoReader reader(packet.body);
  while (auto field = reader.next()) {
    if (field->is(logon_response_field::kEResult, WireType::kVarint)) {
      result.eresult = static_cast<int>(field->integer);
    } else if (field->is(logon_response_field::kHeartbeatSeconds, WireType::kVarint)) {
      result.heartbeat_seconds = static_cast<int>(field->integer);
    }
  }
  return result;
}

}  // namespace

class Session::Impl {
 public:
  ~Impl() {
    std::lock_guard lock(m_connectMutex);
    close();
  }

  void connect() {
    std::lock_guard lock(m_connectMutex);
    if (m_connected) return;
    close();  // joins the threads of a dropped connection
    open();
  }

  // Sends a packet as a new job and waits for the packet that answers it.
  Packet call(std::uint32_t emsg, std::string_view job_name, ByteSpan body) {
    std::uint64_t jobid = m_nextJobId++;
    std::future<Packet> future;
    {
      // Checked under m_jobsMutex so the job cannot be registered after the reader failed the pending ones.
      std::lock_guard lock(m_jobsMutex);
      if (!m_connected) FailTransient("Session not connected");
      future = m_pendingJobs[jobid].get_future();
    }
    auto forget = [&] {
      std::lock_guard lock(m_jobsMutex);
      m_pendingJobs.erase(jobid);
    };
    try {
      Packet request = sessionPacket(emsg);
      request.jobid_source = jobid;
      request.target_job_name = job_name;
      request.body.assign(body.begin(), body.end());
      sendPacket(SerializePacket(request));
    } catch (...) {
      forget();
      throw;
    }
    if (future.wait_for(kCallTimeout) == std::future_status::timeout) {
      forget();
      FailTransient("Steam CM request timed out (EMsg " + std::to_string(emsg) + " " + std::string(job_name) + ")");
    }
    return future.get();
  }

 private:
  // A packet outside any job.
  Packet sessionPacket(std::uint32_t emsg) const {
    Packet packet;
    packet.emsg = emsg;
    packet.steamid = m_steamid;
    packet.sessionid = m_sessionid;
    return packet;
  }

  // Tries the CM endpoints in turn. Requires m_connectMutex and a closed session.
  void open() {
    std::vector<std::string> endpoints = FetchCmEndpoints();
    std::string last_error = "no endpoint reachable";
    for (std::size_t i = 0; i < std::min(endpoints.size(), kMaxCmAttempts); ++i) {
      try {
        if (logon("wss://" + endpoints[i] + "/cmsocket/")) return;
        last_error = "no logon response";
      } catch (const std::exception& e) {
        last_error = e.what();
      }
      std::lock_guard lock(m_ioMutex);
      if (m_curl) curl_easy_cleanup(m_curl);
      m_curl = nullptr;
    }
    FailTransient("Failed to connect to Steam CM: " + last_error);
  }

  // Logs off (if still connected), stops the threads and releases the socket. Requires m_connectMutex.
  void close() {
    bool was_connected = markDisconnected("Session disconnected");
    stopThreads();
    if (was_connected) {
      try {
        sendPacket(SerializePacket(sessionPacket(emsg::kClientLogOff)));
      } catch (...) {
        // The connection is going away anyway.
      }
    }
    if (m_heartbeatThread.joinable()) m_heartbeatThread.join();
    if (m_readerThread.joinable()) m_readerThread.join();

    std::lock_guard lock(m_ioMutex);
    if (m_curl) curl_easy_cleanup(m_curl);
    m_curl = nullptr;
  }

  void stopThreads() {
    {
      std::lock_guard lock(m_heartbeatMutex);  // no lost wakeup between the heartbeat's check and wait
      m_stopping = true;
    }
    m_heartbeatCv.notify_all();
  }

  // Stops accepting calls and fails the ones in flight; returns whether the session was connected.
  bool markDisconnected(const char* reason) {
    std::lock_guard lock(m_jobsMutex);
    bool was_connected = m_connected.exchange(false);
    for (auto& [id, promise] : m_pendingJobs) {
      promise.set_exception(std::make_exception_ptr(TransientError(reason)));
    }
    m_pendingJobs.clear();
    return was_connected;
  }

  // Opens the socket and logs on anonymously; on success starts the reader and heartbeat threads.
  // Returns false on a connection failure or timeout, throws on a rejected logon.
  bool logon(const std::string& url) {
    CURL* handle = curl_easy_init();
    if (!handle) Fail("curl_easy_init failed");
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    SetCaBundle(handle);
    curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);  // WebSocket
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    {
      std::lock_guard lock(m_ioMutex);
      m_curl = handle;  // owned (and cleaned up on failure) by open()
    }
    if (curl_easy_perform(handle) != CURLE_OK) return false;

    Packet logon;
    logon.emsg = emsg::kClientLogon;
    logon.steamid = kAnonymousSteamId;
    logon.body = LogonBody();
    sendPacket(SerializePacket(logon));
    std::optional<Packet> response = awaitLogonResponse(handle);
    if (!response) return false;
    LogonResult result = ParseLogonResponse(*response);
    if (result.eresult != kEResultOk) FailEResult("Logon failed", result.eresult);

    m_steamid = response->steamid;
    m_sessionid = response->sessionid;
    m_stopping = false;
    m_connected = true;
    m_readerThread = std::thread(&Impl::readerLoop, this);
    m_heartbeatThread = std::thread(&Impl::heartbeatLoop, this,
                                    std::clamp(result.heartbeat_seconds, kMinHeartbeatSeconds, kMaxHeartbeatSeconds));
    return true;
  }

  // Reads until the logon response arrives; nullopt if the socket closes or the logon times out. No other thread
  // uses the handle yet, so it is read synchronously.
  static std::optional<Packet> awaitLogonResponse(CURL* handle) {
    Bytes partial;
    Bytes message;
    auto deadline = std::chrono::steady_clock::now() + kLogonTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      ReceiveStatus status = Receive(handle, partial, message);
      if (status == ReceiveStatus::kClosed) return std::nullopt;
      if (status == ReceiveStatus::kMore) continue;
      if (status == ReceiveStatus::kAgain) {
        WaitSocket(ActiveSocket(handle), false, kReadPollTimeoutMs);
        continue;
      }
      for (Packet& packet : ParseMessage(message)) {
        if (packet.emsg == emsg::kClientLogonResponse) return std::move(packet);
      }
    }
    return std::nullopt;
  }

  void sendPacket(ByteSpan data) {
    std::lock_guard lock(m_ioMutex);
    if (!m_curl) FailTransient("Session not connected");
    curl_socket_t socket = ActiveSocket(m_curl);
    for (std::size_t offset = 0; offset < data.size();) {
      std::size_t sent = 0;
      CURLcode status = curl_ws_send(m_curl, data.data() + offset, data.size() - offset, &sent, 0, CURLWS_BINARY);
      if (status == CURLE_AGAIN) {
        if (socket == CURL_SOCKET_BAD) FailTransient("curl socket unavailable for send");
        if (!WaitSocket(socket, true, kSendPollTimeoutMs)) FailTransient("curl_ws_send poll timeout");
        continue;
      }
      if (status != CURLE_OK) FailTransient(std::string("curl_ws_send failed: ") + curl_easy_strerror(status));
      offset += sent;
    }
  }

  // Hands each packet of a message to the call waiting for it.
  void dispatch(ByteSpan message) {
    std::vector<Packet> packets;
    try {
      packets = ParseMessage(message);
    } catch (const std::exception&) {
      return;  // malformed message: nothing to route
    }
    for (Packet& packet : packets) {
      if (packet.emsg == emsg::kClientLoggedOff) stopThreads();
      if (packet.jobid_target == kNoJob) continue;
      std::promise<Packet> promise;
      {
        std::lock_guard lock(m_jobsMutex);
        auto it = m_pendingJobs.find(packet.jobid_target);
        if (it == m_pendingJobs.end()) continue;
        promise = std::move(it->second);
        m_pendingJobs.erase(it);
      }
      promise.set_value(std::move(packet));
    }
  }

  void readerLoop() {
    Bytes partial;
    Bytes message;
    while (!m_stopping) {
      ReceiveStatus status;
      curl_socket_t socket;
      {
        std::lock_guard lock(m_ioMutex);
        if (!m_curl) break;
        status = Receive(m_curl, partial, message);
        socket = ActiveSocket(m_curl);
      }
      if (status == ReceiveStatus::kClosed) break;
      if (status == ReceiveStatus::kMessage) {
        dispatch(message);
      } else if (status == ReceiveStatus::kMore) {
        continue;
      } else if (socket == CURL_SOCKET_BAD) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        WaitSocket(socket, false, kReadPollTimeoutMs);  // without m_ioMutex, so senders can proceed
      }
    }
    markDisconnected("Connection closed");
  }

  void heartbeatLoop(int interval_seconds) {
    std::unique_lock lock(m_heartbeatMutex);
    while (!m_heartbeatCv.wait_for(lock, std::chrono::seconds(interval_seconds), [&] { return m_stopping.load(); })) {
      if (!m_connected) break;
      try {
        sendPacket(SerializePacket(sessionPacket(emsg::kClientHeartbeat)));
      } catch (...) {
        break;  // the reader notices the broken connection
      }
    }
  }

  std::mutex m_connectMutex;  // serializes open() and close()

  std::mutex m_ioMutex;  // guards m_curl
  CURL* m_curl = nullptr;
  // Assigned by the logon; atomic because a call racing a reconnect may still read them.
  std::atomic<std::uint64_t> m_steamid{kAnonymousSteamId};
  std::atomic<std::int32_t> m_sessionid{0};
  std::atomic<std::uint64_t> m_nextJobId{1};

  std::mutex m_jobsMutex;  // guards m_pendingJobs, and m_connected transitions to false
  std::unordered_map<std::uint64_t, std::promise<Packet>> m_pendingJobs;
  std::atomic<bool> m_connected{false};

  std::atomic<bool> m_stopping{false};
  std::thread m_readerThread;
  std::thread m_heartbeatThread;
  std::mutex m_heartbeatMutex;
  std::condition_variable m_heartbeatCv;
};

Session::Session() : m_impl(std::make_unique<Impl>()) {}

Session::~Session() = default;

void Session::connect() { m_impl->connect(); }

Bytes Session::request(std::uint32_t emsg, ByteSpan body) {
  connect();
  return m_impl->call(emsg, "", body).body;
}

Bytes Session::rpc(std::string_view method, ByteSpan body) {
  connect();
  Packet response = m_impl->call(emsg::kServiceMethodCall, method, body);
  if (response.eresult != kEResultOk) {
    FailEResult("RPC " + std::string(method) + " failed", response.eresult, response.error_message);
  }
  return std::move(response.body);
}

}  // namespace pxsteamdl::detail
