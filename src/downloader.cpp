// SPDX-License-Identifier: LGPL-3.0-or-later
#include "downloader.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "async.hpp"
#include "cdn.hpp"
#include "chunk.hpp"
#include "cm_packet.hpp"
#include "common.hpp"
#include "crypto.hpp"
#include "directory_update.hpp"
#include "eresult.hpp"
#include "http.hpp"
#include "manifest.hpp"
#include "paths.hpp"
#include "pending_file.hpp"
#include "proto.hpp"
#include "retry.hpp"

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
    Fail(ErrorKind::kFilesystem, message);
  }
}

// Downloads an item from before SteamPipe: a single file at a direct URL. The disk work runs on pool.
asio::awaitable<void> DownloadLegacy(ItemJob& job, const HttpConfig& config, std::stop_token stop,
                                     asio::thread_pool& pool) {
  const Item& item = job.item;
  std::string filename = item.filename;
  std::replace(filename.begin(), filename.end(), '\\', '/');
  fs::path name = Utf8Path(filename).filename();
  if (name.empty() || name == "." || name == ".." || IsWindowsUnsafeName(ToUtf8(name))) {
    Fail("legacy item has no safe filename");
  }
  std::unique_ptr<DirectoryUpdate> directory =
      co_await OnPool(pool, [&] { return std::make_unique<DirectoryUpdate>(job.destination, stop); });
  HttpResponse response = co_await RetryAsync(stop, [&]() -> asio::awaitable<HttpResponse> {
    HttpResponse reply = co_await HttpRequestAsync(item.file_url, config);
    CheckHttpStatus("legacy file", reply.status);
    co_return reply;
  });
  job.downloaded = response.body.size();
  if (stop.stop_requested()) FailCancelled();
  co_await OnPool(pool, [&] {
    ManifestFile legacy;
    legacy.name = ToUtf8(name);
    legacy.size = response.body.size();
    legacy.sha = Sha1(response.body);  // commit() verifies what reached the disk against this
    directory->copyPrevious();
    PendingFile output(legacy, directory->staging() / name, 1);
    output.write(0, response.body);
    output.commit(kRegularPerms);
    job.unpacked = legacy.size;
    if (job.progress) job.progress({job.downloaded, job.downloaded, job.unpacked, job.unpacked});
    directory->commit(stop);
    directory.reset();
  });
}

struct ChunkTask {
  PendingFile* output = nullptr;
  const ManifestChunk* chunk = nullptr;
};

// An item between planning and completion; owned by Downloader::Impl::m_states.
struct ItemState {
  std::size_t index = 0;
  ItemJob* job = nullptr;
  AesKey key{};
  const std::vector<std::string>* hosts = nullptr;
  std::string chunk_prefix;
  std::vector<ManifestFile> files;
  // Declared before pending so temporary files are closed before the workspace and lock are released.
  std::unique_ptr<DirectoryUpdate> directory;
  std::vector<std::unique_ptr<PendingFile>> pending;
  std::vector<ChunkTask> chunks;
  std::size_t next_chunk = 0;  // next to hand out; guarded by Downloader::m_queueMutex
  std::atomic<std::size_t> remaining{0};
  std::atomic<bool> failed{false};
  std::mutex mutex;  // guards progress, error and the calls of the progress callback
  JobProgress progress;
  std::string error;
  ErrorKind error_kind = ErrorKind::kNone;
};

}  // namespace

// Planners resolve items into chunk tasks as they are added, and a shared set of workers fetches chunks of all items,
// so a large item keeps every worker busy instead of only its own share. Planners and workers are coroutines on the
// session's io thread: a waiting one costs a coroutine frame, not a thread. What blocks (file locks, hashing,
// decrypting, decompressing, writing) runs on a small thread pool and hops back to the io thread afterwards, so all
// scheduling state below is touched by the io thread alone and needs no lock.
class Downloader::Impl {
 public:
  Impl(Session& session, std::size_t max_jobs, unsigned planners, unsigned workers, std::stop_token stop)
      : m_session(session),
        m_io(session.executor()),
        m_maxJobs(max_jobs),
        m_states(max_jobs),
        m_planners(std::clamp<std::size_t>(planners, 1, std::max<std::size_t>(max_jobs, 1))),
        m_workers(std::max(1u, workers)),
        m_stopForward(stop, [this] { m_abort.request_stop(); }),
        m_stop(m_abort.get_token()),
        m_cacheMutex(m_io),
        m_planWake(m_io),
        m_queueReady(m_io),
        m_pool(PoolSize(std::max(1u, workers))) {
    m_activePlanners = m_planners;
    m_running = m_workers + m_planners;
    for (std::size_t i = 0; i < m_workers; ++i) spawn([this, i] { return work(i); });
    for (std::size_t i = 0; i < m_planners; ++i) spawn([this, i] { return planItems(i); });
  }

  ~Impl() {
    if (!m_finished) {
      m_abort.request_stop();
      finish();
    }
  }

  void add(ItemJob& job) {
    if (m_accepted.fetch_add(1) >= m_maxJobs) {
      --m_accepted;
      Fail(ErrorKind::kOther, "Downloader: more jobs than announced");
    }
    asio::post(m_io, [this, &job] {
      m_incoming.push_back(&job);
      m_planWake.notify();
    });
  }

  void finish() {
    if (m_finished) return;
    asio::post(m_io, [this] {
      m_closed = true;
      m_planWake.notify();
    });
    m_done.get_future().get();
    m_finished = true;
  }

 private:
  // Threads for the work that blocks: about one per core, as many as there are workers at most.
  static unsigned PoolSize(unsigned workers) {
    unsigned cores = std::thread::hardware_concurrency();
    return std::max(2u, std::min(workers, cores == 0 ? 4u : cores));
  }

  // Runs a planner or worker to its end; the last one to end releases finish().
  template <class Loop>
  void spawn(Loop loop) {
    asio::co_spawn(
        m_io,
        [this, loop = std::move(loop)]() -> asio::awaitable<void> {
          try {
            co_await loop();
          } catch (...) {
            // Loops report failures through their jobs; nothing may escape into the io thread.
          }
          if (--m_running == 0) m_done.set_value();
        },
        asio::detached);
  }

  asio::awaitable<AesKey> depotKey(std::uint32_t depot, std::uint32_t app_id) {
    co_await m_cacheMutex.lock();
    AsyncMutex::Guard unlock(m_cacheMutex);
    if (auto it = m_keys.find(depot); it != m_keys.end()) co_return it->second;
    AesKey key = co_await RetryAsync(m_stop, [&]() -> asio::awaitable<AesKey> {
      co_return co_await requestDepotKey(depot, app_id);
    });
    co_return m_keys.emplace(depot, key).first->second;
  }

  asio::awaitable<AesKey> requestDepotKey(std::uint32_t depot, std::uint32_t app_id) {
    Bytes reply = co_await m_session.request(emsg::kClientGetDepotDecryptionKey,
                                             Concat({EncodeUint(depot_key_field::kRequestDepotId, depot),
                                                     EncodeUint(depot_key_field::kRequestAppId, app_id)}));
    AesKey key{};
    std::int64_t eresult = 0;
    bool has_key = false;
    ProtoReader reader(reply);
    while (auto field = reader.next()) {
      if (field->is(depot_key_field::kResponseEResult, WireType::kVarint)) {
        eresult = static_cast<std::int64_t>(field->integer);
      } else if (field->is(depot_key_field::kResponseKey, WireType::kLengthDelimited)) {
        if (field->bytes.size() != key.size()) Fail("depot key response: invalid key length");
        std::copy(field->bytes.begin(), field->bytes.end(), key.begin());
        has_key = true;
      }
    }
    if (eresult != kEResultOk) FailEResult("depot key request failed", eresult);
    if (!has_key) Fail("depot key response lacks the key");
    co_return key;
  }

  asio::awaitable<const std::vector<std::string>*> cdnHosts(std::uint32_t app_id) {
    co_await m_cacheMutex.lock();
    AsyncMutex::Guard unlock(m_cacheMutex);
    if (auto it = m_hosts.find(app_id); it != m_hosts.end()) co_return &it->second;
    std::vector<std::string> hosts = co_await FetchCdnHosts(app_id, m_session.httpConfig(), m_stop);
    co_return &m_hosts.emplace(app_id, std::move(hosts)).first->second;
  }

  asio::awaitable<std::uint64_t> manifestRequestCode(const Item& item, std::uint32_t depot) {
    Bytes request =
        Concat({EncodeUint(request_code_field::kAppId, item.app_id), EncodeUint(request_code_field::kDepotId, depot),
                EncodeUint(request_code_field::kManifestId, item.manifest_id)});
    Bytes reply = co_await RetryAsync(m_stop, [&]() -> asio::awaitable<Bytes> {
      co_return co_await m_session.rpc(kManifestRequestCodeMethod, request);
    });
    std::uint64_t code = 0;
    ProtoReader reader(reply);
    while (auto field = reader.next()) {
      if (field->is(request_code_field::kResponseCode, WireType::kVarint)) code = field->integer;
    }
    if (!code) Fail(ErrorKind::kRejected, "manifest request code is zero");
    co_return code;
  }

  // Takes the next added job once fewer than kOpenItemsPerPlanner * m_planners items are open; nullptr once no
  // more jobs follow.
  asio::awaitable<ItemJob*> nextJob(std::size_t& index) {
    while (m_incoming.empty() ? !m_closed : m_openItems >= kOpenItemsPerPlanner * m_planners) {
      co_await m_planWake.wait();
    }
    if (m_incoming.empty()) co_return nullptr;
    ItemJob* job = m_incoming.front();
    m_incoming.pop_front();
    index = m_added++;
    ++m_openItems;
    co_return job;
  }

  asio::awaitable<void> planItems(std::size_t planner) {
    std::size_t index = 0;
    while (ItemJob* job = co_await nextJob(index)) {
      bool queued = false;
      bool failed = false;
      try {
        queued = co_await plan(*job, index, planner);
      } catch (const std::exception& e) {
        job->error = e.what();
        job->error_kind = KindOf(e);
        failed = true;
      }
      if (failed) co_await release(index);
      if (!queued) closeItem();
    }
    if (--m_activePlanners == 0) m_queueReady.notify();
  }

  void closeItem() {
    --m_openItems;
    m_planWake.notify();
  }

  // Destroys the item's state (removing its temporary files and workspace) away from the io thread.
  asio::awaitable<void> release(std::size_t index) {
    if (m_states[index]) co_await OnPool(m_pool, [&] { m_states[index].reset(); });
  }

  // Returns whether chunk tasks were queued; otherwise the item is already complete.
  asio::awaitable<bool> plan(ItemJob& job, std::size_t index, std::size_t planner) {
    if (m_stop.stop_requested()) FailCancelled();
    const Item& item = job.item;
    if (job.destination.empty()) Fail(ErrorKind::kOther, "download destination is empty");
    if (!item.file_url.empty() && item.manifest_id == 0) {
      co_await DownloadLegacy(job, m_session.httpConfig(), m_stop, m_pool);
      co_return false;
    }
    if (!item.manifest_id || !item.app_id) Fail(ErrorKind::kRejected, "item has no SteamPipe manifest or app ID");

    m_states[index] = std::make_unique<ItemState>();
    ItemState& state = *m_states[index];
    state.index = index;
    state.job = &job;
    state.directory = co_await OnPool(m_pool, [&] { return std::make_unique<DirectoryUpdate>(job.destination, m_stop); });
    co_await fetchManifest(state, item, planner);
    co_await OnPool(m_pool, [&] {
      prepareDirectories(state);
      queueChangedFiles(state);
    });

    if (state.chunks.empty()) {
      co_await OnPool(m_pool, [&] {
        if (job.progress) job.progress({});
      });
      co_await finish(state);
      co_return false;
    }
    if (m_stop.stop_requested()) FailCancelled();
    state.remaining = state.chunks.size();
    m_ready.push_back(&state);
    m_queueReady.notify();
    co_return true;
  }

  asio::awaitable<void> fetchManifest(ItemState& state, const Item& item, std::size_t planner) {
    // Workshop content lives in the depot with the app's ID.
    std::uint32_t depot = item.app_id;
    state.key = co_await depotKey(depot, item.app_id);
    std::uint64_t code = co_await manifestRequestCode(item, depot);
    state.hosts = co_await cdnHosts(item.app_id);
    std::string path = "/depot/" + std::to_string(depot) + "/manifest/" + std::to_string(item.manifest_id) + "/" +
                       std::to_string(kManifestVersion) + "/" + std::to_string(code);
    std::size_t host = planner;
    state.files = co_await FetchFromCdn(
        *state.hosts, path, host, m_session.httpConfig(), m_stop,
        [&](Bytes zip) -> asio::awaitable<std::vector<ManifestFile>> {
          co_return co_await OnPool(m_pool, [&] { return ParseManifest(zip, state.key, depot, item.manifest_id); });
        });
    state.chunk_prefix = "/depot/" + std::to_string(depot) + "/chunk/";
  }

  // Validates the item's paths and creates directories in its private workspace.
  static void prepareDirectories(ItemState& state) {
    std::set<std::string> declared;  // path keys of the manifest entries
    std::set<std::string> dirs;      // directories to create
    std::set<std::string> dir_keys;
    auto add_directory = [&](const std::string& name) {
      dirs.insert(name);
      dir_keys.insert(PathKey(name));
    };
    for (const ManifestFile& file : state.files) {
      if (!declared.insert(PathKey(file.name)).second) Fail("manifest: duplicate path: " + file.name);
      if (file.isDirectory()) add_directory(file.name);
      for (fs::path parent = Utf8Path(file.name).parent_path(); !parent.empty(); parent = parent.parent_path()) {
        add_directory(ToUtf8(parent));
      }
    }
    for (const ManifestFile& file : state.files) {
      if (!file.isDirectory() && dir_keys.contains(PathKey(file.name))) {
        Fail("manifest: file/directory path conflict: " + file.name);
      }
    }

    const fs::path& root = state.directory->staging();
    for (const std::string& dir : SortByDepth(dirs)) fs::create_directory(root / Utf8Path(dir));
  }

  // Starts a PendingFile for each regular file that differs from the manifest and lists its chunks.
  static void queueChangedFiles(ItemState& state) {
    for (const ManifestFile& file : state.files) {
      if (!file.isRegular()) continue;
      fs::path path = state.job->destination / Utf8Path(file.name);
      if (state.directory->hasRegularFile(file.name) && IsUpToDate(path, file)) {
        state.directory->reuseFile(file.name, file.isExecutable());
        continue;
      }
      fs::path staged = state.directory->staging() / Utf8Path(file.name);
      auto& output = state.pending.emplace_back(std::make_unique<PendingFile>(file, staged, file.chunks.size()));
      for (const ManifestChunk& chunk : file.chunks) {
        state.chunks.push_back({output.get(), &chunk});
        state.progress.downloaded_total += chunk.compressed_size;
        state.progress.unpacked_total += chunk.original_size;
      }
    }
  }

  // Takes the next chunk to fetch, waiting for one; nullptr once all items are planned and handed out.
  asio::awaitable<ItemState*> nextTask(ChunkTask& task) {
    while (m_ready.empty() && m_activePlanners != 0) co_await m_queueReady.wait();
    if (m_ready.empty()) co_return nullptr;
    ItemState* item = m_ready.front();
    m_ready.pop_front();
    task = item->chunks[item->next_chunk++];
    // Back into the rotation window; the state lives until its last handed-out chunk completes.
    if (item->next_chunk < item->chunks.size()) {
      m_ready.insert(m_ready.begin() + std::min(m_planners - 1, m_ready.size()), item);
    }
    co_return item;
  }

  asio::awaitable<void> work(std::size_t worker) {
    std::size_t host = worker;  // each worker sticks to one CDN host and moves on only after a failure
    ChunkTask task;
    while (ItemState* item = co_await nextTask(task)) {
      ItemState& state = *item;
      if (!state.failed) {
        try {
          co_await fetchChunk(state, task, host);
        } catch (const std::exception& e) {
          if (!state.failed) {
            state.error = e.what();
            state.error_kind = KindOf(e);
          }
          state.failed = true;
        }
      }
      if (--state.remaining == 0) {
        co_await finish(state);
        closeItem();
      }
    }
  }

  // Fetches a chunk, then decrypts, decompresses and writes it on the pool, and reports its progress.
  asio::awaitable<void> fetchChunk(ItemState& state, const ChunkTask& task, std::size_t& host) {
    const ManifestChunk& chunk = *task.chunk;
    std::uint64_t downloaded = 0;
    // A body that fails to decode is fetched from another host; a failure to write it or to report it is not.
    std::exception_ptr late_error;
    co_await FetchFromCdn(*state.hosts, state.chunk_prefix + ToHex(chunk.sha), host, m_session.httpConfig(), m_stop,
                          [&](Bytes body) -> asio::awaitable<bool> {
                            downloaded = body.size();
                            co_return co_await OnPool(m_pool, [&]() -> bool {
                              Bytes data = DecodeChunk(body, state.key, chunk);
                              try {
                                task.output->write(chunk.offset, data);
                                std::lock_guard lock(state.mutex);
                                state.progress.downloaded += downloaded;
                                state.progress.unpacked += chunk.original_size;
                                if (state.job->progress) state.job->progress(state.progress);
                              } catch (...) {
                                late_error = std::current_exception();
                              }
                              return true;
                            });
                          });
    if (late_error) std::rethrow_exception(late_error);
  }

  // Runs when the item's last chunk is done (or its planner when nothing was needed).
  asio::awaitable<void> finish(ItemState& state) {
    if (!state.failed) {
      try {
        co_await OnPool(m_pool, [&] { finalize(state); });
      } catch (const std::exception& e) {
        state.error = e.what();
        state.error_kind = KindOf(e);
        state.failed = true;
      }
    }
    state.job->downloaded = state.progress.downloaded;
    state.job->unpacked = state.progress.unpacked;
    if (state.failed) {
      state.job->error = state.error;
      state.job->error_kind = state.error_kind;
    }
    co_await release(state.index);
  }

  // Finishes and verifies every file in staging before replacing any part of the previous copy.
  void finalize(ItemState& state) const {
    if (m_stop.stop_requested()) FailCancelled();
    for (const auto& output : state.pending) {
      output->commit(output->file().isExecutable() ? kRegularPerms | kExecPerms : kRegularPerms);
    }
    const fs::path& root = state.directory->staging();
    for (const ManifestFile& file : state.files) {
      if (file.isSymlink()) CreateSymlink(root, file);
    }
    state.directory->commit(m_stop);
  }

  Session& m_session;
  asio::any_io_executor m_io;
  std::size_t m_maxJobs;
  std::atomic<std::size_t> m_accepted{0};  // add() calls, checked on the calling thread
  // Indexed in the order jobs are taken by planners.
  std::vector<std::unique_ptr<ItemState>> m_states;
  std::size_t m_planners;
  std::size_t m_workers;
  // Stopped on the caller's request, or when the downloader is destroyed without finish().
  std::stop_source m_abort;
  std::stop_callback<std::function<void()>> m_stopForward;
  std::stop_token m_stop;

  AsyncMutex m_cacheMutex;  // guards m_keys and m_hosts
  std::map<std::uint32_t, AesKey> m_keys;
  std::map<std::uint32_t, std::vector<std::string>> m_hosts;

  // Everything below is touched on the io thread only.
  Signal m_planWake;
  Signal m_queueReady;
  // Added jobs no planner has taken yet.
  std::deque<ItemJob*> m_incoming;
  std::size_t m_added = 0;  // jobs taken by planners
  bool m_closed = false;    // no more jobs follow
  // Items with chunks left to hand out. Workers rotate over the first m_planners of them, so parallel_items items
  // download side by side and the next one starts as soon as one of them has no chunks left, instead of queueing
  // behind every chunk of the others.
  std::deque<ItemState*> m_ready;
  std::size_t m_activePlanners = 0;
  std::size_t m_openItems = 0;  // planned but not finished; bounds open temporary files
  std::size_t m_running = 0;    // planner and worker coroutines still going
  std::promise<void> m_done;    // set when the last of them has ended

  bool m_finished = false;
  // Last, so its threads are joined before the state they use is destroyed.
  asio::thread_pool m_pool;
};

Downloader::Downloader(Session& session, std::size_t max_jobs, unsigned parallel_items, unsigned threads_per_item,
                       std::stop_token stop)
    : m_impl(std::make_unique<Impl>(session, max_jobs, parallel_items,
                                    std::max(1u, parallel_items) * std::max(1u, threads_per_item), std::move(stop))) {}

Downloader::~Downloader() = default;

void Downloader::add(ItemJob& job) { m_impl->add(job); }

void Downloader::finish() { m_impl->finish(); }

}  // namespace pxsteamdl::detail
