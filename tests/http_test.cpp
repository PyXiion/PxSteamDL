// SPDX-License-Identifier: LGPL-3.0-or-later
// The real HTTP client (src/http.cpp, src/net.cpp) against a scripted server on the loopback interface. Plain http://
// URLs keep these tests free of certificates; the TLS path is covered by the benchmark server.
#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>

#include <asio.hpp>
#include <gtest/gtest.h>

#include "http.hpp"
#include "net.hpp"

namespace pxsteamdl::detail {
namespace {

using std::chrono::steady_clock;

// Answers each request with whatever the script returns; a reply starting with "!" is sent and then the connection
// is closed. Every accepted connection gets its own thread.
class ScriptedServer {
 public:
  using Script = std::function<std::string(const std::string& request, int connection, int index)>;

  explicit ScriptedServer(Script script)
      : m_script(std::move(script)), m_acceptor(m_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    m_thread = std::thread([this] {
      for (;;) {
        asio::error_code ec;
        asio::ip::tcp::socket socket(m_io);
        m_acceptor.accept(socket, ec);
        if (ec || m_stopping) return;
        int connection = m_connections++;
        std::lock_guard lock(m_mutex);
        m_handlers.emplace_back([this, connection, s = std::make_shared<asio::ip::tcp::socket>(std::move(socket))] {
          serve(*s, connection);
        });
      }
    });
  }

  ~ScriptedServer() {
    // A blocking accept() is not woken by close(): connect once to let the thread see the flag.
    m_stopping = true;
    asio::error_code ignored;
    asio::ip::tcp::socket wake(m_io);
    wake.connect(m_acceptor.local_endpoint(), ignored);
    m_thread.join();
    for (std::thread& handler : m_handlers) handler.join();
  }

  std::string url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(m_acceptor.local_endpoint().port()) + path;
  }
  int connections() const { return m_connections; }

 private:
  void serve(asio::ip::tcp::socket& socket, int connection) {
    asio::streambuf buffer;
    for (int index = 0;; ++index) {
      asio::error_code ec;
      std::size_t head = asio::read_until(socket, buffer, "\r\n\r\n", ec);
      if (ec) return;
      std::string request(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + static_cast<std::ptrdiff_t>(head));
      buffer.consume(head);
      // A request body, if any, is part of the text the script sees.
      std::size_t at = request.find("Content-Length: ");
      if (at != std::string::npos) {
        std::size_t length = std::stoull(request.substr(at + 16));
        while (buffer.size() < length) {
          if (asio::read(socket, buffer, asio::transfer_exactly(length - buffer.size()), ec); ec) return;
        }
        request.append(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + static_cast<std::ptrdiff_t>(length));
        buffer.consume(length);
      }
      std::string reply = m_script(request, connection, index);
      bool close = !reply.empty() && reply.front() == '!';
      if (close) reply.erase(0, 1);
      asio::write(socket, asio::buffer(reply), ec);
      if (ec || close) {
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        return;
      }
    }
  }

  Script m_script;
  asio::io_context m_io;
  asio::ip::tcp::acceptor m_acceptor;
  std::thread m_thread;
  std::mutex m_mutex;
  std::list<std::thread> m_handlers;
  std::atomic<int> m_connections{0};
  std::atomic<bool> m_stopping{false};
};

std::string Ok(const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}

std::string Text(const Bytes& body) { return std::string(body.begin(), body.end()); }

TEST(HttpTest, ReadsABodyWithContentLength) {
  ScriptedServer server([](const std::string&, int, int) { return Ok("hello"); });
  HttpResponse response = HttpRequest(server.url("/a"), {});
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(Text(response.body), "hello");
}

TEST(HttpTest, SendsTheRequestLineHostAndPostBody) {
  std::string seen;
  ScriptedServer server([&](const std::string& request, int, int) {
    seen = request;
    return Ok("ok");
  });
  HttpResponse response = HttpRequest(server.url("/submit?x=1"), {}, "POST", "a=b&c=d", "application/x-www-form-urlencoded");
  EXPECT_EQ(response.status, 200);
  EXPECT_NE(seen.find("POST /submit?x=1 HTTP/1.1\r\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("Host: 127.0.0.1:"), std::string::npos);
  EXPECT_NE(seen.find("Content-Type: application/x-www-form-urlencoded\r\n"), std::string::npos);
  EXPECT_NE(seen.find("Content-Length: 7\r\n"), std::string::npos);
  EXPECT_EQ(seen.substr(seen.size() - 7), "a=b&c=d");
}

TEST(HttpTest, ReadsAChunkedBodyWithExtensionsAndTrailers) {
  ScriptedServer server([](const std::string&, int, int) {
    return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n");
  });
  EXPECT_EQ(Text(HttpRequest(server.url("/"), {}).body), "hello world");
}

TEST(HttpTest, ReadsABodyThatEndsWithTheConnection) {
  ScriptedServer server([](const std::string&, int, int) {
    return std::string("!HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil the end");
  });
  EXPECT_EQ(Text(HttpRequest(server.url("/"), {}).body), "until the end");
}

TEST(HttpTest, ReturnsErrorStatusesAsResponses) {
  ScriptedServer server([](const std::string&, int, int) {
    return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  });
  HttpResponse response = HttpRequest(server.url("/missing"), {});
  EXPECT_EQ(response.status, 404);
  EXPECT_TRUE(response.body.empty());
}

TEST(HttpTest, ReusesAKeepAliveConnection) {
  ScriptedServer server([](const std::string&, int, int) { return Ok("x"); });
  HttpConfig config;
  config.net = Net::acquire();  // the pool lives as long as the Net, which a Client's session holds on to
  for (int i = 0; i < 5; ++i) EXPECT_EQ(HttpRequest(server.url("/"), config).status, 200);
  EXPECT_EQ(server.connections(), 1);
}

TEST(HttpTest, ReplacesAKeepAliveConnectionTheServerClosedMeanwhile) {
  // The server promises keep-alive but hangs up after every answer.
  ScriptedServer server([](const std::string&, int, int) {
    return "!" + Ok("fresh");
  });
  HttpConfig config;
  config.net = Net::acquire();
  EXPECT_EQ(Text(HttpRequest(server.url("/"), config).body), "fresh");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the FIN arrive
  EXPECT_EQ(Text(HttpRequest(server.url("/"), config).body), "fresh");
  EXPECT_EQ(server.connections(), 2);
}

TEST(HttpTest, FollowsRedirects) {
  ScriptedServer server([](const std::string& request, int, int) -> std::string {
    if (request.starts_with("GET /start ")) return "HTTP/1.1 302 Found\r\nLocation: /middle\r\nContent-Length: 0\r\n\r\n";
    if (request.starts_with("GET /middle ")) return "HTTP/1.1 301 Moved\r\nLocation: next\r\nContent-Length: 0\r\n\r\n";
    if (request.starts_with("GET /next ")) return Ok("arrived");
    return "HTTP/1.1 500 Oops\r\nContent-Length: 0\r\n\r\n";
  });
  EXPECT_EQ(Text(HttpRequest(server.url("/start"), {}).body), "arrived");
}

TEST(HttpTest, RedirectsAPostToAGetOnlyFor303) {
  ScriptedServer server([](const std::string& request, int, int) -> std::string {
    if (request.starts_with("POST /see-other ")) return "HTTP/1.1 303 See Other\r\nLocation: /result\r\nContent-Length: 0\r\n\r\n";
    if (request.starts_with("POST /temporary ")) return "HTTP/1.1 307 Temporary\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n";
    if (request.starts_with("GET /result ")) return Ok("got");
    if (request.starts_with("POST /again ") && request.ends_with("body")) return Ok("posted again");
    return "HTTP/1.1 500 Oops\r\nContent-Length: 0\r\n\r\n";
  });
  EXPECT_EQ(Text(HttpRequest(server.url("/see-other"), {}, "POST", "body").body), "got");
  EXPECT_EQ(Text(HttpRequest(server.url("/temporary"), {}, "POST", "body").body), "posted again");
}

TEST(HttpTest, GivesUpOnEndlessRedirects) {
  ScriptedServer server([](const std::string&, int, int) {
    return std::string("HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n");
  });
  EXPECT_THROW(HttpRequest(server.url("/loop"), {}), Error);
}

TEST(HttpTest, TruncatedBodyIsATransientError) {
  ScriptedServer server([](const std::string&, int, int) {
    return std::string("!HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhalf");
  });
  EXPECT_THROW(HttpRequest(server.url("/"), {}), TransientError);
}

TEST(HttpTest, RefusedConnectionIsATransientError) {
  std::uint16_t port;
  {
    ScriptedServer server([](const std::string&, int, int) { return Ok(""); });
    port = static_cast<std::uint16_t>(std::stoi(server.url("").substr(17)));
  }
  EXPECT_THROW(HttpRequest("http://127.0.0.1:" + std::to_string(port) + "/", {}), TransientError);
}

TEST(HttpTest, SilentServerStallsOut) {
  ScriptedServer server([](const std::string&, int, int) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return Ok("too late");
  });
  HttpConfig config;
  config.stall_timeout_seconds = 1;
  auto start = steady_clock::now();
  EXPECT_THROW(HttpRequest(server.url("/"), config), TransientError);
  EXPECT_LT(steady_clock::now() - start, std::chrono::milliseconds(2900));
}

TEST(HttpTest, ProxiesAreRefused) {
  HttpConfig config;
  config.proxy = "http://proxy.test:3128";
  EXPECT_THROW(HttpRequest("http://127.0.0.1:1/", config), Error);
}

TEST(HttpTest, RejectsUnsupportedSchemes) {
  EXPECT_THROW(HttpRequest("ftp://example.test/", {}), Error);
  EXPECT_THROW(HttpRequest("example.test/", {}), Error);
}

TEST(HttpTest, ParsesUrls) {
  Url url = ParseUrl("https://host.test:8443/a/b?c=d#frag");
  EXPECT_TRUE(url.tls);
  EXPECT_EQ(url.host, "host.test");
  EXPECT_EQ(url.port, "8443");
  EXPECT_EQ(url.target, "/a/b?c=d");
  Url plain = ParseUrl("http://[::1]/");
  EXPECT_FALSE(plain.tls);
  EXPECT_EQ(plain.host, "::1");
  EXPECT_EQ(plain.port, "80");
  EXPECT_EQ(ParseUrl("https://host.test?q=1").target, "/?q=1");
}

}  // namespace
}  // namespace pxsteamdl::detail
