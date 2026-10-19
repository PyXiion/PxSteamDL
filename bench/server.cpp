// SPDX-License-Identifier: LGPL-3.0-or-later
// A pretend Steam for benchmarks: serves the Web API, the CDN and the CM WebSocket over real TLS on localhost, from
// synthetic items (corpus.hpp). Point the library at it with a private CA (PXSTEAMDL_CA_BUNDLE_FILE) and /etc/hosts.
//
//   bench-server --pki DIR [--port 443] [--threads 2] [--latency-ms 0] [--seed 1] [--group
//   name:first_id:items:files:file_bytes:chunk_bytes]...
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "cm_packet.hpp"
#include "corpus.hpp"

namespace {

using asio::ip::tcp;
using Stream = asio::ssl::stream<tcp::socket>;
using pxsteamdl::bench::Bytes;
namespace detail = pxsteamdl::detail;

struct Settings {
  std::string pki;
  unsigned short port = 443;
  unsigned threads = 2;
  int latency_ms = 0;
  std::uint64_t seed = 1;
  std::vector<pxsteamdl::bench::Group> groups;
};

std::atomic<std::uint64_t> g_requests{0};
std::atomic<std::uint64_t> g_chunks{0};
std::atomic<std::uint64_t> g_connections{0};
std::atomic<std::uint64_t> g_bytes{0};

pxsteamdl::bench::Corpus g_corpus;
Settings g_settings;
constexpr std::string_view kCdnHosts[] = {"cdn1.bench.test", "cdn2.bench.test", "cdn3.bench.test", "cdn4.bench.test"};

struct Request {
  std::string method;
  std::string target;
  std::string host;
  std::string key;  // Sec-WebSocket-Key
  bool upgrade = false;
  bool close = false;
  std::size_t content_length = 0;
  std::string body;
};

struct Response {
  int status = 200;
  std::string content_type = "application/octet-stream";
  std::string text;
  const Bytes* bytes = nullptr;
};

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

Request ParseRequest(const std::string& head) {
  Request request;
  std::istringstream in(head);
  std::string line;
  std::getline(in, line);
  std::istringstream first(line);
  std::string version;
  first >> request.method >> request.target >> version;
  while (std::getline(in, line) && line != "\r") {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = Lower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ')) value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    if (name == "host")
      request.host = value;
    else if (name == "content-length")
      request.content_length = std::stoull(value);
    else if (name == "sec-websocket-key")
      request.key = value;
    else if (name == "upgrade")
      request.upgrade = Lower(value) == "websocket";
    else if (name == "connection")
      request.close = Lower(value).find("close") != std::string::npos;
  }
  return request;
}

std::vector<std::uint64_t> ParseIds(const std::string& form) {
  std::vector<std::uint64_t> ids;
  constexpr std::string_view kKey = "%5D=";
  for (std::size_t at = form.find(kKey); at != std::string::npos; at = form.find(kKey, at)) {
    at += kKey.size();
    std::uint64_t id = 0;
    while (at < form.size() && form[at] >= '0' && form[at] <= '9')
      id = id * 10 + static_cast<std::uint64_t>(form[at++] - '0');
    ids.push_back(id);
  }
  return ids;
}

Response Route(const Request& request) {
  ++g_requests;
  const std::string& target = request.target;
  Response response;
  std::string host = request.host.substr(0, request.host.find(':'));
  if (host == "api.steampowered.com") {
    response.content_type = "application/json";
    if (target.starts_with("/ISteamRemoteStorage/GetPublishedFileDetails/v1/")) {
      nlohmann::json details = nlohmann::json::array();
      for (std::uint64_t id : ParseIds(request.body)) {
        auto it = g_corpus.items.find(id);
        if (it == g_corpus.items.end()) {
          details.push_back({{"publishedfileid", std::to_string(id)}, {"result", 9}});
        } else {
          details.push_back({{"publishedfileid", std::to_string(id)},
                             {"result", 1},
                             {"hcontent_file", std::to_string(it->second.manifest_id)},
                             {"consumer_app_id", pxsteamdl::bench::kAppId},
                             {"title", it->second.title}});
        }
      }
      response.text =
          nlohmann::json{
              {"response", {{"result", 1}, {"resultcount", details.size()}, {"publishedfiledetails", details}}}}
              .dump();
    } else if (target.starts_with("/IContentServerDirectoryService/GetServersForSteamPipe/v1/")) {
      nlohmann::json servers = nlohmann::json::array();
      for (std::string_view name : kCdnHosts) {
        servers.push_back({{"type", "CDN"}, {"https_support", "mandatory"}, {"host", std::string(name)}});
      }
      response.text = nlohmann::json{{"response", {{"servers", servers}}}}.dump();
    } else if (target.starts_with("/ISteamDirectory/GetCMListForConnect/v1/")) {
      response.text =
          nlohmann::json{
              {"response",
               {{"serverlist",
                 {{{"type", "websockets"}, {"endpoint", "cm1.bench.test:" + std::to_string(g_settings.port)}}}}}}}
              .dump();
    } else {
      response.status = 404;
    }
    return response;
  }
  // CDN: /depot/<depot>/manifest/<manifest>/<version>/<code> or /depot/<depot>/chunk/<sha>
  std::size_t chunk = target.find("/chunk/");
  if (chunk != std::string::npos) {
    ++g_chunks;
    auto it = g_corpus.chunks.find(target.substr(chunk + 7));
    if (it == g_corpus.chunks.end()) {
      response.status = 404;
    } else {
      response.bytes = &it->second;
    }
    return response;
  }
  std::size_t manifest = target.find("/manifest/");
  if (manifest != std::string::npos) {
    std::uint64_t manifest_id = std::stoull(target.substr(manifest + 10));
    for (const auto& [id, item] : g_corpus.items) {
      if (item.manifest_id == manifest_id) {
        response.bytes = &item.manifest;
        return response;
      }
    }
  }
  response.status = 404;
  return response;
}

asio::awaitable<void> Delay() {
  if (g_settings.latency_ms <= 0) co_return;
  asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds(g_settings.latency_ms));
  co_await timer.async_wait(asio::use_awaitable);
}

std::string Base64(const unsigned char* data, std::size_t size) {
  std::string out(4 * ((size + 2) / 3), '\0');
  int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
  out.resize(static_cast<std::size_t>(length));
  return out;
}

asio::awaitable<void> Need(Stream& stream, std::string& buffer, std::size_t size) {
  if (buffer.size() < size) {
    co_await asio::async_read(stream, asio::dynamic_buffer(buffer), asio::transfer_exactly(size - buffer.size()),
                              asio::use_awaitable);
  }
}

Bytes Frame(std::uint8_t opcode, const Bytes& payload) {
  Bytes frame{static_cast<std::uint8_t>(0x80 | opcode)};
  if (payload.size() < 126) {
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(126);
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
  } else {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame.push_back(static_cast<std::uint8_t>(std::uint64_t{payload.size()} >> shift));
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

// A CM packet with the header fields the library reads, including EResult (which SerializePacket leaves out).
Bytes CmReply(std::uint32_t emsg_id, std::uint64_t job_target, std::int32_t eresult, const Bytes& body) {
  Bytes header = detail::Concat({detail::EncodeFixed64(1, 0x0110000100000001ULL), detail::EncodeUint(2, 1234),
                                 job_target != detail::kNoJob ? detail::EncodeFixed64(11, job_target) : Bytes{},
                                 detail::EncodeUint(13, static_cast<std::uint64_t>(eresult))});
  Bytes out;
  detail::AppendLe32(out, emsg_id | 0x80000000u);
  detail::AppendLe32(out, static_cast<std::uint32_t>(header.size()));
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

asio::awaitable<void> ServeWebSocket(Stream& stream, std::string& buffer, const std::string& key) {
  std::string accept_input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(accept_input.data()), accept_input.size(), digest);
  std::string upgrade =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
      Base64(digest, sizeof digest) + "\r\n\r\n";
  co_await asio::async_write(stream, asio::buffer(upgrade), asio::use_awaitable);
  for (;;) {
    co_await Need(stream, buffer, 2);
    std::uint8_t opcode = static_cast<std::uint8_t>(buffer[0]) & 0x0F;
    std::uint64_t length = static_cast<std::uint8_t>(buffer[1]) & 0x7F;
    bool masked = static_cast<std::uint8_t>(buffer[1]) & 0x80;
    std::size_t header = 2;
    if (length == 126) {
      co_await Need(stream, buffer, 4);
      length = (std::uint64_t{static_cast<std::uint8_t>(buffer[2])} << 8) | static_cast<std::uint8_t>(buffer[3]);
      header = 4;
    } else if (length == 127) {
      co_await Need(stream, buffer, 10);
      length = 0;
      for (int i = 0; i < 8; ++i) length = (length << 8) | static_cast<std::uint8_t>(buffer[2 + i]);
      header = 10;
    }
    std::size_t mask_at = header;
    if (masked) header += 4;
    co_await Need(stream, buffer, header + length);
    Bytes payload(length);
    for (std::size_t i = 0; i < length; ++i) {
      payload[i] = static_cast<std::uint8_t>(buffer[header + i]) ^
                   (masked ? static_cast<std::uint8_t>(buffer[mask_at + i % 4]) : 0);
    }
    buffer.erase(0, header + length);
    if (opcode == 0x8) co_return;
    if (opcode == 0x9) {
      Bytes pong = Frame(0xA, payload);
      co_await asio::async_write(stream, asio::buffer(pong), asio::use_awaitable);
      continue;
    }
    if (opcode != 0x2) continue;
    std::vector<detail::Packet> packets = detail::ParseMessage(payload);
    for (const detail::Packet& packet : packets) {
      Bytes reply;
      switch (packet.emsg) {
        case detail::emsg::kClientLogon:
          reply = CmReply(detail::emsg::kClientLogonResponse, detail::kNoJob, 1,
                          detail::Concat({detail::EncodeUint(1, 1), detail::EncodeUint(3, 30)}));
          break;
        case detail::emsg::kClientGetDepotDecryptionKey:
          co_await Delay();
          reply =
              CmReply(5439, packet.jobid_source, 1,
                      detail::Concat({detail::EncodeUint(1, 1), detail::EncodeBytes(3, pxsteamdl::bench::DepotKey())}));
          break;
        case detail::emsg::kServiceMethodCall:
          co_await Delay();
          reply = CmReply(147, packet.jobid_source, 1, detail::EncodeUint(1, pxsteamdl::bench::kRequestCode));
          break;
        case detail::emsg::kClientLogOff:
          co_return;
        default:
          break;  // heartbeats and anything else need no answer
      }
      if (reply.empty()) continue;
      Bytes frame = Frame(0x2, reply);
      co_await asio::async_write(stream, asio::buffer(frame), asio::use_awaitable);
    }
  }
}

asio::awaitable<void> Serve(tcp::socket socket, asio::ssl::context& context) {
  ++g_connections;
  socket.set_option(tcp::no_delay(true));
  Stream stream(std::move(socket), context);
  try {
    co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
    std::string buffer;
    for (;;) {
      std::size_t head_size =
          co_await asio::async_read_until(stream, asio::dynamic_buffer(buffer), "\r\n\r\n", asio::use_awaitable);
      Request request = ParseRequest(buffer.substr(0, head_size));
      buffer.erase(0, head_size);
      if (request.upgrade) {
        co_await ServeWebSocket(stream, buffer, request.key);
        co_return;
      }
      if (request.content_length > 0) {
        co_await Need(stream, buffer, request.content_length);
        request.body = buffer.substr(0, request.content_length);
        buffer.erase(0, request.content_length);
      }
      Response response = Route(request);
      co_await Delay();
      std::size_t size = response.bytes ? response.bytes->size() : response.text.size();
      std::string head = "HTTP/1.1 " + std::to_string(response.status) +
                         (response.status == 200 ? " OK" : " Not Found") +
                         "\r\nContent-Type: " + response.content_type + "\r\nContent-Length: " + std::to_string(size) +
                         (request.close ? "\r\nConnection: close" : "\r\nConnection: keep-alive") + "\r\n\r\n";
      std::array<asio::const_buffer, 2> parts{
          asio::buffer(head), response.bytes ? asio::buffer(*response.bytes) : asio::buffer(response.text)};
      co_await asio::async_write(stream, parts, asio::use_awaitable);
      g_bytes += size;
      if (request.close) break;
    }
  } catch (const std::exception&) {
    // The client went away or spoke garbage; the connection ends.
  }
}

asio::awaitable<void> Listen(asio::ssl::context& context) {
  auto executor = co_await asio::this_coro::executor;
  tcp::acceptor acceptor(executor, tcp::endpoint(tcp::v4(), g_settings.port));
  acceptor.set_option(asio::socket_base::reuse_address(true));
  std::printf("READY\n");
  std::fflush(stdout);
  for (;;) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    asio::co_spawn(executor, Serve(std::move(socket), context), asio::detached);
  }
}

pxsteamdl::bench::Group ParseGroup(const std::string& spec) {
  pxsteamdl::bench::Group group;
  std::vector<std::string> parts;
  std::stringstream in(spec);
  for (std::string part; std::getline(in, part, ':');) parts.push_back(part);
  if (parts.size() != 6) throw std::runtime_error("--group wants name:first_id:items:files:file_bytes:chunk_bytes");
  group.name = parts[0];
  group.first_id = std::stoull(parts[1]);
  group.items = std::stoi(parts[2]);
  group.files = std::stoi(parts[3]);
  group.file_bytes = std::stoull(parts[4]);
  group.chunk_bytes = std::stoull(parts[5]);
  return group;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto value = [&] {
        if (++i >= argc) throw std::runtime_error("missing value for " + arg);
        return std::string(argv[i]);
      };
      if (arg == "--pki")
        g_settings.pki = value();
      else if (arg == "--port")
        g_settings.port = static_cast<unsigned short>(std::stoi(value()));
      else if (arg == "--threads")
        g_settings.threads = static_cast<unsigned>(std::stoi(value()));
      else if (arg == "--latency-ms")
        g_settings.latency_ms = std::stoi(value());
      else if (arg == "--seed")
        g_settings.seed = std::stoull(value());
      else if (arg == "--group")
        g_settings.groups.push_back(ParseGroup(value()));
      else
        throw std::runtime_error("unknown option " + arg);
    }
    if (g_settings.pki.empty()) throw std::runtime_error("--pki DIR is required");
    if (g_settings.groups.empty()) {
      // 4 items x 4 files x 16 MiB in 1 MiB chunks, and 4 items x 400 files x 8 KiB in one chunk each.
      g_settings.groups.push_back(ParseGroup("bulk:1000:4:4:16777216:1048576"));
      g_settings.groups.push_back(ParseGroup("small:2000:4:400:8192:8192"));
    }
    for (const auto& group : g_settings.groups) g_corpus.add(group, g_settings.seed);
    std::fprintf(stderr, "corpus: %zu items, %zu chunks, %.1f MiB plain, %.1f MiB on the wire\n", g_corpus.items.size(),
                 g_corpus.chunks.size(), static_cast<double>(g_corpus.total_plain) / 1048576.0,
                 static_cast<double>(g_corpus.total_wire) / 1048576.0);

    asio::ssl::context context(asio::ssl::context::tls_server);
    context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                        asio::ssl::context::no_sslv3);
    context.use_certificate_chain_file(g_settings.pki + "/server.pem");
    context.use_private_key_file(g_settings.pki + "/server.key", asio::ssl::context::pem);

    asio::io_context io(static_cast<int>(g_settings.threads));
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code&, int) { io.stop(); });
    asio::co_spawn(io, Listen(context), [](std::exception_ptr error) {
      if (error) std::rethrow_exception(error);
    });
    std::vector<std::thread> threads;
    for (unsigned i = 1; i < g_settings.threads; ++i) threads.emplace_back([&] { io.run(); });
    io.run();
    for (auto& thread : threads) thread.join();
    std::fprintf(stderr, "served %llu requests (%llu chunks) on %llu connections, %.1f MiB\n",
                 static_cast<unsigned long long>(g_requests.load()), static_cast<unsigned long long>(g_chunks.load()),
                 static_cast<unsigned long long>(g_connections.load()),
                 static_cast<double>(g_bytes.load()) / 1048576.0);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bench-server: %s\n", e.what());
    return 1;
  }
}
