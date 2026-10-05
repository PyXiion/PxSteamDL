# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows [Semantic Versioning](https://semver.org/)
as described in [CONTRIBUTING.md](CONTRIBUTING.md#versioning).

## [Unreleased]

### Fixed

- Failed updates preserve the previous item even when a file becomes a directory or a whole-file hash fails after
  other files were verified. Items are staged separately, then installed with rollback on ordinary rename errors.
- Cancellation during the last in-flight chunk, an up-to-date item's progress callback or a legacy request is checked
  before installation. Legacy progress callback failures preserve the old file too.
- Concurrent downloads of the same destination wait for a persistent `.<item id>.lock` with the owner's PID and a
  kernel lock. Waiting supports cancellation; process exit releases the lock and the next owner replaces stale PID data.

### Added

- `examples/`: programs using the whole C++, Python and asyncio API.
- `pxsteamdl::Version()`, the `PXSTEAMDL_VERSION_*` macros (`<pxsteamdl/version.hpp>`), `pxsteamdl --version` and
  `pxsteamdl.__version__`.
- `cmake --install` and `find_package(PxSteamDL)`: headers, the CLI, and one static archive that already contains
  curl, mbedTLS, zlib, liblzma and zstd.
- Transient failures are retried up to four times with exponential backoff (0.5 s, 1 s, 2 s): item details, the CDN
  server list, depot keys, manifest request codes, legacy files and the CM server list. After a transient failure
  (HTTP 429/5xx, a network error) CDN downloads pause before they try the next host. Pauses end when the download is
  cancelled.
- Steam's result codes are spelled out: `Steam rejected the item: not found (EResult 9)` instead of `Steam result 9`.
- `ErrorKind` (`kNone`, `kCancelled`, `kNotFound`, `kRejected`, `kNetwork`, `kData`, `kFilesystem`, `kOther`) in
  `Result::error_kind` and `ItemInfo::error_kind`, so callers need not parse messages; `pxsteamdl::Error` (a
  `std::runtime_error` with `kind()`) is what `Client`'s constructor throws. Python: `ErrorKind`, `Error`,
  `Result.error_kind`, `ItemInfo.error_kind`.
- `Result::ok()`, `Result::cancelled()`, `ItemInfo::ok()`, `ItemInfo::cancelled()` (Python properties `ok` and
  `cancelled`; `Result.ok` existed).
- `ClientOptions` (C++) and `Client(proxy=, connect_timeout=, stall_timeout=)` (Python): a proxy, a connect timeout and
  a stall timeout for all connections.
- `Result::downloaded_bytes` and `unpacked_bytes`: what a run fetched for the item, over the network and on disk.
- `Client` can be moved. Python: `Result`, `ItemInfo` and `Progress` can be constructed (keyword arguments), for tests.
- `pxsteamdl -h`/`--help` (exit code 0) and `--` (the rest are item IDs).
- Unit tests (GoogleTest, `-DPXSTEAMDL_BUILD_TESTS=ON`), including end-to-end tests of `Client` against a pretend
  Steam; the network smoke tests run in CI again, on all three platforms.

### Changed

- **Breaking:** `Progress::bytes_done` and `bytes_total` are replaced by `downloaded_bytes`/`downloaded_total` (what came
  over the network: encrypted, compressed chunks) and `unpacked_bytes`/`unpacked_total` (what was decrypted,
  decompressed and written). The old pair counted the unpacked bytes, so `unpacked_*` is the drop-in replacement. Same
  in Python.
- **Behavior change:** `download()` returns one `Result` per entry of the ID list, also for a repeated ID: the item is
  downloaded once and every occurrence gets the same result. Before, a repeated ID made two jobs write the same
  directory at once, and one of them could fail.
- **Behavior change:** `download()` throws `std::invalid_argument` if `parallel_items` or `threads_per_item` is 0
  (the CLI refuses `-j 0` and `-t 0` with exit code 2); before, 0 was taken as 1. Python already raised `ValueError`.
- `Result::path` is documented: it is `root/<id>` for every item, failed ones included. An exception thrown by
  `on_progress` fails its item (kind `kOther`), one thrown by `on_resolved` propagates out of `download()`; both are
  now documented.
- Item details are looked up in batches while earlier items already download, instead of all before the first byte.
  `on_resolved` is still called once per item, in order, on the thread that called `download()`, but progress of
  earlier items may now arrive before it.
- **Behavior change:** a failed item-details request no longer throws out of `download()`. The items of the failed
  batch carry the error in `Result.error` (`item details: ...`), and the other items are downloaded. Items that were
  not looked up before a stop request report `cancelled`.
- A depot key reply that succeeds without a key now says so, instead of reporting `EResult 1`.
- The source is split into modules, one header each, and follows the style described in
  [CONTRIBUTING.md](CONTRIBUTING.md); nothing changes for users.

## [0.2.0] - 2026-09-29

### Added

- `on_resolved` callback, called once per item with its title before any bytes are downloaded (C++ `Options`, Python
  `Client.download` and `AsyncClient.download`).
- `Progress.title`: the Workshop title of the item.
- A notice in the README that the project is made for [PxModRim](https://github.com/PyXiion/PxModRim).

## [0.1.1] - 2026-09-28

### Changed

- The next item starts as soon as a download slot frees up, instead of when the previous items are finished.
- Review fixes and refactoring of the session, the downloader and the item lookup.

## [0.1.0] - 2026-09-28

First release.

### Added

- Library and CLI that log in to Steam anonymously and download Workshop items in parallel, replacing
  `steamcmd +login anonymous +workshop_download_item`; incremental updates, checksums of every chunk and file,
  atomic replacement of files.
- Cancellation (`std::stop_token`, Ctrl-C in the CLI).
- Python bindings (nanobind) and an asyncio client.
- Linux, macOS and Windows builds from vendored static dependencies; release workflow with CLI binaries, wheels and
  an sdist.

[Unreleased]: https://github.com/PyXiion/PxSteamDL/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/PyXiion/PxSteamDL/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/PyXiion/PxSteamDL/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/PyXiion/PxSteamDL/releases/tag/v0.1.0
