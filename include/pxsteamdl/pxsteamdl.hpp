// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "pxsteamdl/error.hpp"
#include "pxsteamdl/version.hpp"

namespace pxsteamdl {

// The version of the library, e.g. "1.2.3". PXSTEAMDL_VERSION_MAJOR, _MINOR, _PATCH and _STRING (in
// pxsteamdl/version.hpp, included above) are the version of the headers a program was compiled against.
std::string_view Version();

namespace detail {
class Session;
}

// How far an item is. Both pairs cover only the data that had to be fetched in this run: an item that is already
// up to date reports zeros, and one that changed in a single file only counts that file's chunks.
struct Progress {
  std::uint64_t item_id = 0;
  // Bytes received over the network so far, and how many there will be: the chunks as the CDN serves them,
  // encrypted and compressed. This is what the connection moves, e.g. for a transfer rate.
  std::uint64_t downloaded_bytes = 0;
  std::uint64_t downloaded_total = 0;
  // Bytes decrypted, decompressed and written to disk so far, and how many there will be. This is how much of the
  // item is in place, e.g. for a progress bar.
  std::uint64_t unpacked_bytes = 0;
  std::uint64_t unpacked_total = 0;
  // Workshop title of the item; empty if unknown.
  std::string title;
};

// What Steam says about an item, known before the item's bytes are downloaded. Not an outcome: see Result.
struct ItemInfo {
  std::uint64_t item_id = 0;
  std::string title;
  // Empty if Steam accepted the item. Otherwise why it was rejected (missing from the answer, a non-success
  // result code, unusable details, a failed details request) or "cancelled" if the download was stopped before the
  // item was looked up. Download failures come later, in Result::error.
  std::string error;
  // kNone if error is empty.
  ErrorKind error_kind = ErrorKind::kNone;

  bool ok() const { return error.empty(); }
  bool cancelled() const { return error_kind == ErrorKind::kCancelled; }
};

// The outcome of one item.
struct Result {
  std::uint64_t item_id = 0;
  std::string title;
  // The item's directory, root/<item id>. Set for every item, failed ones included: after a failure it holds the
  // previous copy, if there was one, since files are replaced only when an item completes.
  std::filesystem::path path;
  // Empty on success. The wording is for people and may change; match on error_kind instead. Cancelled items have
  // exactly "cancelled".
  std::string error;
  // kNone on success.
  ErrorKind error_kind = ErrorKind::kNone;
  // What this run fetched for the item: the bytes that came over the network (encrypted, compressed) and the bytes
  // that were decrypted, decompressed and written. Both are 0 for an item that was already up to date, and
  // whatever was done before the failure for one that failed.
  std::uint64_t downloaded_bytes = 0;
  std::uint64_t unpacked_bytes = 0;

  bool ok() const { return error.empty(); }
  bool cancelled() const { return error_kind == ErrorKind::kCancelled; }
};

// Settings of the network connections of a Client.
struct ClientOptions {
  // The proxy for every connection, as libcurl takes it ("http://host:3128", "socks5h://host:1080"). Not set: use
  // what libcurl finds in the environment (https_proxy, all_proxy; no_proxy). Empty: no proxy at all.
  std::optional<std::string> proxy;
  // How long to wait for a connection to be made; at least 1 second.
  std::chrono::seconds connect_timeout{10};
  // A transfer that moves less than 1 byte per second for this long is given up (and retried); at least 1 second.
  std::chrono::seconds stall_timeout{30};
};

struct Options {
  // Items downloaded side by side; the next item starts as soon as one of them has all its chunks under way.
  // Up to twice as many are resolved (manifest fetched, files planned) ahead. At least 1.
  unsigned parallel_items = 2;
  // Chunk downloads share one pool of parallel_items * threads_per_item workers across all items. At least 1.
  unsigned threads_per_item = 4;
  // Called on the download() thread once per item, in order, as soon as its batch of up to 100 items has been
  // answered by Steam (not during the HTTP request) and before that item's bytes are downloaded. Details are looked
  // up while earlier items download, so on_progress calls for earlier items may come first. An exception it throws
  // propagates out of download(), after the items under way have been stopped (they report nothing; the Results are
  // lost).
  std::function<void(const ItemInfo&)> on_resolved;
  // Called from worker threads, one call at a time per item. It must not throw: an exception fails the item it was
  // called for (Result::error is its message, error_kind kOther) and the download goes on with the others.
  std::function<void(const Progress&)> on_progress;
  // Cancellation: once stop is requested, in-flight chunk requests finish, remaining work is skipped and every
  // unfinished item reports Result::error == "cancelled"; completed items stay ok and download() returns normally.
  // As with failures, temporary files are removed and existing files of unfinished items are not replaced.
  std::stop_token stop;
};

class Client {
 public:
  // Logs in anonymously; throws Error (a std::runtime_error) on failure, std::invalid_argument for bad options.
  explicit Client(const ClientOptions& options = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  // A moved-from Client can only be destroyed or assigned to.
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;

  // Downloads each item into root/<item id>/, updating existing copies incrementally.
  // Returns one Result per entry of item_ids, in order. Per-item failures are reported in Result::error (and its
  // kind); thread-safe. A repeated ID is downloaded once: on_resolved is called once for it and every occurrence gets
  // the same Result. Throws std::invalid_argument if parallel_items or threads_per_item is 0.
  std::vector<Result> download(std::span<const std::uint64_t> item_ids, const std::filesystem::path& root,
                               const Options& options = {});

 private:
  std::unique_ptr<detail::Session> m_session;
};

}  // namespace pxsteamdl
