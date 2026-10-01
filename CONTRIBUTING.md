# Contributing

## Building and testing

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPXSTEAMDL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The unit tests (GoogleTest, `tests/`) need no network access and no waiting; see [Tests](#tests) below for how.

## Code style

C++ follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html), lightened as described
below. `.clang-format` and `.clang-tidy` encode the mechanical parts, and CI checks both:

```sh
clang-format -i src/*.cpp src/*.hpp include/pxsteamdl/*.hpp cli/main.cpp python/pxsteamdl.cpp tests/*.cpp tests/*.hpp tests/fakes/*.?pp
clang-tidy -p build src/*.cpp cli/main.cpp tests/*.cpp tests/fakes/*.cpp
```

After changing a naming rule in `.clang-tidy`, `run-clang-tidy -p build -fix '/(src|cli|tests)/'` renames the
identifiers and their uses across the code base; code under `#ifdef _WIN32` and `python/` is not compiled on Linux
and has to be renamed by hand.

### Rules

- **Naming** (Google's, except for methods and data members): types and free functions `PascalCase`; methods
  `camelCase`; variables, parameters and struct members `snake_case`; private and protected class data members
  `m_camelCase`; constants and enumerators `kPascalCase`; namespaces `snake_case`.
- **One header per source file:** every `src/foo.cpp` has a `src/foo.hpp` declaring what other files use, and
  everything else stays in an unnamed namespace.
- **Include order:** the related header, C system headers, C++ standard headers, other libraries, project headers,
  each block separated by a blank line. Platform-specific includes go last.
- **`struct` only for passive data;** anything with invariants or behavior is a `class` with private members.
- **No magic numbers:** protocol field numbers, signatures, sizes and timeouts are named constants.
- **Short functions:** a function that does several things is split into named steps.
- Prefer return values (`std::optional` included) to output parameters; where a function does fill in or update
  an argument, it takes a non-const reference. Single-argument constructors are `explicit`.

### Where it differs from Google's guide

- **Exceptions are used** for errors (`std::runtime_error`, via `detail::Fail`), so per-item failures can unwind
  through the download pipeline. `detail::TransientError` marks a failure that is worth retrying (`detail::Retry`).
- **120 columns** instead of 80 (indentation stays at 2 spaces).
- **`#pragma once`** instead of `#define` guards; **`.hpp`/`.cpp`** extensions.
- The **public API keeps snake_case fields** (`Options::parallel_items`), as Google style allows for structs, and
  the Python API follows PEP 8 (`Client.download`).

## Tests

The unit tests build the library's sources against fakes of the three files that touch the outside world:
`src/http.cpp`, `src/session.cpp` and `src/sleep.cpp` are replaced by `tests/fakes/fake_*.cpp`, so the whole download
pipeline (item lookup, retries, manifests, chunk decoding, file handling, cancellation) runs offline and without
waiting. `tests/fake_steam.cpp` is a pretend Steam built from items a test declares, and `tests/client_test.cpp`
drives `Client` against it. The real `SleepFor()` has its own small test executable. Anything new that talks to the
network or sleeps goes through one of those three files.

## Versioning

From 1.0.0 the project follows [Semantic Versioning 2.0.0](https://semver.org/). Before 1.0.0 a minor version may
break the API; a patch version never does.

Covered by the version number:

- the C++ API in `include/pxsteamdl/` (everything in namespace `pxsteamdl` except `detail`) and the macros of
  `<pxsteamdl/version.hpp>`;
- the Python package `pxsteamdl`: the names in `__all__`;
- the CLI's options and exit codes (0 success, 1 failure, 2 usage error, 130 interrupted);
- the CMake package: `find_package(PxSteamDL)` and the target `PxSteamDL::pxsteamdl`;
- the layout on disk: an item goes into `DIR/<ITEM_ID>/`, as with steamcmd;
- the exact string `cancelled` as `Result::error` and `ItemInfo::error` of a stopped item.

Not covered: the wording of other error messages (match on `Result::error` being empty or not, not on its text),
the format of the CLI's human-readable output, the internal headers in `src/` and namespace `pxsteamdl::detail`, and the
build recipe (which dependencies are built, and how). A change Steam forces on the protocol is a patch release even if
it changes behavior.

A deprecated API stays for at least one minor version and is announced in the changelog. Dropping a supported platform,
Python version or compiler is a minor version and is announced in the changelog.

### Releasing

1. Move the entries of `[Unreleased]` in `CHANGELOG.md` under a new `## [X.Y.Z] - date` heading and update the links
   at the bottom.
2. Set the version in `project(PxSteamDL VERSION X.Y.Z ...)` in `CMakeLists.txt` (the wheels, the CLI and the
   package config read it from there).
3. Tag the commit `vX.Y.Z` and push the tag. The release workflow refuses a tag that does not match the version in
   `CMakeLists.txt` or that has no changelog section, builds the binaries, wheels and sdist, and publishes the release
   with the changelog section as its notes.
