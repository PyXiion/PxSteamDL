// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "pxsteamdl/version.hpp"

namespace pxsteamdl {

// The version of the library, e.g. "1.2.3". PXSTEAMDL_VERSION_MAJOR, _MINOR, _PATCH and _STRING (in
// pxsteamdl/version.hpp, included above) are the version of the headers a program was compiled against.
std::string_view Version();

namespace detail {
class Session;
}

struct Progress {
  std::uint64_t item_id;
  std::uint64_t bytes_done;
  std::uint64_t bytes_total;
  // Workshop title of the item; empty if unknown.
  std::string title;
};

// What Steam says about an item, known before the item's bytes are downloaded. Not an outcome: see Result.
struct ItemInfo {
  std::uint64_t item_id;
  std::string title;
  // Empty if Steam accepted the item. Otherwise why it was rejected (missing from the answer, a non-success
  // result code, unusable details, a failed details request) or "cancelled" if the download was stopped before the
  // item was looked up. Download failures come later, in Result::error.
  std::string error;
};

struct Result {
  std::uint64_t item_id;
  std::string title;
  std::filesystem::path path;
  std::string error;
};

struct Options {
  // Items downloaded side by side; the next item starts as soon as one of them has all its chunks under way.
  // Up to twice as many are resolved (manifest fetched, files planned) ahead.
  unsigned parallel_items = 2;
  // Chunk downloads share one pool of parallel_items * threads_per_item workers across all items.
  unsigned threads_per_item = 4;
  // Called on the download() thread once per item, in order, as soon as its batch of up to 100 items has been
  // answered by Steam (not during the HTTP request) and before that item's bytes are downloaded. Details are looked
  // up while earlier items download, so on_progress calls for earlier items may come first.
  std::function<void(const ItemInfo&)> on_resolved;
  // Called from worker threads.
  std::function<void(const Progress&)> on_progress;
  // Cancellation: once stop is requested, in-flight chunk requests finish, remaining work is skipped and every
  // unfinished item reports Result::error == "cancelled"; completed items stay ok and download() returns normally.
  // As with failures, temporary files are removed and existing files of unfinished items are not replaced.
  std::stop_token stop;
};

class Client {
 public:
  // Logs in anonymously; throws std::runtime_error on failure.
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Downloads each item into root/<item id>/, updating existing copies incrementally.
  // Per-item failures are reported in Result::error; thread-safe.
  std::vector<Result> download(std::span<const std::uint64_t> item_ids, const std::filesystem::path& root,
                               const Options& options = {});

 private:
  std::unique_ptr<detail::Session> m_session;
};

}  // namespace pxsteamdl
