// SPDX-License-Identifier: LGPL-3.0-or-later
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <pxsteamdl/pxsteamdl.hpp>

int main(int argc, char** argv) {
  // Never true, but the linker cannot know: it has to resolve the whole library and its dependencies.
  if (argc > 100) {
    pxsteamdl::Client client;
    std::vector<std::uint64_t> ids;
    client.download(ids, ".");
  }
  // The version of the headers the program was compiled with, and of the library it was linked with.
  if (std::strcmp(PXSTEAMDL_VERSION_STRING, std::string(pxsteamdl::Version()).c_str()) != 0) {
    std::fputs("version mismatch\n", stderr);
    return 1;
  }
  std::printf("%s\n", PXSTEAMDL_VERSION_STRING);
  return 0;
}
