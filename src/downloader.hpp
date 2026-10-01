// SPDX-License-Identifier: LGPL-3.0-or-later
// Downloading workshop items into directories.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

#include "session.hpp"
#include "workshop.hpp"

namespace pxsteamdl::detail {

struct ItemJob {
  Item item;
  std::filesystem::path destination;
  // Called with (bytes done, bytes total) from worker threads.
  std::function<void(std::uint64_t, std::uint64_t)> progress;
  // Set when the item fails.
  std::string error;
};

// Downloads jobs as they are added, while the caller is still looking up further items. Up to parallel_items items
// are resolved concurrently, and their chunks share a pool of parallel_items * threads_per_item workers, which start
// in the constructor. Once stop is requested, unfinished jobs fail with kCancelled.
class Downloader {
 public:
  // max_jobs bounds the number of add() calls.
  Downloader(Session& session, std::size_t max_jobs, unsigned parallel_items, unsigned threads_per_item,
             std::stop_token stop);
  // Without a finish() call (e.g. while an exception unwinds), stops the jobs and waits for the threads.
  ~Downloader();
  Downloader(const Downloader&) = delete;
  Downloader& operator=(const Downloader&) = delete;

  // Queues a job, which must stay alive and unchanged until finish() returns.
  void add(ItemJob& job);

  // Declares that no more jobs follow and waits until every added job has finished or failed.
  void finish();

 private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace pxsteamdl::detail
