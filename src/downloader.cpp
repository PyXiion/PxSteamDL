// SPDX-License-Identifier: LGPL-3.0-or-later
#include "downloader.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "cdn.hpp"
#include "chunk.hpp"
#include "cm_packet.hpp"
#include "common.hpp"
#include "crypto.hpp"
#include "http.hpp"
#include "manifest.hpp"
#include "paths.hpp"
#include "pending_file.hpp"
#include "proto.hpp"

namespace pxsteamdl::detail {

namespace {

namespace fs = std::filesystem;

constexpr char kManifestRequestCodeMethod[] = "ContentServerDirectory.GetManifestRequestCode#1";
// Manifest format version in CDN manifest URLs.
constexpr int kManifestVersion = 5;
// Planned items allowed per planner thread; bounds how many items hold temporary files at once.
constexpr std::size_t kOpenItemsPerPlanner = 2;

// CMsgClientGetDepotDecryptionKey(Response) field numbers.
namespace depot_key_field {
constexpr std::uint32_t kRequestDepotId = 1;
constexpr std::uint32_t kRequestAppId = 2;
constexpr std::uint32_t kResponseEResult = 1;
constexpr std::uint32_t kResponseKey = 3;
}  // namespace depot_key_field

// CContentServerDirectory_GetManifestRequestCode_Request/Response field numbers.
namespace request_code_field {
constexpr std::uint32_t kAppId = 1;
constexpr std::uint32_t kDepotId = 2;
constexpr std::uint32_t kManifestId = 3;
constexpr std::uint32_t kResponseCode = 1;
}  // namespace request_code_field

// Replaces whatever is at path, unless it is a directory already, with an empty directory.
void EnsureDirectory(const fs::path& path) {
    auto status = fs::symlink_status(path);
    if (fs::is_directory(status)) return;
    if (fs::exists(status)) fs::remove_all(path);
    fs::create_directory(path);
}

// Removes everything under dir whose path relative to root is not in expected.
void Prune(const fs::path& root, const fs::path& dir, const std::set<std::string>& expected) {
    std::vector<fs::path> children;
    for (const auto& entry : fs::directory_iterator(dir)) children.push_back(entry.path());
    for (const fs::path& child : children) {
        if (!expected.contains(PathKey(ToUtf8(child.lexically_relative(root))))) {
            fs::remove_all(child);
        } else if (fs::is_directory(fs::symlink_status(child))) {
            Prune(root, child, expected);
        }
    }
}

// Parents before children.
std::vector<std::string> SortByDepth(const std::set<std::string>& dirs) {
    std::vector<std::string> sorted(dirs.begin(), dirs.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
        auto depth_a = std::count(a.begin(), a.end(), '/');
        auto depth_b = std::count(b.begin(), b.end(), '/');
        return depth_a == depth_b ? a < b : depth_a < depth_b;
    });
    return sorted;
}

void CheckDestination(const fs::path& root) {
    if (fs::is_symlink(fs::symlink_status(root))) Fail("destination must not be a symlink");
    fs::create_directories(root);
}

void CreateSymlink(const fs::path& root, const ManifestFile& file) {
    fs::path path = root / Utf8Path(file.name);
    fs::path target = Utf8Path(file.link_target).make_preferred();
    if (fs::is_symlink(fs::symlink_status(path)) && fs::read_symlink(path) == target) return;
    fs::remove_all(path);
    std::error_code error;
    // Windows distinguishes directory symlinks; elsewhere both calls are the same.
    if (fs::is_directory(path.parent_path() / target)) {
        fs::create_directory_symlink(target, path, error);
    } else {
        fs::create_symlink(target, path, error);
    }
    if (error) {
        std::string message = "cannot create symlink " + file.name + " -> " + file.link_target + ": " + error.message();
#ifdef _WIN32
        message += " (Windows allows symlinks only with Developer Mode enabled or as administrator)";
#endif
        Fail(message);
    }
}

// Downloads an item from before SteamPipe: a single file at a direct URL.
void DownloadLegacy(const ItemJob& job) {
    const Item& item = *job.item;
    std::string filename = item.filename;
    std::replace(filename.begin(), filename.end(), '\\', '/');
    fs::path name = Utf8Path(filename).filename();
    if (name.empty() || name == "." || name == ".." || IsWindowsUnsafeName(ToUtf8(name))) {
        Fail("legacy item has no safe filename");
    }
    CheckDestination(job.destination);
    HttpResponse response = HttpRequest(item.file_url);
    if (response.status != 200) Fail("legacy file: HTTP " + std::to_string(response.status));
    ManifestFile legacy;
    legacy.name = ToUtf8(name);
    legacy.size = response.body.size();
    legacy.sha = Sha1(response.body);  // Commit() verifies what reached the disk against this
    PendingFile output(legacy, job.destination / name, 1);
    output.Write(0, response.body);
    output.Commit(kRegularPerms);
    if (job.progress) job.progress(legacy.size, legacy.size);
}

struct ChunkTask {
    PendingFile* output = nullptr;
    const ManifestChunk* chunk = nullptr;
};

// An item between planning and completion; owned by Downloader::states_.
struct ItemState {
    std::size_t index = 0;
    ItemJob* job = nullptr;
    AesKey key{};
    const std::vector<std::string>* hosts = nullptr;
    std::string chunk_prefix;
    std::vector<ManifestFile> files;
    std::set<std::string> expected;  // PathKey of every path the item consists of
    std::vector<std::unique_ptr<PendingFile>> pending;
    std::vector<ChunkTask> chunks;
    std::size_t next_chunk = 0;  // next to hand out; guarded by Downloader::queue_mutex_
    std::atomic<std::size_t> remaining{0};
    std::atomic<bool> failed{false};
    std::mutex mutex;  // guards done, error and progress calls
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
};

// One Download() call: planners resolve items into chunk tasks, a shared worker pool fetches chunks of all
// items, so a large item keeps every worker busy instead of only its own share.
class Downloader {
public:
    Downloader(Session& session, std::span<ItemJob> jobs, unsigned planners, unsigned workers, std::stop_token stop)
        : session_(session),
          jobs_(jobs),
          states_(jobs.size()),
          planners_(std::clamp<std::size_t>(planners, 1, jobs.size())),
          workers_(std::max(1u, workers)),
          stop_(std::move(stop)) {}

    // Returns when every job has finished or failed.
    void Run() {
        active_planners_ = planners_;
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < workers_; ++i) threads.emplace_back([this, i] { Work(i); });
        for (std::size_t i = 0; i < planners_; ++i) threads.emplace_back([this, i] { PlanItems(i); });
    }

private:
    AesKey DepotKey(std::uint32_t depot, std::uint32_t app_id) {
        std::lock_guard lock(cache_mutex_);
        if (auto it = keys_.find(depot); it != keys_.end()) return it->second;
        Bytes reply = session_.Request(emsg::kClientGetDepotDecryptionKey,
                                       Concat({EncodeUint(depot_key_field::kRequestDepotId, depot),
                                               EncodeUint(depot_key_field::kRequestAppId, app_id)}));
        AesKey key{};
        std::uint64_t eresult = 0;
        bool has_key = false;
        ProtoReader reader(reply);
        while (auto field = reader.Next()) {
            if (field->Is(depot_key_field::kResponseEResult, WireType::kVarint)) {
                eresult = field->integer;
            } else if (field->Is(depot_key_field::kResponseKey, WireType::kLengthDelimited)) {
                if (field->bytes.size() != key.size()) Fail("depot key response: invalid key length");
                std::copy(field->bytes.begin(), field->bytes.end(), key.begin());
                has_key = true;
            }
        }
        if (eresult != kEResultOk || !has_key) Fail("depot key request failed: eresult " + std::to_string(eresult));
        return keys_.emplace(depot, key).first->second;
    }

    const std::vector<std::string>& CdnHosts(std::uint32_t app_id) {
        std::lock_guard lock(cache_mutex_);
        if (auto it = hosts_.find(app_id); it != hosts_.end()) return it->second;
        return hosts_.emplace(app_id, FetchCdnHosts(app_id)).first->second;
    }

    std::uint64_t ManifestRequestCode(const Item& item, std::uint32_t depot) {
        Bytes reply = session_.Rpc(kManifestRequestCodeMethod,
                                   Concat({EncodeUint(request_code_field::kAppId, item.app_id),
                                           EncodeUint(request_code_field::kDepotId, depot),
                                           EncodeUint(request_code_field::kManifestId, item.manifest_id)}));
        std::uint64_t code = 0;
        ProtoReader reader(reply);
        while (auto field = reader.Next()) {
            if (field->Is(request_code_field::kResponseCode, WireType::kVarint)) code = field->integer;
        }
        if (!code) Fail("manifest request code is zero");
        return code;
    }

    void PlanItems(std::size_t planner) {
        for (std::size_t i; (i = next_item_++) < jobs_.size();) {
            {
                std::unique_lock lock(queue_mutex_);
                item_slot_free_.wait(lock, [&] { return open_items_ < kOpenItemsPerPlanner * planners_; });
                ++open_items_;
            }
            bool queued = false;
            try {
                queued = Plan(i, planner);
            } catch (const std::exception& e) {
                jobs_[i].error = e.what();
                states_[i].reset();
            }
            if (!queued) CloseItem();
        }
        std::lock_guard lock(queue_mutex_);
        if (--active_planners_ == 0) queue_ready_.notify_all();
    }

    void CloseItem() {
        std::lock_guard lock(queue_mutex_);
        --open_items_;
        item_slot_free_.notify_all();
    }

    // Returns whether chunk tasks were queued; otherwise the item is already complete.
    bool Plan(std::size_t index, std::size_t planner) {
        ItemJob& job = jobs_[index];
        if (stop_.stop_requested()) Fail(kCancelled);
        const Item& item = *job.item;
        if (job.destination.empty()) Fail("download destination is empty");
        if (!item.file_url.empty() && item.manifest_id == 0) {
            DownloadLegacy(job);
            return false;
        }
        if (!item.manifest_id || !item.app_id) Fail("item has no SteamPipe manifest or app ID");

        states_[index] = std::make_unique<ItemState>();
        ItemState& state = *states_[index];
        state.index = index;
        state.job = &job;
        FetchManifest(state, item, planner);
        PrepareDirectories(state);
        QueueChangedFiles(state);

        if (state.chunks.empty()) {
            if (job.progress) job.progress(0, 0);
            Finish(state);
            return false;
        }
        if (stop_.stop_requested()) Fail(kCancelled);
        state.remaining = state.chunks.size();
        std::lock_guard lock(queue_mutex_);
        ready_.push_back(&state);
        queue_ready_.notify_all();
        return true;
    }

    void FetchManifest(ItemState& state, const Item& item, std::size_t planner) {
        // Workshop content lives in the depot with the app's ID.
        std::uint32_t depot = item.app_id;
        state.key = DepotKey(depot, item.app_id);
        std::uint64_t code = ManifestRequestCode(item, depot);
        state.hosts = &CdnHosts(item.app_id);
        std::string path = "/depot/" + std::to_string(depot) + "/manifest/" + std::to_string(item.manifest_id) + "/" +
                           std::to_string(kManifestVersion) + "/" + std::to_string(code);
        std::size_t host = planner;
        state.files = FetchFromCdn(*state.hosts, path, host, stop_, [&](const Bytes& zip) {
            return ParseManifest(zip, state.key, depot, item.manifest_id);
        });
        state.chunk_prefix = "/depot/" + std::to_string(depot) + "/chunk/";
    }

    // Records the item's paths in state.expected and creates its directories.
    static void PrepareDirectories(ItemState& state) {
        std::set<std::string> declared;  // path keys of the manifest entries
        std::set<std::string> dirs;      // directories to create
        std::set<std::string> dir_keys;
        auto add_directory = [&](const std::string& name) {
            dirs.insert(name);
            dir_keys.insert(PathKey(name));
            state.expected.insert(PathKey(name));
        };
        for (const ManifestFile& file : state.files) {
            if (!declared.insert(PathKey(file.name)).second) Fail("manifest: duplicate path: " + file.name);
            state.expected.insert(PathKey(file.name));
            if (file.IsDirectory()) add_directory(file.name);
            for (fs::path parent = Utf8Path(file.name).parent_path(); !parent.empty(); parent = parent.parent_path()) {
                add_directory(ToUtf8(parent));
            }
        }
        for (const ManifestFile& file : state.files) {
            if (!file.IsDirectory() && dir_keys.contains(PathKey(file.name))) {
                Fail("manifest: file/directory path conflict: " + file.name);
            }
        }

        const fs::path& root = state.job->destination;
        CheckDestination(root);
        for (const std::string& dir : SortByDepth(dirs)) EnsureDirectory(root / Utf8Path(dir));
    }

    // Starts a PendingFile for each regular file that differs from the manifest and lists its chunks.
    static void QueueChangedFiles(ItemState& state) {
        for (const ManifestFile& file : state.files) {
            if (!file.IsRegular()) continue;
            fs::path path = state.job->destination / Utf8Path(file.name);
            if (IsUpToDate(path, file)) {
                if (file.IsExecutable()) fs::permissions(path, kExecPerms, fs::perm_options::add);
                continue;
            }
            auto& output = state.pending.emplace_back(std::make_unique<PendingFile>(file, path, file.chunks.size()));
            for (const ManifestChunk& chunk : file.chunks) {
                state.chunks.push_back({output.get(), &chunk});
                state.total += chunk.original_size;
            }
        }
    }

    // Takes the next chunk to fetch, waiting for one; nullptr once all items are planned and handed out.
    ItemState* NextTask(ChunkTask& task) {
        std::unique_lock lock(queue_mutex_);
        queue_ready_.wait(lock, [&] { return !ready_.empty() || active_planners_ == 0; });
        if (ready_.empty()) return nullptr;
        ItemState* item = ready_.front();
        ready_.pop_front();
        task = item->chunks[item->next_chunk++];
        // Back into the rotation window; the state lives until its last handed-out chunk completes.
        if (item->next_chunk < item->chunks.size()) {
            ready_.insert(ready_.begin() + std::min(planners_ - 1, ready_.size()), item);
        }
        return item;
    }

    void Work(std::size_t worker) {
        std::size_t host = worker;  // each worker sticks to one CDN host and moves on only after a failure
        ChunkTask task;
        while (ItemState* item = NextTask(task)) {
            ItemState& state = *item;
            if (!state.failed) {
                try {
                    FetchChunk(state, task, host);
                    std::lock_guard lock(state.mutex);
                    state.done += task.chunk->original_size;
                    if (state.job->progress) state.job->progress(state.done, state.total);
                } catch (const std::exception& e) {
                    std::lock_guard lock(state.mutex);
                    if (!state.failed) state.error = e.what();
                    state.failed = true;
                }
            }
            if (--state.remaining == 0) {
                Finish(state);
                CloseItem();
            }
        }
    }

    void FetchChunk(const ItemState& state, const ChunkTask& task, std::size_t& host) const {
        const ManifestChunk& chunk = *task.chunk;
        Bytes data = FetchFromCdn(*state.hosts, state.chunk_prefix + ToHex(chunk.sha), host, stop_,
                                  [&](const Bytes& body) { return DecodeChunk(body, state.key, chunk); });
        task.output->Write(chunk.offset, data);  // a local write error is not worth retrying on another host
    }

    // Runs on the thread that completed the item's last chunk (or its planner when nothing was needed).
    void Finish(ItemState& state) {
        if (!state.failed) {
            try {
                Finalize(state);
            } catch (const std::exception& e) {
                state.error = e.what();
                state.failed = true;
            }
        }
        if (state.failed) state.job->error = state.error;
        states_[state.index].reset();
    }

    // Moves the downloaded files into place, recreates symlinks and removes what the manifest does not list.
    static void Finalize(ItemState& state) {
        for (const auto& output : state.pending) {
            output->Commit(output->file().IsExecutable() ? kRegularPerms | kExecPerms : kRegularPerms);
        }
        const fs::path& root = state.job->destination;
        for (const ManifestFile& file : state.files) {
            if (file.IsSymlink()) CreateSymlink(root, file);
        }
        Prune(root, root, state.expected);
    }

    Session& session_;
    std::span<ItemJob> jobs_;
    std::vector<std::unique_ptr<ItemState>> states_;
    std::size_t planners_;
    std::size_t workers_;
    std::stop_token stop_;
    std::atomic<std::size_t> next_item_{0};

    std::mutex cache_mutex_;  // guards keys_ and hosts_
    std::map<std::uint32_t, AesKey> keys_;
    std::map<std::uint32_t, std::vector<std::string>> hosts_;

    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::condition_variable item_slot_free_;
    // Items with chunks left to hand out. Workers rotate over the first planners_ of them, so parallel_items items
    // download side by side and the next one starts as soon as one of them has no chunks left, instead of queueing
    // behind every chunk of the others.
    std::deque<ItemState*> ready_;
    std::size_t active_planners_ = 0;
    std::size_t open_items_ = 0;  // planned but not finished; bounds open temporary files
};

}  // namespace

void DownloadItems(Session& session, std::span<ItemJob> jobs, unsigned parallel_items, unsigned threads_per_item,
                   std::stop_token stop) {
    if (jobs.empty()) return;
    unsigned workers = std::max(1u, parallel_items) * std::max(1u, threads_per_item);
    Downloader(session, jobs, parallel_items, workers, std::move(stop)).Run();
}

}  // namespace pxsteamdl::detail
