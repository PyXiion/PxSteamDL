// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/pxsteamdl.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <pthread.h>
#include <string_view>
#include <thread>

namespace {

void usage() {
    std::fputs("usage: pxsteamdl [-o DIR] [-j PARALLEL_ITEMS] [-t THREADS_PER_ITEM] ITEM_ID...\n"
               "Downloads Steam Workshop items anonymously into DIR/<ITEM_ID>/ (default DIR: .)\n",
               stderr);
}

bool parse(std::string_view text, auto& value) {
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

// Ctrl-C requests a graceful stop; a second one quits immediately.
void watch_interrupts(std::stop_source stop, sigset_t signals) {
    int signal = 0;
    sigwait(&signals, &signal);
    std::fputs("\ninterrupted, stopping (Ctrl-C again to quit immediately)\n", stderr);
    stop.request_stop();
    sigwait(&signals, &signal);
    std::_Exit(130);
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path root = ".";
    pxsteamdl::Options options;
    std::vector<std::uint64_t> ids;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "-o" && has_value) {
            root = argv[++i];
        } else if (arg == "-j" && has_value && parse(argv[i + 1], options.parallel_items)) {
            ++i;
        } else if (arg == "-t" && has_value && parse(argv[i + 1], options.threads_per_item)) {
            ++i;
        } else if (std::uint64_t id; parse(arg, id)) {
            ids.push_back(id);
        } else {
            usage();
            return 2;
        }
    }
    if (ids.empty()) {
        usage();
        return 2;
    }

    // Block SIGINT before any thread starts, so only the watcher receives it.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::stop_source stop;
    options.stop = stop.get_token();
    std::thread(watch_interrupts, stop, signals).detach();

    try {
        auto start = std::chrono::steady_clock::now();
        pxsteamdl::Client client;
        auto results = client.download(ids, root, options);
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
        if (stop.stop_requested()) return 130;
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
