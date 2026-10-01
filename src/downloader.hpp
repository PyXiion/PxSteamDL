// SPDX-License-Identifier: LGPL-3.0-or-later
// Downloading workshop items into directories.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>

#include "session.hpp"
#include "workshop.hpp"

namespace pxsteamdl::detail {

struct ItemJob {
    const Item* item = nullptr;
    std::filesystem::path destination;
    // Called with (bytes done, bytes total) from worker threads.
    std::function<void(std::uint64_t, std::uint64_t)> progress;
    // Set when the item fails.
    std::string error;
};

// Downloads all jobs: up to parallel_items items are resolved concurrently, and their chunks share a pool of
// parallel_items * threads_per_item workers. Once stop is requested, unfinished jobs fail with kCancelled.
void DownloadItems(Session& session, std::span<ItemJob> jobs, unsigned parallel_items, unsigned threads_per_item,
                   std::stop_token stop);

}  // namespace pxsteamdl::detail
