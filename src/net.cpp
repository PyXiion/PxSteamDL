// SPDX-License-Identifier: LGPL-3.0-or-later
#include "net.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace pxsteamdl::detail {

// Generated from the pinned Mozilla CA bundle by CMakeLists.txt; NUL-terminated.
extern const unsigned char kCaBundle[];
extern const std::size_t kCaBundleSize;

namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;
constexpr std::size_t kMaxIdleConnections = 256;
constexpr auto kIdleLifetime = std::chrono::seconds(60);

void LoadCaBundle(asio::ssl::context& context) {
  X509_STORE* store = SSL_CTX_get_cert_store(context.native_handle());
  BIO* bio = BIO_new_mem_buf(kCaBundle, static_cast<int>(kCaBundleSize - 1));
  if (!bio) Fail(ErrorKind::kOther, "cannot read the CA bundle");
  std::size_t loaded = 0;
  while (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
    if (X509_STORE_add_cert(store, certificate) == 1) ++loaded;
    X509_free(certificate);
  }
  BIO_free(bio);
  ERR_clear_error();  // PEM_read_bio_X509 leaves "no start line" behind at the end of the data
  if (loaded == 0) Fail(ErrorKind::kOther, "the CA bundle holds no certificate");
}

bool IsIpLiteral(const std::string& host) {
  std::error_code ignored;
  asio::ip::make_address(host, ignored);
  return !ignored;
}

}  // namespace

Url ParseUrl(std::string_view url) {
  Url result;
  std::string_view rest = url;
  if (rest.starts_with("https://")) {
    rest.remove_prefix(8);
  } else if (rest.starts_with("http://")) {
    result.tls = false;
    rest.remove_prefix(7);
  } else {
    Fail(ErrorKind::kRejected, "unsupported URL: " + std::string(url));
  }
  std::size_t end = rest.find_first_of("/?#");
  std::string_view authority = rest.substr(0, end);
  std::string_view tail = end == std::string_view::npos ? std::string_view() : rest.substr(end);
  if (std::size_t hash = tail.find('#'); hash != std::string_view::npos) tail = tail.substr(0, hash);
  result.target = tail.empty() ? "/" : (tail.front() == '/' ? std::string(tail) : "/" + std::string(tail));
  std::string_view port;
  if (!authority.empty() && authority.front() == '[') {
    std::size_t close = authority.find(']');
    if (close == std::string_view::npos) Fail(ErrorKind::kRejected, "unsupported URL: " + std::string(url));
    result.host = std::string(authority.substr(1, close - 1));
    if (authority.size() > close + 1 && authority[close + 1] == ':') port = authority.substr(close + 2);
  } else {
    std::size_t colon = authority.rfind(':');
    result.host = std::string(authority.substr(0, colon));
    if (colon != std::string_view::npos) port = authority.substr(colon + 1);
  }
  if (result.host.empty() || result.host.find('@') != std::string::npos) {
    Fail(ErrorKind::kRejected, "unsupported URL: " + std::string(url));
  }
  result.port = port.empty() ? (result.tls ? "443" : "80") : std::string(port);
  return result;
}

// ---- Net ----

std::shared_ptr<Net> Net::acquire() {
  static std::mutex mutex;
  static std::weak_ptr<Net> current;
  std::lock_guard lock(mutex);
  if (auto net = current.lock()) return net;
  // The last reference may be dropped by a coroutine on the io thread, which cannot join itself: a helper thread
  // then does the shutdown. The helper is joined at process exit so that it cannot still be tearing down TLS state
  // while static destructors run.
  std::shared_ptr<Net> net(new Net(), [](Net* released) {
    if (std::this_thread::get_id() == released->m_thread.get_id()) {
      struct Reaper {
        std::mutex mutex;
        std::vector<std::thread> threads;
        ~Reaper() {
          std::vector<std::thread> done;
          {
            std::lock_guard lock(mutex);
            done.swap(threads);
          }
          for (std::thread& thread : done) thread.join();
        }
      };
      static Reaper reaper;  // first used after a Net exists, so it is destroyed before the TLS library's own cleanup
      std::lock_guard lock(reaper.mutex);
      reaper.threads.emplace_back([released] { delete released; });
    } else {
      delete released;
    }
  });
  current = net;
  return net;
}

Net::Net() {
  m_tls.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                    asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
  m_tls.set_verify_mode(asio::ssl::verify_peer);
  LoadCaBundle(m_tls);
  m_thread = std::thread([this] { m_io.run(); });
}

Net::~Net() {
  asio::post(m_io, [this] {
    m_idle.clear();
    m_idleCount = 0;
  });
  m_work.reset();
  m_thread.join();
}

asio::awaitable<std::vector<asio::ip::tcp::endpoint>> Net::resolve(const std::string& host, const std::string& port) {
  std::string key = host + ":" + port;
  if (auto it = m_addresses.find(key); it != m_addresses.end()) co_return it->second;
  std::vector<asio::ip::tcp::endpoint> endpoints;
  try {
    auto results = co_await m_resolver.async_resolve(host, port, asio::use_awaitable);
    for (const auto& entry : results) endpoints.push_back(entry.endpoint());
  } catch (const std::system_error& e) {
    FailTransient("cannot resolve " + host + ": " + e.code().message());
  }
  // IPv4 first: without happy eyeballs, a dead IPv6 route would otherwise cost a full connect timeout.
  std::stable_partition(endpoints.begin(), endpoints.end(),
                        [](const auto& endpoint) { return endpoint.address().is_v4(); });
  m_addresses[key] = endpoints;
  co_return endpoints;
}

void Net::forgetAddresses(const std::string& host, const std::string& port) { m_addresses.erase(host + ":" + port); }

std::shared_ptr<Connection> Net::takeIdle(const std::string& origin) {
  auto it = m_idle.find(origin);
  if (it == m_idle.end()) return nullptr;
  auto now = std::chrono::steady_clock::now();
  auto& list = it->second;
  while (!list.empty()) {
    Idle idle = std::move(list.back());
    list.pop_back();
    --m_idleCount;
    if (now - idle.since < kIdleLifetime && idle.connection->isOpen() && !idle.connection->looksDead()) {
      idle.connection->reused = true;
      return idle.connection;
    }
  }
  return nullptr;
}

void Net::giveIdle(const std::string& origin, std::shared_ptr<Connection> connection) {
  if (m_idleCount >= kMaxIdleConnections) {
    connection->close();  // also ends the watchdog, which would keep the connection alive until it fires
    return;
  }
  connection->disarm();
  m_idle[origin].push_back({std::move(connection), std::chrono::steady_clock::now()});
  ++m_idleCount;
}

// ---- Connection ----

Connection::Connection(Net& net, bool tls, std::string host)
    : m_socket(net.io()), m_host(std::move(host)), m_watchdog(net.io()), m_in(kReadBufferSize) {
  if (tls) m_stream = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(m_socket, net.tls());
}

Connection::~Connection() { close(); }

asio::awaitable<std::shared_ptr<Connection>> Connection::open(std::shared_ptr<Net> net, Url url,
                                                              std::chrono::seconds connect_timeout) {
  std::shared_ptr<Connection> connection(new Connection(*net, url.tls, url.host));
  connection->arm(connect_timeout);
  std::vector<asio::ip::tcp::endpoint> endpoints = co_await net->resolve(url.host, url.port);
  if (connection->m_timedOut) FailTransient("resolving " + url.host + " timed out");
  std::string last_error = "no address";
  bool connected = false;
  for (const auto& endpoint : endpoints) {
    try {
      co_await connection->m_socket.async_connect(endpoint, asio::use_awaitable);
      connected = true;
      break;
    } catch (const std::system_error& e) {
      last_error = connection->m_timedOut ? "timed out" : e.code().message();
      std::error_code ignored;
      connection->m_socket.close(ignored);
      if (connection->m_timedOut) break;
    }
  }
  if (!connected) {
    net->forgetAddresses(url.host, url.port);
    FailTransient("cannot connect to " + url.host + ":" + url.port + ": " + last_error);
  }
  connection->m_socket.set_option(asio::ip::tcp::no_delay(true));
  if (connection->m_stream) {
    SSL* ssl = connection->m_stream->native_handle();
    if (!IsIpLiteral(url.host)) SSL_set_tlsext_host_name(ssl, url.host.c_str());
    connection->m_stream->set_verify_callback(asio::ssl::host_name_verification(url.host));
    try {
      co_await connection->m_stream->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
    } catch (const std::system_error& e) {
      long verify = SSL_get_verify_result(ssl);
      if (verify != X509_V_OK) {
        Fail("TLS certificate of " + url.host + " rejected: " + X509_verify_cert_error_string(verify));
      }
      FailTransient("TLS handshake with " + url.host +
                    " failed: " + (connection->m_timedOut ? "timed out" : e.code().message()));
    }
  }
  connection->disarm();
  co_return connection;
}

void Connection::arm(std::chrono::seconds idle_limit) {
  m_idleLimit = idle_limit;
  m_lastActivity = std::chrono::steady_clock::now();
  m_timedOut = false;
  m_armed = true;
  m_watchdog.expires_after(idle_limit);
  m_watchdog.async_wait([self = shared_from_this(), generation = ++m_watchGeneration](const std::error_code& ec) {
    self->watchdogTick(ec, generation);
  });
}

void Connection::disarm() {
  m_armed = false;
  ++m_watchGeneration;
  m_watchdog.cancel();
}

void Connection::watchdogTick(const std::error_code& ec, std::uint64_t generation) {
  if (ec || !m_armed || generation != m_watchGeneration) return;
  auto deadline = m_lastActivity + m_idleLimit;
  if (std::chrono::steady_clock::now() < deadline) {
    m_watchdog.expires_at(deadline);
    m_watchdog.async_wait([self = shared_from_this(), generation](const std::error_code& again) {
      self->watchdogTick(again, generation);
    });
    return;
  }
  m_timedOut = true;
  m_armed = false;
  std::error_code ignored;
  m_socket.close(ignored);
}

void Connection::close() {
  m_armed = false;
  ++m_watchGeneration;
  m_watchdog.cancel();
  std::error_code ignored;
  m_socket.close(ignored);
}

bool Connection::looksDead() {
  std::error_code ec;
  std::uint8_t probe;
  m_socket.non_blocking(true, ec);
  m_socket.receive(asio::buffer(&probe, 1), asio::socket_base::message_peek, ec);
  return ec != asio::error::would_block && ec != asio::error::try_again;
}

asio::awaitable<std::size_t> Connection::rawRead(std::uint8_t* dst, std::size_t size) {
  std::size_t read = 0;
  try {
    if (m_stream) {
      read = co_await m_stream->async_read_some(asio::buffer(dst, size), asio::use_awaitable);
    } else {
      read = co_await m_socket.async_read_some(asio::buffer(dst, size), asio::use_awaitable);
    }
  } catch (const std::system_error& e) {
    if (m_timedOut) FailTransient("transfer from " + m_host + " stalled");
    FailTransient("receiving from " + m_host + " failed: " + e.code().message());
  }
  m_lastActivity = std::chrono::steady_clock::now();
  m_gotBytes = true;
  co_return read;
}

asio::awaitable<std::size_t> Connection::readSome(std::uint8_t* dst, std::size_t size) {
  if (size == 0) co_return 0;
  if (m_inBegin == m_inEnd) {
    if (size >= m_in.size()) co_return co_await rawRead(dst, size);  // straight into the destination
    m_inBegin = 0;
    m_inEnd = co_await rawRead(m_in.data(), m_in.size());
  }
  std::size_t count = std::min(size, m_inEnd - m_inBegin);
  std::memcpy(dst, m_in.data() + m_inBegin, count);
  m_inBegin += count;
  co_return count;
}

asio::awaitable<void> Connection::readExact(std::uint8_t* dst, std::size_t size) {
  while (size > 0) {
    std::size_t count = co_await readSome(dst, size);
    dst += count;
    size -= count;
  }
}

asio::awaitable<std::string> Connection::readUntil(std::string_view delimiter, std::size_t limit) {
  std::string text;
  for (;;) {
    // Look for the delimiter in what is buffered, including its overlap with what was taken before.
    if (m_inBegin == m_inEnd) {
      m_inBegin = 0;
      m_inEnd = co_await rawRead(m_in.data(), m_in.size());
    }
    std::size_t before = text.size();
    text.append(reinterpret_cast<const char*>(m_in.data()) + m_inBegin, m_inEnd - m_inBegin);
    std::size_t from = before >= delimiter.size() ? before - delimiter.size() + 1 : 0;
    std::size_t at = text.find(delimiter, from);
    if (at != std::string::npos) {
      std::size_t keep = at + delimiter.size();
      m_inBegin += keep - before;  // leave what follows the delimiter buffered
      text.resize(keep);
      co_return text;
    }
    m_inBegin = m_inEnd;
    if (text.size() > limit) Fail("response header too large");
  }
}

asio::awaitable<void> Connection::writeAll(const std::uint8_t* data, std::size_t size) {
  try {
    if (m_stream) {
      co_await asio::async_write(*m_stream, asio::buffer(data, size), asio::use_awaitable);
    } else {
      co_await asio::async_write(m_socket, asio::buffer(data, size), asio::use_awaitable);
    }
  } catch (const std::system_error& e) {
    if (m_timedOut) FailTransient("sending to " + m_host + " timed out");
    FailTransient("sending to " + m_host + " failed: " + e.code().message());
  }
  m_lastActivity = std::chrono::steady_clock::now();
}

}  // namespace pxsteamdl::detail
