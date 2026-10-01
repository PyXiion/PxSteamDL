// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/pxsteamdl.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include "downloader.hpp"
#include "session.hpp"
#include "workshop.hpp"

namespace pxsteamdl {

Client::Client() : m_session(std::make_unique<detail::Session>()) { m_session->connect(); }

Client::~Client() = default;

std::vector<Result> Client::download(std::span<const std::uint64_t> item_ids, const std::filesystem::path& root,
                                     const Options& options) {
  if (item_ids.empty()) return {};
  // Reserved up front: the downloader keeps pointers to the jobs, and progress callbacks to the titles.
  std::vector<Result> results;
  results.reserve(item_ids.size());
  std::vector<detail::ItemJob> jobs;
  jobs.reserve(item_ids.size());
  std::vector<std::size_t> job_results;  // index into results of each job

  detail::Downloader downloader(*m_session, item_ids.size(), options.parallel_items, options.threads_per_item,
                                options.stop);
  // Each batch of details is queued as soon as Steam answers it, while earlier items download.
  detail::FetchItems(item_ids, options.stop, [&](detail::Item item) {
    if (options.on_resolved) options.on_resolved({item.id, item.title, item.error});
    const Result& result =
        results.emplace_back(Result{item.id, item.title, root / std::to_string(item.id), item.error});
    if (!item.error.empty()) return;
    // The downloader creates the directory, after refusing a symlink in its place.
    detail::ItemJob& job = jobs.emplace_back(detail::ItemJob{std::move(item), result.path, {}, {}});
    if (options.on_progress) {
      job.progress = [&options, &job](std::uint64_t done, std::uint64_t total) {
        options.on_progress({job.item.id, done, total, job.item.title});
      };
    }
    job_results.push_back(results.size() - 1);
    downloader.add(job);
  });
  downloader.finish();

  for (std::size_t j = 0; j < jobs.size(); ++j) results[job_results[j]].error = std::move(jobs[j].error);
  return results;
}

}  // namespace pxsteamdl
