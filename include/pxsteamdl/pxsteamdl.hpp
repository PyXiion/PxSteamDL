// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pxsteamdl {

namespace detail {
class Session;
}

struct Progress {
    std::uint64_t item_id;
    std::uint64_t bytes_done;
    std::uint64_t bytes_total;
};

struct Options {
    // Items resolved (manifest fetched, files planned) concurrently.
    unsigned parallel_items = 4;
    // Chunk downloads share one pool of parallel_items * threads_per_item workers across all items.
    unsigned threads_per_item = 8;
    // Called from worker threads.
    std::function<void(const Progress&)> on_progress;
};

struct Result {
    std::uint64_t item_id;
    std::string title;
    std::filesystem::path path;
    std::string error;
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
    std::vector<Result> download(std::span<const std::uint64_t> item_ids,
                                 const std::filesystem::path& root, const Options& options = {});

private:
    std::unique_ptr<detail::Session> session_;
};

} // namespace pxsteamdl
