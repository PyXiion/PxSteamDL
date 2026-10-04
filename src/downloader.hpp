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

// How far an item is. "Downloaded" counts what comes over the network (encrypted and compressed chunks), "unpacked"
// what is decrypted, decompressed and written; both cover only the data that had to be fetched, so an item that
// is up to date reports zeros.
struct JobProgress {
  std::uint64_t downloaded = 0;
  std::uint64_t downloaded_total = 0;
  std::uint64_t unpacked = 0;
  std::uint64_t unpacked_total = 0;
};

struct ItemJob {
  Item item;
  std::filesystem::path destination;
  // Called from worker threads.
  std::function<void(const JobProgress&)> progress;
  // Set when the item fails.
  std::string error;
  ErrorKind error_kind = ErrorKind::kNone;
  // What this run fetched and wrote; set when the job ends, whether it succeeded or not.
  std::uint64_t downloaded = 0;
  std::uint64_t unpacked = 0;
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
