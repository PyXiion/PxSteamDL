// SPDX-License-Identifier: LGPL-3.0-or-later
// End-to-end tests of Client against a pretend Steam: Web API, CDN and CM session are fakes (see tests/fakes), the
// downloader, retries, manifests, chunk decoding and file handling are the real code.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <gtest/gtest.h>

#include "pxsteamdl/pxsteamdl.hpp"

#include "downloader.hpp"
#include "eresult.hpp"
#include "fake_http.hpp"
#include "fake_session.hpp"
#include "fake_sleep.hpp"
#include "fake_steam.hpp"
#include "retry.hpp"
#include "test_util.hpp"

namespace pxsteamdl {
namespace {

namespace fs = std::filesystem;
using detail::kMaxAttempts;
using detail::testing::ExpectDelayBetween;
using detail::testing::FakeFile;
using detail::testing::FakeItem;
using detail::testing::FakeLegacyItem;
using detail::testing::FakeLink;
using detail::testing::FakeSteam;
using detail::testing::HttpCall;
using detail::testing::RequestKind;
using detail::testing::SessionCall;
using std::chrono::milliseconds;

// Distinct, reproducible text: different seeds give different chunks.
std::string Content(int seed, std::size_t size) {
  std::string text;
  for (std::size_t i = 0; i < size; ++i) text.push_back(static_cast<char>('a' + (seed + i * 7 + i / 13) % 26));
  return text;
}

FakeItem MakeItem(std::uint64_t id, const std::string& title, int seed = 0) {
  FakeItem item;
  item.id = id;
  item.title = title;
  item.chunk_size = 50;
  item.files = {
      {"About/About.xml", Content(seed + 1, 180), false},  // four chunks
      {"Defs/Things.xml", Content(seed + 2, 75), false},   // two chunks
      {"Assemblies/Mod.dll", Content(seed + 3, 10), true},
      {"empty.txt", "", false},
  };
  item.directories = {"Textures"};
  return item;
}

FakeItem MakeTinyItem(std::uint64_t id) {
  FakeItem item;
  item.id = id;
  item.title = "Tiny " + std::to_string(id);
  item.files = {{"a.txt", Content(static_cast<int>(id), 10), false}};
  return item;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void WriteFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
}

// Paths of the regular files under dir, relative and with '/' separators.
std::set<std::string> ListFiles(const fs::path& dir) {
  std::set<std::string> files;
  if (!fs::exists(dir)) return files;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) files.insert(entry.path().lexically_relative(dir).generic_string());
  }
  return files;
}

bool HasTemporaryFiles(const fs::path& dir) {
  for (const std::string& file : ListFiles(dir)) {
    if (file.find(".pxsteamdl.") != std::string::npos) return true;
  }
  return false;
}

std::string ChunkId(const std::string& content, std::size_t chunk_size, std::size_t index) {
  return detail::ToHex(detail::Sha1(detail::AsBytes(content.substr(index * chunk_size, chunk_size))));
}

class ClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    detail::testing::ForgetSleeps();
    std::random_device random;
    m_root = fs::temp_directory_path() / ("pxsteamdl-test-" + std::to_string(random()));
    fs::create_directories(m_root);
  }

  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(m_root, ignored);
  }

  std::vector<Result> download(std::vector<std::uint64_t> ids, const Options& options = {}) {
    Client client;
    return client.download(ids, m_root, options);
  }

  static Options sequential() {
    Options options;
    options.parallel_items = 1;
    options.threads_per_item = 1;
    return options;
  }

  fs::path itemDir(std::uint64_t id) const { return m_root / std::to_string(id); }

  // The item's files, and nothing else, are on disk with the right content.
  void expectOnDisk(const FakeItem& item) const {
    std::set<std::string> expected;
    for (const FakeFile& file : item.files) {
      expected.insert(file.path);
      EXPECT_EQ(ReadFile(itemDir(item.id) / file.path), file.content) << file.path;
    }
    EXPECT_EQ(ListFiles(itemDir(item.id)), expected);
  }

  static std::size_t totalChunks(const FakeItem& item) {
    std::size_t total = 0;
    for (const FakeFile& file : item.files) total += FakeSteam::chunkCount(item, file.path);
    return total;
  }

  FakeSteam m_steam;
  fs::path m_root;
};

TEST_F(ClientTest, DownloadsItemsIntoDirectories) {
  FakeItem alpha = MakeItem(111, "Alpha");
  FakeItem beta = MakeItem(222, "Beta", 5);
  m_steam.addItem(alpha);
  m_steam.addItem(beta);

  std::mutex mutex;
  std::vector<std::uint64_t> resolved;
  std::map<std::uint64_t, std::string> titles;
  std::map<std::uint64_t, std::vector<std::uint64_t>> done_by_item;
  std::map<std::uint64_t, std::uint64_t> total_by_item;
  Options options;
  options.on_resolved = [&](const ItemInfo& info) {
    resolved.push_back(info.item_id);
    titles[info.item_id] = info.title;
    EXPECT_TRUE(info.error.empty());
  };
  options.on_progress = [&](const Progress& progress) {
    std::lock_guard lock(mutex);
    done_by_item[progress.item_id].push_back(progress.unpacked_bytes);
    total_by_item[progress.item_id] = progress.unpacked_total;
    EXPECT_FALSE(progress.title.empty());
  };

  std::vector<Result> results = download({111, 222}, options);

  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].item_id, 111u);
  EXPECT_EQ(results[1].item_id, 222u);
  for (const Result& result : results) {
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.path, itemDir(result.item_id));
  }
  EXPECT_EQ(results[0].title, "Alpha");
  EXPECT_EQ(resolved, (std::vector<std::uint64_t>{111, 222}));
  EXPECT_EQ(titles[222], "Beta");
  expectOnDisk(alpha);
  expectOnDisk(beta);

  for (std::uint64_t id : {111u, 222u}) {
    EXPECT_EQ(total_by_item[id], 265u);  // 180 + 75 + 10
    ASSERT_FALSE(done_by_item[id].empty());
    EXPECT_TRUE(std::is_sorted(done_by_item[id].begin(), done_by_item[id].end()));
    EXPECT_EQ(done_by_item[id].back(), 265u);
  }
  EXPECT_EQ(m_steam.callCount(RequestKind::kChunk), static_cast<int>(2 * totalChunks(alpha)));
  EXPECT_EQ(detail::testing::ConnectCount(), 1);
}

#ifndef _WIN32
TEST_F(ClientTest, SetsTheExecutableBit) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  ASSERT_TRUE(download({111})[0].error.empty());
  auto perms = [&](const char* file) { return fs::status(itemDir(111) / file).permissions(); };
  EXPECT_NE(perms("Assemblies/Mod.dll") & fs::perms::owner_exec, fs::perms::none);
  EXPECT_EQ(perms("About/About.xml") & fs::perms::owner_exec, fs::perms::none);
}

TEST_F(ClientTest, CreatesSymlinks) {
  FakeItem item = MakeItem(111, "Alpha");
  item.links = {FakeLink{"current.xml", "About/About.xml"}};
  m_steam.addItem(item);

  for (int run = 0; run < 2; ++run) {  // the second run finds the link in place
    ASSERT_TRUE(download({111})[0].error.empty()) << run;
    EXPECT_TRUE(fs::is_symlink(itemDir(111) / "current.xml"));
    EXPECT_EQ(fs::read_symlink(itemDir(111) / "current.xml"), fs::path("About/About.xml"));
    EXPECT_EQ(ReadFile(itemDir(111) / "current.xml"), item.files[0].content);
  }
}
#endif

TEST_F(ClientTest, SecondRunDownloadsNothing) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].error.empty());
  int chunks = m_steam.callCount(RequestKind::kChunk);

  std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
  Options options;
  options.on_progress = [&](const Progress& p) { progress.emplace_back(p.unpacked_bytes, p.unpacked_total); };
  std::vector<Result> results = download({111}, options);

  EXPECT_TRUE(results[0].error.empty());
  EXPECT_EQ(m_steam.callCount(RequestKind::kChunk), chunks);
  EXPECT_EQ(progress, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 0}}));
  expectOnDisk(item);
}

TEST_F(ClientTest, RepairsDamagedFilesAndRemovesStrangers) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].error.empty());
  int chunks = m_steam.callCount(RequestKind::kChunk);

  // Same size, different content: only the checksum can tell.
  WriteFile(itemDir(111) / "Defs/Things.xml", std::string(75, 'X'));
  WriteFile(itemDir(111) / "stray.txt", "stray");
  WriteFile(itemDir(111) / "strays/deep/file.txt", "stray");
  ASSERT_TRUE(download({111})[0].error.empty());

  expectOnDisk(item);
  EXPECT_FALSE(fs::exists(itemDir(111) / "strays"));
  EXPECT_EQ(m_steam.callCount(RequestKind::kChunk) - chunks,
            static_cast<int>(FakeSteam::chunkCount(item, "Defs/Things.xml")));
}

TEST_F(ClientTest, UpdatesOnlyChangedFiles) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].error.empty());
  int chunks = m_steam.callCount(RequestKind::kChunk);

  item.files[0].content = Content(99, 130);  // About.xml, now three chunks
  item.files.push_back({"New/Added.txt", Content(98, 20), false});
  item.files.erase(item.files.begin() + 1);  // Things.xml is gone
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].error.empty());

  expectOnDisk(item);
  EXPECT_EQ(m_steam.callCount(RequestKind::kChunk) - chunks, 3 + 1);
}

TEST_F(ClientTest, ReportsItemsSteamDoesNotKnow) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  std::vector<ItemInfo> infos;
  Options options;
  options.on_resolved = [&](const ItemInfo& info) { infos.push_back(info); };

  std::vector<Result> results = download({111, 12345}, options);

  EXPECT_TRUE(results[0].error.empty());
  EXPECT_EQ(results[1].error, "Steam rejected the item: not found (EResult 9)");
  EXPECT_EQ(results[1].error_kind, ErrorKind::kNotFound);
  EXPECT_TRUE(results[0].ok());
  EXPECT_EQ(results[0].error_kind, ErrorKind::kNone);
  EXPECT_FALSE(results[1].ok());
  EXPECT_FALSE(results[1].cancelled());
  ASSERT_EQ(infos.size(), 2u);
  EXPECT_EQ(infos[1].error, results[1].error);
  EXPECT_EQ(infos[1].error_kind, ErrorKind::kNotFound);
  EXPECT_TRUE(infos[0].ok());
  EXPECT_FALSE(infos[1].ok());
  EXPECT_FALSE(fs::exists(itemDir(12345)));
}

TEST_F(ClientTest, DownloadsNothingForNoItems) {
  Client client;
  EXPECT_TRUE(client.download({}, m_root).empty());
  EXPECT_TRUE(m_steam.calls().empty());
}

TEST_F(ClientTest, DownloadsLegacyItems) {
  m_steam.addLegacyItem(FakeLegacyItem{7, "Old mod", "Old.zip", "legacy bytes"});
  std::vector<Result> results = download({7});
  EXPECT_TRUE(results[0].error.empty()) << results[0].error;
  EXPECT_EQ(ReadFile(itemDir(7) / "Old.zip"), "legacy bytes");
  EXPECT_EQ(m_steam.callCount(RequestKind::kManifest), 0);
}

// ---- failures ----

TEST_F(ClientTest, ChunkMissingOnEveryHostFailsOnlyItsItem) {
  FakeItem broken = MakeItem(111, "Broken");
  FakeItem fine = MakeItem(222, "Fine", 5);
  m_steam.addItem(broken);
  m_steam.addItem(fine);
  std::string missing = ChunkId(broken.files[0].content, broken.chunk_size, 1);
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind == RequestKind::kChunk && call.subject == missing) {
      detail::HttpResponse response;
      response.status = 404;
      return response;
    }
    return std::nullopt;
  });

  std::vector<Result> results = download({111, 222});

  EXPECT_NE(results[0].error.find("CDN request failed"), std::string::npos) << results[0].error;
  EXPECT_NE(results[0].error.find("HTTP 404"), std::string::npos) << results[0].error;
  EXPECT_EQ(results[0].error_kind, ErrorKind::kRejected);
  EXPECT_EQ(results[0].path, itemDir(111));  // set for a failed item too
  EXPECT_TRUE(results[1].error.empty());
  expectOnDisk(fine);
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
  EXPECT_TRUE(ListFiles(itemDir(111)).empty());
  EXPECT_TRUE(detail::testing::RecordedSleeps().empty());  // a 404 is not worth waiting for
}

TEST_F(ClientTest, FailedUpdateKeepsTheOldFiles) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].error.empty());

  FakeItem updated = MakeItem(111, "Alpha", 40);
  m_steam.addItem(updated);
  std::string missing = ChunkId(updated.files[1].content, updated.chunk_size, 1);
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kChunk || call.subject != missing) return std::nullopt;
    detail::HttpResponse response;
    response.status = 404;
    return response;
  });

  EXPECT_FALSE(download({111})[0].error.empty());
  expectOnDisk(item);
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
}

TEST_F(ClientTest, CorruptChunkIsFetchedFromAnotherHost) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  m_steam.setHttpHook([](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kChunk || call.host != detail::testing::kFakeHostA) return std::nullopt;
    detail::HttpResponse response;
    response.status = 200;
    response.body.assign(64, 0x42);
    return response;
  });

  std::vector<Result> results = download({111}, sequential());

  EXPECT_TRUE(results[0].error.empty()) << results[0].error;
  expectOnDisk(item);
  // The worker moved on after the first bad answer and stayed with the second host.
  EXPECT_EQ(m_steam.callsToHost(detail::testing::kFakeHostA, RequestKind::kChunk), 1);
  EXPECT_EQ(m_steam.callsToHost(detail::testing::kFakeHostB, RequestKind::kChunk), static_cast<int>(totalChunks(item)));
  EXPECT_TRUE(detail::testing::RecordedSleeps().empty());
}

TEST_F(ClientTest, CancelledDownloadKeepsTheOldFilesAndLeavesNoTemporaryFiles) {
  FakeItem item;
  item.id = 111;
  item.title = "Big";
  item.chunk_size = 20;
  item.files = {{"big.bin", Content(1, 400), false}, {"small.txt", Content(2, 30), false}};
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].error.empty());

  FakeItem updated = item;
  updated.files[0].content = Content(3, 400);  // twenty chunks
  updated.files[1].content = Content(4, 30);
  m_steam.addItem(updated);
  std::stop_source stop;
  std::atomic<int> chunk_calls{0};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind == RequestKind::kChunk && ++chunk_calls == 3) stop.request_stop();
    return std::nullopt;
  });
  Options options = sequential();
  options.stop = stop.get_token();

  std::vector<Result> results = download({111}, options);

  EXPECT_EQ(results[0].error, "cancelled");
  EXPECT_EQ(results[0].error_kind, ErrorKind::kCancelled);
  EXPECT_TRUE(results[0].cancelled());
  EXPECT_LT(chunk_calls.load(), 8);
  expectOnDisk(item);
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
}

TEST_F(ClientTest, StopRequestedBeforeTheStartCancelsEverything) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  std::stop_source stop;
  stop.request_stop();
  std::vector<ItemInfo> infos;
  Options options;
  options.stop = stop.get_token();
  options.on_resolved = [&](const ItemInfo& info) { infos.push_back(info); };

  std::vector<Result> results = download({111}, options);

  EXPECT_EQ(results[0].error, "cancelled");
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].error, "cancelled");
  EXPECT_TRUE(infos[0].cancelled());
  EXPECT_TRUE(results[0].cancelled());
  EXPECT_TRUE(m_steam.calls().empty());
  EXPECT_FALSE(fs::exists(itemDir(111)));
}

// ---- retries ----

TEST_F(ClientTest, ItemDetailsRequestIsRetried) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::atomic<int> details_calls{0};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kDetails || ++details_calls > 2) return std::nullopt;
    detail::HttpResponse response;
    response.status = details_calls == 1 ? 503 : 429;
    return response;
  });

  std::vector<Result> results = download({111});

  EXPECT_TRUE(results[0].error.empty()) << results[0].error;
  expectOnDisk(item);
  auto sleeps = detail::testing::RecordedSleeps();
  ASSERT_EQ(sleeps.size(), 2u);
  ExpectDelayBetween(sleeps[0], milliseconds(500));
  ExpectDelayBetween(sleeps[1], milliseconds(1000));
}

TEST_F(ClientTest, ItemDetailsRequestThatKeepsFailingFailsItsBatchOnly) {
  constexpr std::uint64_t kItems = 101;  // two batches: 100 and 1
  for (std::uint64_t id = 1; id <= kItems; ++id) m_steam.addItem(MakeTinyItem(id));
  m_steam.setHttpHook([](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kDetails || !call.subject.starts_with("1,")) return std::nullopt;
    detail::HttpResponse response;
    response.status = 503;
    return response;
  });
  std::vector<std::uint64_t> ids;
  for (std::uint64_t id = 1; id <= kItems; ++id) ids.push_back(id);
  std::vector<ItemInfo> infos;
  Options options;
  options.on_resolved = [&](const ItemInfo& info) { infos.push_back(info); };

  std::vector<Result> results = download(ids, options);

  ASSERT_EQ(results.size(), kItems);
  for (std::size_t i = 0; i < 100; ++i) {
    EXPECT_EQ(results[i].error, "item details: GetPublishedFileDetails: HTTP 503") << i;
    EXPECT_EQ(results[i].error_kind, ErrorKind::kNetwork) << i;
  }
  EXPECT_TRUE(results[100].error.empty()) << results[100].error;
  EXPECT_EQ(ReadFile(itemDir(101) / "a.txt"), Content(101, 10));
  ASSERT_EQ(infos.size(), kItems);
  EXPECT_EQ(infos[0].error, results[0].error);
  EXPECT_TRUE(infos[100].error.empty());
  EXPECT_EQ(m_steam.callCount(RequestKind::kDetails), kMaxAttempts + 1);
  EXPECT_EQ(detail::testing::RecordedSleeps().size(), static_cast<std::size_t>(kMaxAttempts - 1));
}

TEST_F(ClientTest, ItemDetailsFailureWithoutRetryIsReportedAtOnce) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.setHttpHook([](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kDetails) return std::nullopt;
    detail::HttpResponse response;
    response.status = 403;
    return response;
  });
  std::vector<Result> results = download({111});
  EXPECT_EQ(results[0].error, "item details: GetPublishedFileDetails: HTTP 403");
  EXPECT_EQ(results[0].error_kind, ErrorKind::kRejected);
  EXPECT_EQ(m_steam.callCount(RequestKind::kDetails), 1);
  EXPECT_TRUE(detail::testing::RecordedSleeps().empty());
}

TEST_F(ClientTest, TransportErrorsAreRetried) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::atomic<int> failures{0};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind == RequestKind::kDetails && ++failures <= 1) detail::FailTransient("connection reset");
    return std::nullopt;
  });
  EXPECT_TRUE(download({111})[0].error.empty());
  EXPECT_EQ(detail::testing::RecordedSleeps().size(), 1u);
}

TEST_F(ClientTest, CancelledWhileWaitingToRetry) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  std::stop_source stop;
  m_steam.setHttpHook([&](const HttpCall&) -> std::optional<detail::HttpResponse> {
    stop.request_stop();
    detail::HttpResponse response;
    response.status = 503;
    return response;
  });
  Options options;
  options.stop = stop.get_token();

  std::vector<Result> results = download({111}, options);

  EXPECT_EQ(results[0].error, "cancelled");
  EXPECT_EQ(m_steam.callCount(RequestKind::kDetails), 1);
}

TEST_F(ClientTest, CdnRequestsBackOffAfterTransientFailures) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::atomic<int> chunk_calls{0};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kChunk || ++chunk_calls > 2) return std::nullopt;
    detail::HttpResponse response;
    response.status = 503;
    return response;
  });

  std::vector<Result> results = download({111}, sequential());

  EXPECT_TRUE(results[0].error.empty()) << results[0].error;
  expectOnDisk(item);
  auto sleeps = detail::testing::RecordedSleeps();
  ASSERT_EQ(sleeps.size(), 2u);
  ExpectDelayBetween(sleeps[0], milliseconds(500));
  ExpectDelayBetween(sleeps[1], milliseconds(1000));
  // The failed chunk went to the first host, then the second, then back to the first, where the worker stayed.
  EXPECT_EQ(m_steam.callsToHost(detail::testing::kFakeHostB, RequestKind::kChunk), 1);
  EXPECT_EQ(m_steam.callsToHost(detail::testing::kFakeHostA, RequestKind::kChunk),
            static_cast<int>(totalChunks(item)) + 1);
}

TEST_F(ClientTest, CdnChunkThatKeepsFailingGivesUp) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.setHttpHook([](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kChunk) return std::nullopt;
    detail::HttpResponse response;
    response.status = 502;
    return response;
  });

  std::vector<Result> results = download({111}, sequential());

  EXPECT_NE(results[0].error.find("HTTP 502"), std::string::npos) << results[0].error;
  EXPECT_EQ(results[0].error_kind, ErrorKind::kNetwork);
  // Two hosts: four attempts, a pause after each but the last.
  EXPECT_EQ(m_steam.callCount(RequestKind::kChunk), 4);
  EXPECT_EQ(detail::testing::RecordedSleeps().size(), 3u);
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
}

TEST_F(ClientTest, CdnServerListIsRetried) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::atomic<int> list_calls{0};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kServers || ++list_calls > 1) return std::nullopt;
    detail::HttpResponse response;
    response.status = 500;
    return response;
  });
  EXPECT_TRUE(download({111})[0].error.empty());
  EXPECT_EQ(m_steam.callCount(RequestKind::kServers), 2);
}

TEST_F(ClientTest, LegacyFileRequestIsRetried) {
  m_steam.addLegacyItem(FakeLegacyItem{7, "Old mod", "Old.zip", "legacy bytes"});
  std::atomic<int> file_calls{0};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kLegacyFile || ++file_calls > 1) return std::nullopt;
    detail::HttpResponse response;
    response.status = 503;
    return response;
  });
  EXPECT_TRUE(download({7})[0].error.empty());
  EXPECT_EQ(ReadFile(itemDir(7) / "Old.zip"), "legacy bytes");
}

TEST_F(ClientTest, RateLimitedManifestRequestCodeIsRetried) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::atomic<int> rpc_calls{0};
  m_steam.setSessionHook([&](SessionCall call) {
    if (call == SessionCall::kManifestRequestCode && ++rpc_calls <= 2) {
      detail::FailEResult("RPC ContentServerDirectory.GetManifestRequestCode#1 failed", 84);
    }
  });

  std::vector<Result> results = download({111});

  EXPECT_TRUE(results[0].error.empty()) << results[0].error;
  EXPECT_EQ(m_steam.sessionCallCount(SessionCall::kManifestRequestCode), 3);
  EXPECT_EQ(detail::testing::RecordedSleeps().size(), 2u);
}

TEST_F(ClientTest, RejectedDepotKeyIsNotRetried) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.setDepotKeyEResult(15);
  std::vector<Result> results = download({111});
  EXPECT_EQ(results[0].error, "depot key request failed: access denied (EResult 15)");
  EXPECT_EQ(results[0].error_kind, ErrorKind::kRejected);
  EXPECT_EQ(m_steam.sessionCallCount(SessionCall::kDepotKey), 1);
  EXPECT_TRUE(detail::testing::RecordedSleeps().empty());
}

TEST_F(ClientTest, DepotKeyRequestThatIsRateLimitedIsRetriedUntilItGivesUp) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.setDepotKeyEResult(84);
  std::vector<Result> results = download({111});
  EXPECT_EQ(results[0].error, "depot key request failed: rate limit exceeded (EResult 84)");
  EXPECT_EQ(results[0].error_kind, ErrorKind::kNetwork);
  EXPECT_EQ(m_steam.sessionCallCount(SessionCall::kDepotKey), kMaxAttempts);
  EXPECT_EQ(detail::testing::RecordedSleeps().size(), static_cast<std::size_t>(kMaxAttempts - 1));
}

TEST_F(ClientTest, SessionCallsThatTimeOutAreRetried) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::atomic<int> calls{0};
  m_steam.setSessionHook([&](SessionCall call) {
    if (call == SessionCall::kDepotKey && ++calls == 1) detail::FailTransient("Steam CM request timed out");
  });
  EXPECT_TRUE(download({111})[0].error.empty());
  EXPECT_EQ(m_steam.sessionCallCount(SessionCall::kDepotKey), 2);
}

// ---- pipeline ----

TEST_F(ClientTest, ItemsAreDownloadedWhileLaterDetailsAreStillBeingLookedUp) {
  constexpr std::uint64_t kItems = 101;  // two batches
  for (std::uint64_t id = 1; id <= kItems; ++id) m_steam.addItem(MakeTinyItem(id));
  std::atomic<int> chunk_calls{0};
  std::atomic<bool> downloaded_meanwhile{false};
  m_steam.setHttpHook([&](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind == RequestKind::kChunk) ++chunk_calls;
    if (call.kind == RequestKind::kDetails && call.subject == "101") {
      // The second batch is held up until the first batch's items have started to download: if the details of
      // all items were looked up before the first download, this would wait in vain.
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (chunk_calls == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
      }
      downloaded_meanwhile = chunk_calls > 0;
    }
    return std::nullopt;
  });
  std::vector<std::uint64_t> ids;
  for (std::uint64_t id = 1; id <= kItems; ++id) ids.push_back(id);

  std::vector<Result> results = download(ids);

  EXPECT_TRUE(downloaded_meanwhile);
  for (const Result& result : results) EXPECT_TRUE(result.error.empty()) << result.item_id << ": " << result.error;
}

TEST_F(ClientTest, ResolvesItemsInOrder) {
  for (std::uint64_t id : {30, 10, 20}) m_steam.addItem(MakeTinyItem(id));
  std::vector<std::uint64_t> resolved;
  Options options;
  options.on_resolved = [&](const ItemInfo& info) { resolved.push_back(info.item_id); };
  download({30, 10, 20}, options);
  EXPECT_EQ(resolved, (std::vector<std::uint64_t>{30, 10, 20}));
}

// ---- error kinds ----

TEST_F(ClientTest, ChunkThatIsCorruptOnEveryHostIsADataError) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.setHttpHook([](const HttpCall& call) -> std::optional<detail::HttpResponse> {
    if (call.kind != RequestKind::kChunk) return std::nullopt;
    detail::HttpResponse response;
    response.status = 200;
    response.body.assign(64, 0x42);
    return response;
  });
  std::vector<Result> results = download({111}, sequential());
  EXPECT_FALSE(results[0].ok());
  EXPECT_EQ(results[0].error_kind, ErrorKind::kData) << results[0].error;
}

#ifndef _WIN32
TEST_F(ClientTest, DestinationThatIsASymlinkIsAFilesystemError) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  fs::path elsewhere = m_root / "elsewhere";
  fs::create_directories(elsewhere);
  fs::create_directory_symlink(elsewhere, itemDir(111));
  std::vector<Result> results = download({111});
  EXPECT_EQ(results[0].error, "destination must not be a symlink");
  EXPECT_EQ(results[0].error_kind, ErrorKind::kFilesystem);
  EXPECT_TRUE(fs::is_empty(elsewhere));
}
#endif

// ---- downloaded and unpacked bytes ----

TEST_F(ClientTest, ProgressCountsDownloadedAndUnpackedBytesSeparately) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  std::mutex mutex;
  std::vector<Progress> seen;
  Options options = sequential();
  options.on_progress = [&](const Progress& progress) {
    std::lock_guard lock(mutex);
    seen.push_back(progress);
  };

  std::vector<Result> results = download({111}, options);

  std::uint64_t content_bytes = 0;
  for (const FakeFile& file : item.files) content_bytes += file.content.size();
  ASSERT_TRUE(results[0].ok()) << results[0].error;
  ASSERT_FALSE(seen.empty());
  const Progress& last = seen.back();
  EXPECT_EQ(last.unpacked_total, content_bytes);
  EXPECT_EQ(last.unpacked_bytes, content_bytes);
  EXPECT_GT(last.downloaded_total, 0u);
  EXPECT_EQ(last.downloaded_bytes, last.downloaded_total);
  // The chunks are encrypted (a 16-byte IV and padding at least), so the two are different quantities.
  EXPECT_NE(last.downloaded_total, last.unpacked_total);
  EXPECT_EQ(results[0].unpacked_bytes, last.unpacked_bytes);
  EXPECT_EQ(results[0].downloaded_bytes, last.downloaded_bytes);
  for (std::size_t i = 1; i < seen.size(); ++i) {
    EXPECT_GE(seen[i].downloaded_bytes, seen[i - 1].downloaded_bytes);
    EXPECT_GE(seen[i].unpacked_bytes, seen[i - 1].unpacked_bytes);
    EXPECT_EQ(seen[i].downloaded_total, last.downloaded_total);
    EXPECT_EQ(seen[i].unpacked_total, last.unpacked_total);
  }
}

TEST_F(ClientTest, ResultCountsOnlyWhatWasFetchedInThisRun) {
  FakeItem item = MakeItem(111, "Alpha");
  m_steam.addItem(item);
  ASSERT_TRUE(download({111})[0].ok());

  Result again = download({111})[0];
  EXPECT_TRUE(again.ok());
  EXPECT_EQ(again.downloaded_bytes, 0u);
  EXPECT_EQ(again.unpacked_bytes, 0u);

  FakeItem updated = item;
  updated.files[1].content = Content(99, 75);  // only Things.xml changes
  m_steam.addItem(updated);
  Result changed = download({111})[0];
  EXPECT_TRUE(changed.ok());
  EXPECT_EQ(changed.unpacked_bytes, 75u);
  EXPECT_GT(changed.downloaded_bytes, 0u);
}

TEST_F(ClientTest, LegacyItemReportsItsFileAsDownloadedAndUnpacked) {
  m_steam.addLegacyItem(FakeLegacyItem{7, "Old mod", "Old.zip", "legacy bytes"});
  Result result = download({7})[0];
  EXPECT_EQ(result.downloaded_bytes, 12u);
  EXPECT_EQ(result.unpacked_bytes, 12u);
}

// ---- repeated IDs ----

TEST_F(ClientTest, RepeatedIdIsDownloadedOnceAndAnsweredEachTime) {
  FakeItem alpha = MakeItem(111, "Alpha");
  FakeItem beta = MakeItem(222, "Beta", 5);
  m_steam.addItem(alpha);
  m_steam.addItem(beta);
  std::vector<std::uint64_t> resolved;
  Options options;
  options.on_resolved = [&](const ItemInfo& info) { resolved.push_back(info.item_id); };

  std::vector<Result> results = download({111, 222, 111, 111}, options);

  ASSERT_EQ(results.size(), 4u);
  EXPECT_EQ(results[0].item_id, 111u);
  EXPECT_EQ(results[1].item_id, 222u);
  EXPECT_EQ(results[2].item_id, 111u);
  EXPECT_EQ(results[3].item_id, 111u);
  for (const Result& result : results) EXPECT_TRUE(result.ok()) << result.error;
  for (std::size_t copy : {2u, 3u}) {
    EXPECT_EQ(results[copy].title, results[0].title);
    EXPECT_EQ(results[copy].path, results[0].path);
    EXPECT_EQ(results[copy].downloaded_bytes, results[0].downloaded_bytes);
    EXPECT_EQ(results[copy].unpacked_bytes, results[0].unpacked_bytes);
  }
  EXPECT_EQ(resolved, (std::vector<std::uint64_t>{111, 222}));  // once per distinct item
  EXPECT_EQ(m_steam.callCount(RequestKind::kChunk), static_cast<int>(totalChunks(alpha) + totalChunks(beta)));
  expectOnDisk(alpha);
  expectOnDisk(beta);
}

TEST_F(ClientTest, RepeatedFailingIdFailsEveryOccurrence) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  std::vector<Result> results = download({12345, 111, 12345});
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].error_kind, ErrorKind::kNotFound);
  EXPECT_EQ(results[2].error, results[0].error);
  EXPECT_EQ(results[2].error_kind, ErrorKind::kNotFound);
  EXPECT_TRUE(results[1].ok());
}

// ---- options ----

TEST_F(ClientTest, ZeroParallelismIsRefused) {
  Client client;
  Options options;
  options.parallel_items = 0;
  EXPECT_THROW(client.download({}, m_root, options), std::invalid_argument);
  options = {};
  options.threads_per_item = 0;
  EXPECT_THROW(client.download({}, m_root, options), std::invalid_argument);
}

TEST_F(ClientTest, TimeoutsBelowOneSecondAreRefused) {
  ClientOptions options;
  options.connect_timeout = std::chrono::seconds(0);
  EXPECT_THROW(Client{options}, std::invalid_argument);
  options = {};
  options.stall_timeout = std::chrono::seconds(0);
  EXPECT_THROW(Client{options}, std::invalid_argument);
}

TEST_F(ClientTest, ClientOptionsReachEveryRequest) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  ClientOptions options;
  options.proxy = "http://proxy.test:3128";
  options.connect_timeout = std::chrono::seconds(7);
  options.stall_timeout = std::chrono::seconds(11);
  Client client(options);
  std::vector<std::uint64_t> ids{111};
  ASSERT_TRUE(client.download(ids, m_root)[0].ok());

  detail::HttpConfig config = detail::testing::LastHttpConfig();
  EXPECT_EQ(config.proxy, std::optional<std::string>("http://proxy.test:3128"));
  EXPECT_EQ(config.connect_timeout_seconds, 7);
  EXPECT_EQ(config.stall_timeout_seconds, 11);
}

TEST_F(ClientTest, DefaultClientOptionsLeaveTheProxyToTheEnvironment) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  ASSERT_TRUE(download({111})[0].ok());
  detail::HttpConfig config = detail::testing::LastHttpConfig();
  EXPECT_FALSE(config.proxy.has_value());
  EXPECT_EQ(config.connect_timeout_seconds, 10);
  EXPECT_EQ(config.stall_timeout_seconds, 30);
}

TEST_F(ClientTest, ClientCanBeMoved) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.addItem(MakeItem(222, "Beta", 5));
  Client first;
  Client second = std::move(first);
  std::vector<std::uint64_t> ids{111};
  EXPECT_TRUE(second.download(ids, m_root)[0].ok());

  Client third;
  third = std::move(second);
  ids = {222};
  EXPECT_TRUE(third.download(ids, m_root)[0].ok());
}

// ---- callbacks that throw ----

TEST_F(ClientTest, ProgressCallbackThatThrowsFailsOnlyItsItem) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  FakeItem beta = MakeItem(222, "Beta", 5);
  m_steam.addItem(beta);
  Options options;
  options.on_progress = [](const Progress& progress) {
    if (progress.item_id == 111) throw std::runtime_error("callback failed");
  };

  std::vector<Result> results = download({111, 222}, options);

  EXPECT_EQ(results[0].error, "callback failed");
  EXPECT_EQ(results[0].error_kind, ErrorKind::kOther);
  EXPECT_TRUE(results[1].ok()) << results[1].error;
  expectOnDisk(beta);
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
}

TEST_F(ClientTest, ResolvedCallbackThatThrowsEndsTheDownload) {
  m_steam.addItem(MakeItem(111, "Alpha"));
  m_steam.addItem(MakeItem(222, "Beta", 5));
  Options options;
  options.on_resolved = [](const ItemInfo& info) {
    if (info.item_id == 222) throw std::runtime_error("callback failed");
  };

  EXPECT_THROW(download({111, 222}, options), std::runtime_error);
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
  EXPECT_FALSE(HasTemporaryFiles(itemDir(222)));
}

// ---- Downloader on its own ----

class DownloaderTest : public ClientTest {
 protected:
  detail::ItemJob makeJob(const FakeItem& item) {
    m_steam.addItem(item);
    detail::ItemJob job;
    std::vector<std::uint64_t> ids{item.id};
    detail::FetchItems(ids, {}, {}, [&](detail::Item found) { job.item = std::move(found); });
    job.destination = itemDir(item.id);
    return job;
  }

  detail::Session m_session{detail::HttpConfig{}};
};

TEST_F(DownloaderTest, DestroyedWithoutFinishStopsItsJobs) {
  detail::ItemJob job = makeJob(MakeItem(111, "Alpha"));
  {
    detail::Downloader downloader(m_session, 1, 2, 4, {});
    downloader.add(job);
  }  // no finish(): the destructor stops the work and waits for the threads
  // The job either got through before the stop or was cancelled, but it is over, and nothing is left behind.
  EXPECT_TRUE(job.error.empty() || job.error == "cancelled") << job.error;
  EXPECT_FALSE(HasTemporaryFiles(itemDir(111)));
}

TEST_F(DownloaderTest, RefusesMoreJobsThanAnnounced) {
  detail::ItemJob first = makeJob(MakeTinyItem(1));
  detail::ItemJob second = makeJob(MakeTinyItem(2));
  detail::Downloader downloader(m_session, 1, 1, 1, {});
  downloader.add(first);
  EXPECT_THROW(downloader.add(second), std::runtime_error);
  downloader.finish();
  EXPECT_TRUE(first.error.empty()) << first.error;
}

TEST_F(DownloaderTest, FinishWithoutJobsReturns) {
  detail::Downloader downloader(m_session, 3, 2, 4, {});
  downloader.finish();
}

TEST_F(DownloaderTest, StopRequestedWhileJobsAreQueuedCancelsThem) {
  detail::ItemJob job = makeJob(MakeItem(111, "Alpha"));
  std::stop_source stop;
  stop.request_stop();
  detail::Downloader downloader(m_session, 1, 1, 1, stop.get_token());
  downloader.add(job);
  downloader.finish();
  EXPECT_EQ(job.error, "cancelled");
  EXPECT_FALSE(fs::exists(itemDir(111)));
}

}  // namespace
}  // namespace pxsteamdl
