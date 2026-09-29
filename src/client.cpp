// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/pxsteamdl.hpp"

#include "internal.hpp"

namespace pxsteamdl {

Client::Client() : session_(std::make_unique<detail::Session>()) {
    session_->connect();
}

Client::~Client() = default;

std::vector<Result> Client::download(std::span<const std::uint64_t> item_ids,
                                     const std::filesystem::path& root, const Options& options) {
    if (item_ids.empty()) return {};
    // Titles are known as soon as Steam answers, before any bytes move.
    std::function<void(const detail::Item&)> on_item;
    if (options.on_resolved) {
        on_item = [&](const detail::Item& item) { options.on_resolved({item.id, item.title, item.error}); };
    }
    std::vector<detail::Item> items = detail::fetch_items(item_ids, on_item);
    std::vector<Result> results(items.size());
    std::vector<detail::ItemJob> jobs;
    std::vector<std::size_t> job_results;

    for (std::size_t i = 0; i < items.size(); ++i) {
        const detail::Item& item = items[i];
        results[i] = {item.id, item.title, root / std::to_string(item.id), item.error};
        if (!item.error.empty()) continue;
        // The downloader creates the directory, after refusing a symlink in its place.
        detail::ItemJob& job = jobs.emplace_back(detail::ItemJob{&item, results[i].path, {}, {}});
        if (options.on_progress) {
            job.progress = [&options, id = item.id, &title = item.title](std::uint64_t done, std::uint64_t total) {
                options.on_progress({id, done, total, title});
            };
        }
        job_results.push_back(i);
    }

    detail::download_items(*session_, jobs, options.parallel_items, options.threads_per_item, options.stop);
    for (std::size_t j = 0; j < jobs.size(); ++j) results[job_results[j]].error = std::move(jobs[j].error);
    return results;
}

} // namespace pxsteamdl
