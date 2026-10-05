// SPDX-License-Identifier: LGPL-3.0-or-later
// A real child process used to test lock release after forced termination.
#include <chrono>
#include <exception>
#include <fstream>
#include <thread>

#include "directory_lock.hpp"
#include "paths.hpp"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    pxsteamdl::detail::DirectoryLock lock(pxsteamdl::detail::Utf8Path(argv[1]), {});
    {
      std::ofstream ready(pxsteamdl::detail::Utf8Path(argv[2]));
      ready << "ready\n";
      if (!ready) return 1;
    }
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  } catch (const std::exception&) {
    return 1;
  }
}
