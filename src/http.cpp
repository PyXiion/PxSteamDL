// SPDX-License-Identifier: LGPL-3.0-or-later
#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

#include "net.hpp"
#include "pxsteamdl/version.hpp"

namespace pxsteamdl::detail {

namespace {

constexpr int kMaxRedirects = 10;
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::uint64_t kMaxBodyBytes = std::uint64_t{2} << 30;

struct ParsedHead {
  int minor_version = 1;
  long status = 0;
  std::string connection;
  std::string location;
  std::string transfer_encoding;
  bool has_length = false;
  std::uint64_t content_length = 0;
};

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

ParsedHead ParseHead(const std::string& head) {
  ParsedHead parsed;
  std::size_t line_end = head.find("\r\n");
  std::string_view status_line(head.data(), line_end);
  // "HTTP/1.1 200 OK"
  if (!status_line.starts_with("HTTP/1.") || status_line.size() < 12) {
    FailTransient("malformed HTTP status line: " + std::string(status_line.substr(0, 40)));
  }
  parsed.minor_version = status_line[7] - '0';
  std::from_chars(status_line.data() + 9, status_line.data() + 12, parsed.status);
  if (parsed.status < 100) FailTransient("malformed HTTP status line");
  std::size_t pos = line_end + 2;
  while (pos < head.size()) {
    std::size_t end = head.find("\r\n", pos);
    if (end == std::string::npos || end == pos) break;
    std::string_view line(head.data() + pos, end - pos);
    pos = end + 2;
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string name = Lower(line.substr(0, colon));
    std::string_view value = Trim(line.substr(colon + 1));
    if (name == "content-length") {
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed.content_length);
      parsed.has_length = ec == std::errc();
    } else if (name == "transfer-encoding") {
      parsed.transfer_encoding = Lower(value);
    } else if (name == "connection") {
      parsed.connection = Lower(value);
    } else if (name == "location") {
      parsed.location = std::string(value);
    }
  }
  return parsed;
}

// Reads one chunk-size line; ignores chunk extensions.
asio::awaitable<std::uint64_t> ReadChunkSize(Connection& connection) {
  std::string line = co_await connection.readUntil("\r\n", 1024);
  std::uint64_t size = 0;
  auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), size, 16);
  if (ec != std::errc()) FailTransient("malformed chunk size");
  co_return size;
}

struct Exchange {
  HttpResponse response;
  std::string location;
  bool keep_alive = false;
};

asio::awaitable<Exchange> RoundTrip(Connection& connection, const Url& url, const std::string& method,
                                    const std::string& body, const std::string& content_type) {
  std::string request = method + " " + url.target + " HTTP/1.1\r\nHost: " + url.host;
  if ((url.tls && url.port != "443") || (!url.tls && url.port != "80")) request += ":" + url.port;
  request += "\r\nUser-Agent: pxsteamdl/" PXSTEAMDL_VERSION_STRING "\r\nAccept: */*\r\n";
  if (method == "POST") {
    if (!content_type.empty()) request += "Content-Type: " + content_type + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  if (method == "POST") request += body;
  co_await connection.writeAll(reinterpret_cast<const std::uint8_t*>(request.data()), request.size());

  ParsedHead head;
  do {
    head = ParseHead(co_await connection.readUntil("\r\n\r\n", kMaxHeaderBytes));
  } while (head.status >= 100 && head.status < 200);

  Exchange exchange;
  exchange.response.status = head.status;
  exchange.location = std::move(head.location);
  Bytes& out = exchange.response.body;
  bool read_to_close = false;
  if (head.status == 204 || head.status == 304) {
    // no body
  } else if (head.transfer_encoding.find("chunked") != std::string::npos) {
    for (;;) {
      std::uint64_t size = co_await ReadChunkSize(connection);
      if (size == 0) break;
      if (out.size() + size > kMaxBodyBytes) Fail(ErrorKind::kRejected, "response body too large");
      std::size_t at = out.size();
      out.resize(at + size);
      co_await connection.readExact(out.data() + at, size);
      std::uint8_t crlf[2];
      co_await connection.readExact(crlf, 2);
    }
    for (;;) {  // trailers, ended by an empty line
      std::string line = co_await connection.readUntil("\r\n", kMaxHeaderBytes);
      if (line.size() == 2) break;
    }
  } else if (head.has_length) {
    if (head.content_length > kMaxBodyBytes) Fail(ErrorKind::kRejected, "response body too large");
    out.resize(head.content_length);
    co_await connection.readExact(out.data(), out.size());
  } else {
    read_to_close = true;
    std::size_t at = 0;
    try {
      for (;;) {
        at = out.size();
        out.resize(at + 64 * 1024);
        std::size_t count = co_await connection.readSome(out.data() + at, 64 * 1024);
        out.resize(at + count);
      }
    } catch (const TransientError&) {
      if (connection.timedOut()) throw;  // a stall, not the end of the body
      out.resize(at);
    }
  }
  exchange.keep_alive = !read_to_close && head.minor_version >= 1 && head.connection.find("close") == std::string::npos;
  co_return exchange;
}

// One request without redirects. A reused keep-alive connection that the server dropped meanwhile is replaced.
asio::awaitable<Exchange> Send(const std::shared_ptr<Net>& net, const HttpConfig& config, const Url& url,
                               const std::string& method, const std::string& body, const std::string& content_type) {
  std::string origin = url.origin();
  for (;;) {
    std::shared_ptr<Connection> connection = net->takeIdle(origin);
    bool reused = connection != nullptr;
    if (!reused) connection = co_await Connection::open(net, url, std::chrono::seconds(config.connect_timeout_seconds));
    connection->markRequestStart();
    connection->arm(std::chrono::seconds(config.stall_timeout_seconds));
    bool stale = false;
    Exchange exchange;
    try {
      exchange = co_await RoundTrip(*connection, url, method, body, content_type);
    } catch (const TransientError&) {
      connection->close();
      if (!reused || connection->gotBytes()) throw;
      stale = true;
    } catch (...) {
      connection->close();
      throw;
    }
    if (stale) continue;
    if (exchange.keep_alive) {
      net->giveIdle(origin, std::move(connection));
    } else {
      connection->close();
    }
    co_return exchange;
  }
}

std::string ResolveLocation(const Url& base, const std::string& location) {
  if (location.starts_with("https://") || location.starts_with("http://")) return location;
  std::string origin = base.origin();
  if (location.starts_with("/")) return origin + location;
  std::string directory = base.target.substr(0, base.target.find('?'));
  directory.resize(directory.rfind('/') + 1);
  return origin + directory + location;
}

}  // namespace

asio::awaitable<HttpResponse> HttpRequestAsync(std::string url, HttpConfig config, std::string method, std::string body,
                                               std::string content_type) {
  if (config.proxy && !config.proxy->empty()) {
    Fail(ErrorKind::kOther, "the asio backend does not support proxies (ClientOptions::proxy)");
  }
  if (method != "GET" && method != "POST") Fail(ErrorKind::kOther, "HttpRequest: unsupported method " + method);
  std::shared_ptr<Net> net = config.net ? config.net : Net::acquire();
  for (int redirects = 0;; ++redirects) {
    Url parsed = ParseUrl(url);
    Exchange exchange;
    try {
      exchange = co_await Send(net, config, parsed, method, body, content_type);
    } catch (const TransientError& e) {
      FailTransient(method + " " + url + " failed: " + e.what());
    }
    long status = exchange.response.status;
    bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (!redirect || exchange.location.empty()) co_return std::move(exchange.response);
    if (redirects >= kMaxRedirects) Fail(method + " " + url + " failed: too many redirects");
    url = ResolveLocation(parsed, exchange.location);
    if (status != 307 && status != 308 && method == "POST") {
      method = "GET";
      body.clear();
    }
  }
}

HttpResponse HttpRequest(std::string_view url, const HttpConfig& config, std::string_view method, std::string_view body,
                         std::string_view content_type) {
  HttpConfig local = config;
  if (!local.net) local.net = Net::acquire();
  auto executor = local.net->executor();
  return RunBlocking(executor, HttpRequestAsync(std::string(url), std::move(local), std::string(method),
                                                std::string(body), std::string(content_type)));
}

}  // namespace pxsteamdl::detail
