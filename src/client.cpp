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
    std::vector<detail::Item> items = detail::fetch_items(item_ids);
    std::vector<Result> results(items.size());
    std::vector<detail::ItemJob> jobs;
    std::vector<std::size_t> job_results;

    for (std::size_t i = 0; i < items.size(); ++i) {
        const detail::Item& item = items[i];
        Result& result = results[i];
        result = {item.id, item.title, root / std::to_string(item.id), item.error};
        if (!result.error.empty()) continue;
        try {
            std::filesystem::create_directories(result.path);
        } catch (const std::exception& e) {
            result.error = e.what();
            continue;
        }
        detail::ItemJob& job = jobs.emplace_back(detail::ItemJob{&item, result.path.string(), {}, {}});
        if (options.on_progress) {
            job.progress = [&options, id = item.id](std::uint64_t done, std::uint64_t total) {
                options.on_progress({id, done, total});
            };
        }
        job_results.push_back(i);
    }

    detail::download_items(*session_, jobs, options.parallel_items, options.threads_per_item);
    for (std::size_t j = 0; j < jobs.size(); ++j) results[job_results[j]].error = std::move(jobs[j].error);
    return results;
}

} // namespace pxsteamdl
