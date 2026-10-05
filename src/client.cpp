// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/pxsteamdl.hpp"

#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common.hpp"
#include "downloader.hpp"
#include "session.hpp"
#include "workshop.hpp"

namespace pxsteamdl {

namespace {

detail::HttpConfig ToHttpConfig(const ClientOptions& options) {
  if (options.connect_timeout < std::chrono::seconds(1)) {
    throw std::invalid_argument("ClientOptions::connect_timeout must be at least 1 second");
  }
  if (options.stall_timeout < std::chrono::seconds(1)) {
    throw std::invalid_argument("ClientOptions::stall_timeout must be at least 1 second");
  }
  detail::HttpConfig config;
  config.proxy = options.proxy;
  config.connect_timeout_seconds = static_cast<long>(options.connect_timeout.count());
  config.stall_timeout_seconds = static_cast<long>(options.stall_timeout.count());
  return config;
}

// The IDs without repeats, in order of first appearance; slots[i] is where item_ids[i] is in the result.
std::vector<std::uint64_t> Unique(std::span<const std::uint64_t> item_ids, std::vector<std::size_t>& slots) {
  std::vector<std::uint64_t> unique;
  std::unordered_map<std::uint64_t, std::size_t> seen;
  slots.reserve(item_ids.size());
  for (std::uint64_t id : item_ids) {
    auto [it, added] = seen.emplace(id, unique.size());
    if (added) unique.push_back(id);
    slots.push_back(it->second);
  }
  return unique;
}

// Downloads item_ids, which have no repeats.
std::vector<Result> DownloadUnique(detail::Session& session, std::span<const std::uint64_t> item_ids,
                                   const std::filesystem::path& root, const Options& options) {
  // Reserved up front: the downloader keeps pointers to the jobs, and progress callbacks to the titles.
  std::vector<Result> results;
  results.reserve(item_ids.size());
  std::vector<detail::ItemJob> jobs;
  jobs.reserve(item_ids.size());
  std::vector<std::size_t> job_results;  // index into results of each job

  detail::Downloader downloader(session, item_ids.size(), options.parallel_items, options.threads_per_item,
                                options.stop);
  // Each batch of details is queued as soon as Steam answers it, while earlier items download.
  detail::FetchItems(item_ids, session.httpConfig(), options.stop, [&](detail::Item item) {
    if (options.on_resolved) options.on_resolved({item.id, item.title, item.error, item.error_kind});
    Result& result = results.emplace_back();
    result.item_id = item.id;
    result.title = item.title;
    result.path = root / std::to_string(item.id);
    result.error = item.error;
    result.error_kind = item.error_kind;
    if (!item.error.empty()) return;
    // The downloader locks the destination and stages the update, after refusing a symlink in its place.
    detail::ItemJob& job = jobs.emplace_back();
    job.item = std::move(item);
    job.destination = result.path;
    if (options.on_progress) {
      job.progress = [&options, &job](const detail::JobProgress& progress) {
        options.on_progress({job.item.id, progress.downloaded, progress.downloaded_total, progress.unpacked,
                             progress.unpacked_total, job.item.title});
      };
    }
    job_results.push_back(results.size() - 1);
    downloader.add(job);
  });
  downloader.finish();

  for (std::size_t j = 0; j < jobs.size(); ++j) {
    Result& result = results[job_results[j]];
    result.error = std::move(jobs[j].error);
    result.error_kind = jobs[j].error_kind;
    result.downloaded_bytes = jobs[j].downloaded;
    result.unpacked_bytes = jobs[j].unpacked;
  }
  return results;
}

}  // namespace

Client::Client(const ClientOptions& options) : m_session(std::make_unique<detail::Session>(ToHttpConfig(options))) {
  m_session->connect();
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

std::vector<Result> Client::download(std::span<const std::uint64_t> item_ids, const std::filesystem::path& root,
                                     const Options& options) {
  if (options.parallel_items < 1) throw std::invalid_argument("Options::parallel_items must be at least 1");
  if (options.threads_per_item < 1) throw std::invalid_argument("Options::threads_per_item must be at least 1");
  if (item_ids.empty()) return {};

  std::vector<std::size_t> slots;
  std::vector<std::uint64_t> unique = Unique(item_ids, slots);
  std::vector<Result> results = DownloadUnique(*m_session, unique, root, options);
  if (unique.size() == item_ids.size()) return results;

  std::vector<Result> expanded;
  expanded.reserve(item_ids.size());
  for (std::size_t slot : slots) expanded.push_back(results[slot]);
  return expanded;
}

}  // namespace pxsteamdl
