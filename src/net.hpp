// SPDX-License-Identifier: LGPL-3.0-or-later
// The asio side of the library: one io thread, TLS context, name cache and buffered TLS connections with a watchdog.
// Everything here runs on the io thread, so none of it is locked.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "common.hpp"

namespace pxsteamdl::detail {

// Runs a coroutine on executor from another thread and waits for its result.
template <class T, class Executor>
T RunBlocking(Executor executor, asio::awaitable<T> work) {
  return asio::co_spawn(executor, std::move(work), asio::use_future).get();
}

struct Url {
  bool tls = true;
  std::string host;
  std::string port;
  // Path and query, starting with '/'.
  std::string target;
  // scheme://host:port, the key of a connection pool.
  std::string origin() const { return std::string(tls ? "https://" : "http://") + host + ":" + port; }
};

// Throws kRejected for anything but http(s)://host[:port]/...
Url ParseUrl(std::string_view url);

class Connection;

// Process-wide asio state, shared by every Client through a shared_ptr; the io thread ends with the last user.
class Net {
 public:
  static std::shared_ptr<Net> acquire();
  ~Net();
  Net(const Net&) = delete;
  Net& operator=(const Net&) = delete;

  asio::io_context& io() { return m_io; }
  asio::io_context::executor_type executor() { return m_io.get_executor(); }
  asio::ssl::context& tls() { return m_tls; }

  // Resolved addresses of host:port, IPv4 first; cached until a connection to them fails.
  asio::awaitable<std::vector<asio::ip::tcp::endpoint>> resolve(const std::string& host, const std::string& port);
  void forgetAddresses(const std::string& host, const std::string& port);

  // Idle keep-alive connections by origin.
  std::shared_ptr<Connection> takeIdle(const std::string& origin);
  void giveIdle(const std::string& origin, std::shared_ptr<Connection> connection);

 private:
  Net();

  asio::io_context m_io{1};
  asio::ssl::context m_tls{asio::ssl::context::tls_client};
  asio::ip::tcp::resolver m_resolver{m_io};
  std::unordered_map<std::string, std::vector<asio::ip::tcp::endpoint>> m_addresses;
  struct Idle {
    std::shared_ptr<Connection> connection;
    std::chrono::steady_clock::time_point since;
  };
  std::unordered_map<std::string, std::vector<Idle>> m_idle;
  std::size_t m_idleCount = 0;
  asio::executor_work_guard<asio::io_context::executor_type> m_work{m_io.get_executor()};
  std::thread m_thread;
};

// A connected socket, with TLS for https. Reads are buffered; a watchdog closes the socket when nothing moves for
// the armed time, which makes the pending operation fail (see timedOut()).
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  // Resolves, connects (trying each address) and, for TLS, handshakes within connect_timeout.
  static asio::awaitable<std::shared_ptr<Connection>> open(std::shared_ptr<Net> net, Url url,
                                                           std::chrono::seconds connect_timeout);
  ~Connection();

  // Reads whatever is available (at least one byte) into dst, from the buffer first. Throws TransientError on EOF.
  asio::awaitable<std::size_t> readSome(std::uint8_t* dst, std::size_t size);
  // Fills dst completely.
  asio::awaitable<void> readExact(std::uint8_t* dst, std::size_t size);
  // Returns the bytes up to and including the first occurrence of delimiter.
  asio::awaitable<std::string> readUntil(std::string_view delimiter, std::size_t limit);
  asio::awaitable<void> writeAll(const std::uint8_t* data, std::size_t size);

  // Arms the watchdog: close() if no read or write completes within the time. touch() restarts the countdown.
  void arm(std::chrono::seconds idle_limit);
  void disarm();
  bool timedOut() const { return m_timedOut; }

  void close();
  bool isOpen() const { return m_socket.is_open(); }
  // Whether the peer closed an idle connection (or sent stray bytes).
  bool looksDead();
  // Whether any byte of a response was read since markRequestStart().
  bool gotBytes() const { return m_gotBytes; }
  void markRequestStart() { m_gotBytes = false; }
  bool reused = false;

 private:
  Connection(Net& net, bool tls, std::string host);

  asio::awaitable<std::size_t> rawRead(std::uint8_t* dst, std::size_t size);
  void watchdogTick(const std::error_code& ec, std::uint64_t generation);

  asio::ip::tcp::socket m_socket;
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> m_stream;
  std::string m_host;
  asio::steady_timer m_watchdog;
  std::chrono::seconds m_idleLimit{0};
  std::chrono::steady_clock::time_point m_lastActivity;
  std::uint64_t m_watchGeneration = 0;
  bool m_timedOut = false;
  bool m_armed = false;
  bool m_gotBytes = false;
  Bytes m_in;
  std::size_t m_inBegin = 0;
  std::size_t m_inEnd = 0;
};

}  // namespace pxsteamdl::detail
