// SPDX-License-Identifier: LGPL-3.0-or-later
// Uses all of the C++ API: Version(), Client, Options (parallelism, on_resolved, on_progress, stop token),
// ItemInfo, Progress and Result. Usage: example-download DIR ITEM_ID...
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <pxsteamdl/pxsteamdl.hpp>

namespace {

std::stop_source g_stop;

// request_stop() is not guaranteed async-signal-safe, so the handler only sets a flag and a watcher thread acts.
volatile std::sig_atomic_t g_interrupted = 0;
void OnSignal(int) { g_interrupted = 1; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: example-download DIR ITEM_ID...\n";
    return 2;
  }
  std::filesystem::path root = argv[1];
  std::vector<std::uint64_t> ids;
  for (int i = 2; i < argc; ++i) ids.push_back(std::strtoull(argv[i], nullptr, 10));

  std::cout << "pxsteamdl " << pxsteamdl::Version() << " (headers " << PXSTEAMDL_VERSION_STRING << ")\n";

  // Ctrl-C stops the download cleanly: finished items stay, the rest report "cancelled".
  std::signal(SIGINT, OnSignal);
  std::jthread watcher([](const std::stop_token& done) {
    while (!done.stop_requested() && !g_interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_interrupted) g_stop.request_stop();
  });

  try {
    pxsteamdl::Client client;  // anonymous login; throws std::runtime_error on failure

    pxsteamdl::Options options;
    options.parallel_items = 2;
    options.threads_per_item = 4;
    options.stop = g_stop.get_token();

    // Called on this thread, once per item, in order, before the item's bytes are downloaded.
    options.on_resolved = [](const pxsteamdl::ItemInfo& info) {
      if (info.error.empty()) {
        std::cout << "found " << info.item_id << " \"" << info.title << "\"\n";
      } else {
        std::cout << "skipped " << info.item_id << ": " << info.error << "\n";
      }
    };

    // Called from worker threads, so serialize the output.
    std::mutex print_mutex;
    options.on_progress = [&print_mutex](const pxsteamdl::Progress& progress) {
      std::lock_guard lock(print_mutex);
      std::printf("\r%llu %s: %llu/%llu bytes   ", static_cast<unsigned long long>(progress.item_id),
                  progress.title.c_str(), static_cast<unsigned long long>(progress.bytes_done),
                  static_cast<unsigned long long>(progress.bytes_total));
      std::fflush(stdout);
    };

    std::vector<pxsteamdl::Result> results = client.download(ids, root, options);

    int failed = 0;
    std::cout << "\n";
    for (const pxsteamdl::Result& result : results) {
      if (result.error.empty()) {
        std::cout << "ok     " << result.item_id << " -> " << result.path.string() << "\n";
      } else {
        ++failed;
        std::cout << "failed " << result.item_id << ": " << result.error << "\n";
      }
    }
    return g_interrupted ? 130 : failed ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
