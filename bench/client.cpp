// SPDX-License-Identifier: LGPL-3.0-or-later
// The program the benchmark measures: connects, downloads the given items into a directory and prints one JSON line.
// It only uses the public API, so the same source builds against the curl and the asio versions of the library.
//
//   bench-client --root DIR --ids 1000,1001,... [--parallel-items 2] [--threads-per-item 4] [--hold-ms 0] [--idle-only]
// --hold-ms keeps the connected client idle for that long before downloading; with --idle-only it then exits without
// downloading (for measuring the footprint of a connected, idle client).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pxsteamdl/pxsteamdl.hpp"

namespace {

double MillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string root;
    std::vector<std::uint64_t> ids;
    pxsteamdl::Options options;
    int hold_ms = 0;
    bool idle_only = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto value = [&] {
        if (++i >= argc) throw std::runtime_error("missing value for " + arg);
        return std::string(argv[i]);
      };
      if (arg == "--root") {
        root = value();
      } else if (arg == "--ids") {
        std::stringstream in(value());
        for (std::string id; std::getline(in, id, ',');) ids.push_back(std::stoull(id));
      } else if (arg == "--parallel-items") {
        options.parallel_items = static_cast<unsigned>(std::stoi(value()));
      } else if (arg == "--threads-per-item") {
        options.threads_per_item = static_cast<unsigned>(std::stoi(value()));
      } else if (arg == "--idle-only") {
        idle_only = true;
      } else if (arg == "--hold-ms") {
        hold_ms = std::stoi(value());
      } else {
        throw std::runtime_error("unknown option " + arg);
      }
    }
    if (idle_only && ids.empty()) ids.push_back(0);
    if (root.empty() || ids.empty()) throw std::runtime_error("--root and --ids are required");

    pxsteamdl::ClientOptions client_options;
    client_options.proxy = "";  // the benchmark server is local: never go through a proxy from the environment
    auto start = std::chrono::steady_clock::now();
    pxsteamdl::Client client(client_options);
    double ctor_ms = MillisSince(start);

    if (hold_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    if (idle_only) {
      std::printf("{\"ctor_ms\":%.2f,\"download_ms\":0,\"items\":0,\"failed\":0,\"downloaded_bytes\":0,\"unpacked_bytes\":0}\n", ctor_ms);
      return 0;
    }

    std::atomic<std::uint64_t> progress_calls{0};
    options.on_progress = [&](const pxsteamdl::Progress&) { ++progress_calls; };
    start = std::chrono::steady_clock::now();
    std::vector<pxsteamdl::Result> results = client.download(ids, root, options);
    double download_ms = MillisSince(start);

    std::uint64_t downloaded = 0;
    std::uint64_t unpacked = 0;
    int failed = 0;
    std::string first_error;
    for (const pxsteamdl::Result& result : results) {
      downloaded += result.downloaded_bytes;
      unpacked += result.unpacked_bytes;
      if (!result.ok()) {
        if (!failed) first_error = result.error;
        ++failed;
      }
    }
    std::printf(
        "{\"ctor_ms\":%.2f,\"download_ms\":%.2f,\"items\":%zu,\"failed\":%d,\"downloaded_bytes\":%llu,"
        "\"unpacked_bytes\":%llu,\"progress_calls\":%llu,\"first_error\":\"%s\"}\n",
        ctor_ms, download_ms, results.size(), failed, static_cast<unsigned long long>(downloaded),
        static_cast<unsigned long long>(unpacked), static_cast<unsigned long long>(progress_calls.load()), first_error.c_str());
    return failed ? 2 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bench-client: %s\n", e.what());
    return 1;
  }
}
