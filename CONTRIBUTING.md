# Contributing

## Building and testing

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPXSTEAMDL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The unit tests (GoogleTest, `tests/`) cover the offline parts: protobuf and CM packet framing, ZIP, Steam's
symmetric encryption, chunk decoding, manifests and path validation. They need no network access.

## Code style

C++ follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html), lightened as described
below. `.clang-format` and `.clang-tidy` encode the mechanical parts, and CI checks both:

```sh
clang-format -i src/*.cpp src/*.hpp include/pxsteamdl/*.hpp cli/main.cpp python/pxsteamdl.cpp tests/*.cpp tests/*.hpp
clang-tidy -p build src/*.cpp cli/main.cpp tests/*.cpp
```

After changing a naming rule in `.clang-tidy`, `run-clang-tidy -p build -fix '/(src|cli|tests)/'` renames the
identifiers and their uses across the code base; code under `#ifdef _WIN32` and `python/` is not compiled on Linux
and has to be renamed by hand.

### Kept from the Google style

- **Naming:** types and free functions `PascalCase`; methods `camelCase`; variables, parameters and struct
  members `snake_case`; private and protected class data members `m_camelCase`; constants and enumerators
  `kPascalCase`; namespaces `snake_case`.
- **One header per source file:** every `src/foo.cpp` has a `src/foo.hpp` declaring what other files use, and
  everything else stays in an unnamed namespace.
- **Include order:** the related header, C system headers, C++ standard headers, other libraries, project headers,
  each block separated by a blank line. Platform-specific includes go last.
- **`struct` only for passive data;** anything with invariants or behavior is a `class` with private members.
- **No magic numbers:** protocol field numbers, signatures, sizes and timeouts are named constants.
- **Short functions:** a function that does several things is split into named steps.
- Prefer return values (`std::optional` included) to output parameters; where a function does fill in or update
  an argument, it takes a non-const reference. Single-argument constructors are `explicit`.

### Relaxed

- **Exceptions are used** for errors (`std::runtime_error`, via `detail::Fail`), so per-item failures can unwind
  through the download pipeline.
- **120 columns** instead of 80 (indentation stays at 2 spaces).
- **`#pragma once`** instead of `#define` guards; **`.hpp`/`.cpp`** extensions.
- The **public API keeps snake_case fields** (`Options::parallel_items`), as Google style allows for structs, and
  the Python API follows PEP 8 (`Client.download`).
