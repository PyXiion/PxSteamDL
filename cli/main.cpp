// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/pxsteamdl.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <thread>

namespace {

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
// 128 + SIGINT, as shells report a process killed by Ctrl-C.
constexpr int kExitInterrupted = 130;

void Usage() {
    std::fputs(
        "usage: pxsteamdl [-o DIR] [-j PARALLEL_ITEMS] [-t THREADS_PER_ITEM] ITEM_ID...\n"
        "Downloads Steam Workshop items anonymously into DIR/<ITEM_ID>/ (default DIR: .)\n",
        stderr);
}

bool Parse(std::string_view text, auto& value) {
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "must be usable from a signal handler");

// Ctrl-C requests a graceful stop; a second one quits immediately.
extern "C" void OnInterrupt(int) {
    if (interrupted.exchange(true)) std::_Exit(kExitInterrupted);
    std::signal(SIGINT, OnInterrupt);  // Windows resets the handler before calling it
}

// The handler may only touch lock-free atomics, so a thread relays the flag to the stop source.
void WatchInterrupts(std::stop_source stop) {
    while (!interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::fputs("\ninterrupted, stopping (Ctrl-C again to quit immediately)\n", stderr);
    stop.request_stop();
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path root = ".";
    pxsteamdl::Options options;
    std::vector<std::uint64_t> ids;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "-o" && has_value) {
            root = argv[++i];
        } else if (arg == "-j" && has_value && Parse(argv[i + 1], options.parallel_items)) {
            ++i;
        } else if (arg == "-t" && has_value && Parse(argv[i + 1], options.threads_per_item)) {
            ++i;
        } else if (std::uint64_t id; Parse(arg, id)) {
            ids.push_back(id);
        } else {
            Usage();
            return kExitUsage;
        }
    }
    if (ids.empty()) {
        Usage();
        return kExitUsage;
    }

    std::stop_source stop;
    options.stop = stop.get_token();
    std::signal(SIGINT, OnInterrupt);
    std::thread(WatchInterrupts, stop).detach();

    try {
        auto start = std::chrono::steady_clock::now();
        pxsteamdl::Client client;
        auto results = client.Download(ids, root, options);
        int failed = 0;
        for (const auto& result : results) {
            if (result.error.empty()) {
                std::printf("ok     %llu  %s\n", static_cast<unsigned long long>(result.item_id), result.title.c_str());
            } else {
                ++failed;
                std::printf("failed %llu  %s\n", static_cast<unsigned long long>(result.item_id), result.error.c_str());
            }
        }
        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        std::fprintf(stderr, "%zu items, %d failed, %.1fs\n", results.size(), failed, elapsed.count());
        if (stop.stop_requested()) return kExitInterrupted;
        return failed ? kExitFailed : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kExitFailed;
    }
}
