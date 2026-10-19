// SPDX-License-Identifier: LGPL-3.0-or-later
#include "session.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>

#include "async.hpp"
#include "cm_packet.hpp"
#include "eresult.hpp"
#include "http.hpp"
#include "net.hpp"
#include "proto.hpp"
#include "retry.hpp"

namespace pxsteamdl::detail {

namespace {

constexpr char kCmListUrl[] = "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0";
// CM endpoints tried before giving up.
constexpr std::size_t kMaxCmAttempts = 5;
constexpr std::uint64_t kAnonymousSteamId = 0x01A0000000000000ULL;

constexpr auto kLogonTimeout = std::chrono::seconds(10);
constexpr auto kCallTimeout = std::chrono::seconds(30);
constexpr int kDefaultHeartbeatSeconds = 9;
constexpr int kMinHeartbeatSeconds = 1;
constexpr int kMaxHeartbeatSeconds = 60;
constexpr std::size_t kMaxMessageBytes = std::size_t{64} << 20;

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

// ---- WebSocket (RFC 6455), just what the CM protocol needs: binary messages, ping/pong, close ----

namespace ws {

constexpr std::uint8_t kContinuation = 0x0;
constexpr std::uint8_t kText = 0x1;
constexpr std::uint8_t kBinary = 0x2;
constexpr std::uint8_t kClose = 0x8;
constexpr std::uint8_t kPing = 0x9;
constexpr std::uint8_t kPong = 0xA;
constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string Base64(const std::uint8_t* data, std::size_t size) {
  std::string out(4 * ((size + 2) / 3), '\0');
  int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
  out.resize(static_cast<std::size_t>(length));
  return out;
}

std::mt19937& Random() {
  thread_local std::mt19937 random(std::random_device{}());
  return random;
}

// A masked client frame.
Bytes EncodeFrame(std::uint8_t opcode, ByteSpan payload) {
  Bytes frame;
  frame.reserve(payload.size() + 14);
  frame.push_back(static_cast<std::uint8_t>(0x80 | opcode));
  if (payload.size() < 126) {
    frame.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
  } else {
    frame.push_back(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame.push_back(static_cast<std::uint8_t>(std::uint64_t{payload.size()} >> shift));
  }
  std::array<std::uint8_t, 4> mask;
  std::uint32_t random = Random()();
  for (std::size_t i = 0; i < 4; ++i) mask[i] = static_cast<std::uint8_t>(random >> (8 * i));
  frame.insert(frame.end(), mask.begin(), mask.end());
  for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
  return frame;
}

struct Frame {
  std::uint8_t opcode = 0;
  bool fin = false;
  Bytes payload;
};

asio::awaitable<Frame> ReadFrame(Connection& connection) {
  std::uint8_t head[2];
  co_await connection.readExact(head, 2);
  Frame frame;
  frame.fin = head[0] & 0x80;
  frame.opcode = head[0] & 0x0F;
  bool masked = head[1] & 0x80;
  std::uint64_t length = head[1] & 0x7F;
  if (length == 126) {
    std::uint8_t extra[2];
    co_await connection.readExact(extra, 2);
    length = (std::uint64_t{extra[0]} << 8) | extra[1];
  } else if (length == 127) {
    std::uint8_t extra[8];
    co_await connection.readExact(extra, 8);
    length = 0;
    for (std::uint8_t byte : extra) length = (length << 8) | byte;
  }
  if (length > kMaxMessageBytes) FailTransient("WebSocket frame too large");
  std::array<std::uint8_t, 4> mask{};
  if (masked) co_await connection.readExact(mask.data(), 4);
  frame.payload.resize(length);
  co_await connection.readExact(frame.payload.data(), frame.payload.size());
  if (masked) {
    for (std::size_t i = 0; i < frame.payload.size(); ++i) frame.payload[i] ^= mask[i % 4];
  }
  co_return frame;
}

// Upgrades an open TLS connection.
asio::awaitable<void> Handshake(Connection& connection, const Url& url, const std::string& path) {
  std::array<std::uint8_t, 16> nonce;
  for (std::size_t i = 0; i < nonce.size(); i += 4) {
    std::uint32_t random = Random()();
    for (std::size_t j = 0; j < 4; ++j) nonce[i + j] = static_cast<std::uint8_t>(random >> (8 * j));
  }
  std::string key = Base64(nonce.data(), nonce.size());
  std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + url.host + ":" + url.port +
                        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                        "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  co_await connection.writeAll(reinterpret_cast<const std::uint8_t*>(request.data()), request.size());
  std::string head = co_await connection.readUntil("\r\n\r\n", 16 * 1024);
  if (!head.starts_with("HTTP/1.1 101"))
    FailTransient("WebSocket upgrade refused: " + head.substr(0, head.find("\r\n")));
  std::string expected_input = key + std::string(kGuid);
  std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest;
  SHA1(reinterpret_cast<const unsigned char*>(expected_input.data()), expected_input.size(), digest.data());
  std::string expected = Base64(digest.data(), digest.size());
  std::string lower = head;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::size_t at = lower.find("sec-websocket-accept:");
  if (at == std::string::npos) FailTransient("WebSocket upgrade lacks Sec-WebSocket-Accept");
  std::size_t begin = at + 21;
  while (begin < head.size() && head[begin] == ' ') ++begin;
  if (head.compare(begin, expected.size(), expected) != 0) FailTransient("WebSocket accept key mismatch");
}

}  // namespace ws

std::string EndpointUrl(const std::string& endpoint) { return "https://" + endpoint + "/cmsocket/"; }

asio::awaitable<std::vector<std::string>> FetchCmEndpoints(HttpConfig config) {
  HttpResponse response = co_await RetryAsync({}, [&]() -> asio::awaitable<HttpResponse> {
    HttpResponse reply = co_await HttpRequestAsync(kCmListUrl, config);
    CheckHttpStatus("GetCMListForConnect", reply.status);
    co_return reply;
  });
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(response.body.begin(), response.body.end());
  } catch (const nlohmann::json::exception& e) {
    Fail("GetCMListForConnect: invalid JSON: " + std::string(e.what()));
  }
  const nlohmann::json* found = nullptr;
  if (auto reply = root.find("response"); reply != root.end() && reply->is_object()) {
    if (auto list = reply->find("serverlist"); list != reply->end()) found = &*list;
  }
  if (!found || !found->is_array()) Fail("GetCMListForConnect: invalid serverlist");
  const nlohmann::json& servers = *found;

  std::vector<std::string> endpoints;
  for (const auto& server : servers) {
    if (server.value("type", "") != "websockets" || !server.contains("endpoint") || !server["endpoint"].is_string()) {
      continue;
    }
    auto endpoint = server["endpoint"].get<std::string>();
    if (!endpoint.empty()) endpoints.push_back(std::move(endpoint));
  }
  if (endpoints.empty()) Fail("No websocket CM endpoints found");
  co_return endpoints;
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

// A call waiting for the packet that answers it.
struct Job {
  explicit Job(const asio::any_io_executor& executor) : done(executor) {}
  asio::steady_timer done;  // cancelled when reply or error is set
  std::optional<Packet> reply;
  std::exception_ptr error;
};

}  // namespace

// All state is touched on the io thread only, by the caller's coroutines and by three background coroutines per
// connection (reader, writer, heartbeat). They stop when the connection's generation is no longer the current one.
class Session::Impl {
 public:
  explicit Impl(HttpConfig config)
      : m_config(std::move(config)),
        m_net(m_config.net),
        m_executor(m_net->io().get_executor()),
        m_connectSignal(m_executor),
        m_idleSignal(m_executor),
        m_outboxSignal(m_executor),
        m_heartbeat(m_executor) {}

  const HttpConfig& httpConfig() const { return m_config; }
  asio::any_io_executor executor() const { return m_executor; }

  asio::awaitable<void> connect() {
    while (!m_connected) {
      if (m_connecting) {
        co_await m_connectSignal.wait();
        continue;
      }
      m_connecting = true;
      std::exception_ptr error;
      try {
        co_await shutdown();  // joins the tasks of a dropped connection
        co_await open();
      } catch (...) {
        error = std::current_exception();
      }
      m_connecting = false;
      m_connectSignal.notify();
      if (error) std::rethrow_exception(error);
    }
  }

  // Sends a packet as a new job and waits for the packet that answers it.
  asio::awaitable<Packet> call(std::uint32_t emsg, std::string job_name, Bytes body) {
    if (!m_connected) FailTransient("Session not connected");
    std::uint64_t jobid = m_nextJobId++;
    auto job = std::make_shared<Job>(m_executor);
    m_jobs[jobid] = job;
    Packet request = sessionPacket(emsg);
    request.jobid_source = jobid;
    request.target_job_name = job_name;
    request.body = std::move(body);
    enqueue(SerializePacket(request));
    job->done.expires_after(kCallTimeout);
    std::error_code woken;
    co_await job->done.async_wait(asio::redirect_error(asio::use_awaitable, woken));
    if (job->error) std::rethrow_exception(job->error);
    if (job->reply) co_return std::move(*job->reply);
    m_jobs.erase(jobid);
    FailTransient("Steam CM request timed out (EMsg " + std::to_string(emsg) + " " + job_name + ")");
  }

  // Logs off, closes the socket and waits until the background coroutines are gone.
  asio::awaitable<void> shutdown() {
    if (m_connected) {
      enqueue(SerializePacket(sessionPacket(emsg::kClientLogOff)));
      // Give the writer a moment to put the logoff on the wire.
      asio::steady_timer pause(m_executor);
      for (int i = 0; i < 200 && !m_outbox.empty() && m_connected; ++i) {
        pause.expires_after(std::chrono::milliseconds(5));
        std::error_code ignored;
        co_await pause.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
      }
    }
    markDisconnected("Session disconnected");
    if (m_conn) m_conn->close();
    while (m_active > 0) co_await m_idleSignal.wait();
    m_conn.reset();
    m_outbox.clear();
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

  // Tries the CM endpoints in turn. Requires m_connecting and no connection.
  asio::awaitable<void> open() {
    std::vector<std::string> endpoints = co_await FetchCmEndpoints(m_config);
    std::string last_error = "no endpoint reachable";
    for (std::size_t i = 0; i < std::min(endpoints.size(), kMaxCmAttempts); ++i) {
      std::shared_ptr<Connection> connection;
      try {
        if (co_await logon(endpoints[i], connection)) co_return;
        last_error = "no logon response";
      } catch (const std::exception& e) {
        last_error = e.what();
      }
      if (connection) connection->close();
    }
    FailTransient("Failed to connect to Steam CM: " + last_error);
  }

  // Opens the socket and logs on anonymously; on success starts the background coroutines. Returns false if the
  // socket closes first, throws on a rejected logon or a failed connection.
  asio::awaitable<bool> logon(const std::string& endpoint, std::shared_ptr<Connection>& connection) {
    Url url = ParseUrl(EndpointUrl(endpoint));
    connection = co_await Connection::open(m_net, url, std::chrono::seconds(m_config.connect_timeout_seconds));
    connection->arm(kLogonTimeout);
    co_await ws::Handshake(*connection, url, url.target);

    Packet logon;
    logon.emsg = emsg::kClientLogon;
    logon.steamid = kAnonymousSteamId;
    logon.body = LogonBody();
    Bytes frame = ws::EncodeFrame(ws::kBinary, SerializePacket(logon));
    co_await connection->writeAll(frame.data(), frame.size());
    std::optional<Packet> response = co_await awaitLogonResponse(*connection);
    if (!response) co_return false;
    LogonResult result = ParseLogonResponse(*response);
    if (result.eresult != kEResultOk) FailEResult("Logon failed", result.eresult);

    connection->disarm();
    m_steamid = response->steamid;
    m_sessionid = response->sessionid;
    m_conn = connection;
    m_connected = true;
    std::uint64_t generation = m_generation;
    m_active += 3;  // before the first coroutine can run to its end
    asio::co_spawn(m_executor, readerLoop(connection, generation), asio::detached);
    asio::co_spawn(m_executor, writerLoop(connection, generation), asio::detached);
    asio::co_spawn(
        m_executor,
        heartbeatLoop(generation, std::clamp(result.heartbeat_seconds, kMinHeartbeatSeconds, kMaxHeartbeatSeconds)),
        asio::detached);
    co_return true;
  }

  // Reads until the logon response arrives; nullopt if the socket closes. No other coroutine uses the connection yet.
  asio::awaitable<std::optional<Packet>> awaitLogonResponse(Connection& connection) {
    Bytes partial;
    for (;;) {
      ws::Frame frame = co_await ws::ReadFrame(connection);
      if (frame.opcode == ws::kClose) co_return std::nullopt;
      if (frame.opcode == ws::kPing) {
        Bytes pong = ws::EncodeFrame(ws::kPong, frame.payload);
        co_await connection.writeAll(pong.data(), pong.size());
        continue;
      }
      if (frame.opcode != ws::kBinary && frame.opcode != ws::kText && frame.opcode != ws::kContinuation) continue;
      partial.insert(partial.end(), frame.payload.begin(), frame.payload.end());
      if (!frame.fin) continue;
      Bytes message = std::move(partial);
      partial.clear();
      for (Packet& packet : ParseMessage(message)) {
        if (packet.emsg == emsg::kClientLogonResponse) co_return std::move(packet);
      }
    }
  }

  void enqueue(const Bytes& packet) {
    if (!m_connected) FailTransient("Session not connected");
    m_outbox.push_back(ws::EncodeFrame(ws::kBinary, packet));
    m_outboxSignal.notify();
  }

  // Stops accepting calls and fails the ones in flight; wakes the background coroutines so they notice.
  void markDisconnected(const char* reason) {
    if (m_connected) ++m_generation;
    m_connected = false;
    for (auto& [id, job] : m_jobs) {
      job->error = std::make_exception_ptr(TransientError(reason));
      job->done.cancel();
    }
    m_jobs.clear();
    m_outboxSignal.notify();
    m_heartbeat.cancel();
  }

  void finishTask() {
    --m_active;
    m_idleSignal.notify();
  }

  // Hands each packet of a message to the call waiting for it. Returns false once Steam has logged the session off.
  bool dispatch(ByteSpan message) {
    std::vector<Packet> packets;
    try {
      packets = ParseMessage(message);
    } catch (const std::exception&) {
      return true;  // malformed message: nothing to route
    }
    bool open = true;
    for (Packet& packet : packets) {
      if (packet.emsg == emsg::kClientLoggedOff) open = false;
      if (packet.jobid_target == kNoJob) continue;
      auto it = m_jobs.find(packet.jobid_target);
      if (it == m_jobs.end()) continue;
      std::shared_ptr<Job> job = std::move(it->second);
      m_jobs.erase(it);
      job->reply = std::move(packet);
      job->done.cancel();
    }
    return open;
  }

  asio::awaitable<void> readerLoop(std::shared_ptr<Connection> connection, std::uint64_t generation) {
    Bytes partial;
    try {
      for (bool open = true; open && generation == m_generation;) {
        ws::Frame frame = co_await ws::ReadFrame(*connection);
        if (frame.opcode == ws::kClose) break;
        if (frame.opcode == ws::kPing) {
          if (m_connected) m_outbox.push_back(ws::EncodeFrame(ws::kPong, frame.payload)), m_outboxSignal.notify();
          continue;
        }
        if (frame.opcode != ws::kBinary && frame.opcode != ws::kText && frame.opcode != ws::kContinuation) continue;
        partial.insert(partial.end(), frame.payload.begin(), frame.payload.end());
        if (!frame.fin) continue;
        Bytes message = std::move(partial);
        partial.clear();
        open = dispatch(message);
      }
    } catch (const std::exception&) {
      // The connection broke; calls in flight learn it from markDisconnected().
    }
    if (generation == m_generation) markDisconnected("Connection closed");
    connection->close();
    finishTask();
  }

  asio::awaitable<void> writerLoop(std::shared_ptr<Connection> connection, std::uint64_t generation) {
    try {
      for (;;) {
        while (m_outbox.empty() && generation == m_generation) co_await m_outboxSignal.wait();
        if (m_outbox.empty()) break;  // the connection is gone and nothing is left to send
        Bytes frame = std::move(m_outbox.front());
        m_outbox.pop_front();
        co_await connection->writeAll(frame.data(), frame.size());
      }
    } catch (const std::exception&) {
      connection->close();  // the reader fails the pending calls
    }
    finishTask();
  }

  asio::awaitable<void> heartbeatLoop(std::uint64_t generation, int interval_seconds) {
    while (generation == m_generation) {
      m_heartbeat.expires_after(std::chrono::seconds(interval_seconds));
      std::error_code woken;
      co_await m_heartbeat.async_wait(asio::redirect_error(asio::use_awaitable, woken));
      if (generation != m_generation) break;
      if (!woken) {
        m_outbox.push_back(ws::EncodeFrame(ws::kBinary, SerializePacket(sessionPacket(emsg::kClientHeartbeat))));
        m_outboxSignal.notify();
      }
    }
    finishTask();
  }

  HttpConfig m_config;
  std::shared_ptr<Net> m_net;
  asio::any_io_executor m_executor;

  Signal m_connectSignal;  // a connect() finished
  bool m_connecting = false;
  bool m_connected = false;
  std::shared_ptr<Connection> m_conn;
  std::uint64_t m_generation = 0;  // bumped when a connection goes away; its coroutines notice and end
  int m_active = 0;                // background coroutines still running
  Signal m_idleSignal;             // m_active changed

  std::uint64_t m_steamid = kAnonymousSteamId;
  std::int32_t m_sessionid = 0;
  std::uint64_t m_nextJobId = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<Job>> m_jobs;

  std::deque<Bytes> m_outbox;  // WebSocket frames
  Signal m_outboxSignal;
  asio::steady_timer m_heartbeat;
};

Session::Session(HttpConfig config) {
  if (!config.net) config.net = Net::acquire();
  m_impl = std::make_unique<Impl>(std::move(config));
}

Session::~Session() {
  if (!m_impl) return;
  std::shared_ptr<Net> net = m_impl->httpConfig().net;
  try {
    RunBlocking(net->executor(), m_impl->shutdown());
  } catch (...) {
    // The connection is going away anyway.
  }
  m_impl.reset();
}

const HttpConfig& Session::httpConfig() const { return m_impl->httpConfig(); }

asio::any_io_executor Session::executor() const { return m_impl->executor(); }

void Session::connect() { RunBlocking(m_impl->httpConfig().net->executor(), m_impl->connect()); }

asio::awaitable<void> Session::connectAsync() { co_await m_impl->connect(); }

asio::awaitable<Bytes> Session::request(std::uint32_t emsg, Bytes body) {
  co_await m_impl->connect();
  Packet reply = co_await m_impl->call(emsg, "", std::move(body));
  co_return std::move(reply.body);
}

asio::awaitable<Bytes> Session::rpc(std::string method, Bytes body) {
  co_await m_impl->connect();
  Packet response = co_await m_impl->call(emsg::kServiceMethodCall, method, std::move(body));
  if (response.eresult != kEResultOk) {
    FailEResult("RPC " + method + " failed", response.eresult, response.error_message);
  }
  co_return std::move(response.body);
}

}  // namespace pxsteamdl::detail
