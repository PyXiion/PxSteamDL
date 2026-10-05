# PxSteamDL

A small C++20 library and CLI that replaces `steamcmd +login anonymous +workshop_download_item 294100 <id>`.
It logs in to Steam anonymously and downloads RimWorld Workshop items in parallel.

> **Notice:** This project is made for [PyXiion/PxModRim](https://github.com/PyXiion/PxModRim).

**Disclaimer:** PxSteamDL is not affiliated with or endorsed by Valve. It only downloads content that Steam serves to anonymous accounts.

It talks to Steam directly:

- a CM WebSocket session for anonymous logon, the depot key and manifest request codes;
- the public Web API for item details and CDN servers;
- SteamPipe CDN over HTTPS for manifests and chunks.

The protocol follows [DepotDownloader](https://github.com/SteamRE/DepotDownloader) / [SteamKit2](https://github.com/SteamRE/SteamKit) (`-app 294100 -pubfile <id>`).

## Why not an existing library

| Project | Language | Notes |
|---|---|---|
| DepotDownloader / SteamKit2 | C# | Does this exact job (`-pubfile`), but needs the .NET runtime |
| ValvePython/steam, node-steam-user | Python / JS | Full clients, wrong runtime |
| steamdepot, steam-vent | Rust | Depot downloaders, not C++ |
| SteamPP | C++ | Unmaintained SteamKit port with chat/logon only; no depot/CDN support, legacy TCP |

None of them is a maintained C++ library, so this project implements the needed subset.

## Build

Requirements: CMake ≥ 3.24 and a C++20 compiler (GCC, Clang/Apple Clang or MSVC). Linux, macOS and Windows are supported.

Everything else is fetched by [CPM](https://github.com/cpm-cmake/CPM.cmake) at configure time, pinned and hash-checked,
and linked statically: curl (HTTP/1.1, WebSockets), mbedTLS 3.6, zlib, zstd, liblzma and nlohmann_json. No system
libraries beyond the C/C++ runtime are used. Set `CPM_SOURCE_CACHE` to reuse the downloads between build directories.

TLS trusts only the Mozilla CA bundle published by curl (`cacert-<date>.pem`, pinned in `CMakeLists.txt`),
which is compiled into the library; the operating system's certificate store is not consulted.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To build and run the unit tests, add `-DPXSTEAMDL_BUILD_TESTS=ON` and run `ctest --test-dir build`; see
[CONTRIBUTING.md](CONTRIBUTING.md), which also describes the code style.

## CLI

```sh
pxsteamdl [-o DIR] [-j PARALLEL_ITEMS] [-t THREADS_PER_ITEM] [--] ITEM_ID...
```

`pxsteamdl --version` prints the version and `-h`/`--help` the usage (exit code 0); `--` ends the options.

Each item goes into `DIR/<ITEM_ID>/`, which matches steamcmd's `steamapps/workshop/content/294100/<ITEM_ID>`.
The exit code is 1 if any item fails.
Ctrl-C stops the batch: requests in flight finish, unfinished items are reported as `cancelled`, and the exit code is 130.
A second Ctrl-C quits immediately.

Standalone prebuilt binaries for Linux x86_64, macOS arm64, and Windows x64 are attached to [GitHub Releases](https://github.com/PyXiion/PxSteamDL/releases).

## Library

```cpp
#include <pxsteamdl/pxsteamdl.hpp>

pxsteamdl::Client client;  // anonymous logon; throws pxsteamdl::Error on failure
std::vector<std::uint64_t> ids{2009463077, 818773962};
pxsteamdl::Options options;
options.on_resolved = [](const pxsteamdl::ItemInfo& i) { /* title known, no bytes yet; called on this thread */ };
options.on_progress = [](const pxsteamdl::Progress& p) { /* called from worker threads */ };
for (const auto& r : client.download(ids, "mods", options))
    if (!r.ok()) std::fprintf(stderr, "%llu: %s\n", (unsigned long long)r.item_id, r.error.c_str());
```

`download()` returns one `Result` per entry of `ids`, in order. A repeated ID is downloaded once (`on_resolved` is called
once for it) and every occurrence gets the same `Result`. `Result::path` is `root/<id>` for every item, failed ones
included; after a failure it holds the previous copy, if there was one.

**Errors.** `Result::error` is a message for people; its wording may change. To decide what to do, use
`Result::error_kind` (`ItemInfo` has it too, and `Error::kind()` for the exception of `Client`'s constructor):

| `ErrorKind` | Meaning |
|---|---|
| `kNone` | success |
| `kCancelled` | stopped through `Options::stop` (the message is exactly `cancelled`; `cancelled()` tests for it) |
| `kNotFound` | Steam does not have the item |
| `kRejected` | Steam or a CDN refused: private item, access denied, HTTP 4xx |
| `kNetwork` | network or service failure that survived the automatic retries; trying again later may work |
| `kData` | Steam sent data that cannot be used (malformed, checksum mismatch, unsafe path) |
| `kFilesystem` | a local file or directory could not be written |
| `kOther` | anything else |

**Downloaded and unpacked bytes.** `Progress` and `Result` count both: `downloaded_*` is what came over the network
(chunks as the CDN serves them, encrypted and compressed), `unpacked_*` is what was decrypted, decompressed and written
(the size on disk). Use the first for a transfer rate and the second for a progress bar. They cover only what had to be
fetched in this run: an item that is already up to date reports zeros.

**Connection settings.** `pxsteamdl::Client(ClientOptions{...})` takes a `proxy` (not set: libcurl's environment
variables; empty: no proxy), a `connect_timeout` and a `stall_timeout`. A `Client` can be moved.

**Callbacks that throw.** An exception from `on_progress` (worker threads) fails the item it was called for
(`error_kind` `kOther`) and the others go on; one from `on_resolved` propagates out of `download()` after the items
under way have been stopped. In Python, exceptions from either are reported through `sys.unraisablehook` and the
download goes on. `download()` throws `std::invalid_argument` if `parallel_items` or `threads_per_item` is 0.

To cancel, pass a `std::stop_token` in `Options::stop`. Once stop is requested, in-flight chunk requests finish,
the remaining work is skipped and every unfinished item reports `Result::error == "cancelled"`; items that already
completed stay successful and `download()` returns normally. As with failures, temporary files are removed and the
existing files of unfinished items are not replaced.

```cpp
std::stop_source stop;
options.stop = stop.get_token();
// from another thread: stop.request_stop();
```

`pxsteamdl::Version()` returns the version of the library, and `PXSTEAMDL_VERSION_MAJOR`, `_MINOR`, `_PATCH` and
`_STRING` (from `<pxsteamdl/version.hpp>`, included by the main header) that of the headers; in Python it is
`pxsteamdl.__version__`.

Link against the `PxSteamDL::pxsteamdl` CMake target, after `add_subdirectory` or `CPMAddPackage`, or against an
installed copy:

```sh
cmake --install build --prefix /some/prefix
```

```cmake
find_package(PxSteamDL 1.0 CONFIG REQUIRED)  # with -DCMAKE_PREFIX_PATH=/some/prefix
target_link_libraries(app PRIVATE PxSteamDL::pxsteamdl)
```

The installed library is a single static archive that already contains curl, mbedTLS, zlib, liblzma and zstd, so
nothing but the system libraries is needed to link it. (Before 1.0, a new minor version may change the API, so
`find_package` accepts only a version with the same minor number.) The install also contains the CLI (`bin/pxsteamdl`).
Single-configuration generators only.
The CLI executable is built as `build/pxsteamdl`.

## Python

Prebuilt wheels (CPython 3.10–3.14 on Linux x86_64 (manylinux_2_28), macOS arm64 (11.0+) and Windows x64) are attached
to the [GitHub Releases](https://github.com/PyXiion/PxSteamDL/releases); install one with
`pip install <wheel URL or file>`. Alternatively, build from source (needs CMake and a C++20 compiler):

```sh
pip install git+https://github.com/PyXiion/PxSteamDL   # or, from a checkout: pip install .
```

```py
import pxsteamdl

client = pxsteamdl.Client()  # anonymous logon; raises pxsteamdl.Error (a RuntimeError) on failure

def on_progress(p: pxsteamdl.Progress) -> None:  # called from worker threads
    print(f"{p.item_id} {p.title}: {p.unpacked_bytes}/{p.unpacked_total}")

def on_resolved(i: pxsteamdl.ItemInfo) -> None:  # once per item, before its bytes; same thread as download()
    print(f"queued {i.item_id}: {i.title}" if not i.error else f"{i.item_id}: {i.error}")

for r in client.download([2009463077, 818773962], "mods", on_progress=on_progress, on_resolved=on_resolved):
    print(r.item_id, r.title, r.path if r.ok else f"{r.error_kind.name}: {r.error}")
```

The same names exist in Python: `Result.error_kind` is a `pxsteamdl.ErrorKind` (`NONE`, `CANCELLED`, `NOT_FOUND`,
`REJECTED`, `NETWORK`, `DATA`, `FILESYSTEM`, `OTHER`), `Result.cancelled` and `Result.ok` are properties, and
`Client(proxy=None, connect_timeout=10, stall_timeout=30)` takes the connection settings (`proxy=""` means no proxy).
`pxsteamdl.Error` (a `RuntimeError` with a `kind` attribute) is what `Client()` raises when the logon fails.
`Result`, `ItemInfo` and `Progress` can be constructed with keyword arguments, e.g. to test code that takes them.

`download` releases the GIL, and one `Client` may be used from several threads.
Overlapping downloads into the same item directory wait for each other; the wait can be cancelled.
An exception raised by `on_progress` or `on_resolved` is reported like an exception in a thread (`sys.unraisablehook`) and does not stop the download.
The package ships type stubs.

`download` also takes `cancel=pxsteamdl.CancelToken()`; calling `token.cancel()` from another thread stops it the same way as `Options::stop` in C++.

### asyncio

`AsyncClient` runs logon and downloads in worker threads, so the event loop keeps running. `AsyncClient.create()` takes the
arguments of `Client`; `AsyncClient(client)` wraps a `Client` you already have. Several downloads may run concurrently on one client.
`on_progress` and `on_resolved` are called on the event loop thread.
Cancelling the task (including via `asyncio.wait_for` timeouts) stops the download, waits until in-flight requests finish and temporary files are removed, then raises `CancelledError`.

```py
import asyncio
import pxsteamdl

async def main() -> None:
    client = await pxsteamdl.AsyncClient.create()
    harmony, rest = await asyncio.gather(
        client.download([2009463077], "mods"),
        client.download([818773962], "mods", on_progress=lambda p: print(p.item_id, p.unpacked_bytes)),
    )
    try:
        await asyncio.wait_for(client.download([2016436324], "mods"), timeout=60)
    except asyncio.TimeoutError:  # TimeoutError on Python 3.11+
        print("gave up; run again to resume")

asyncio.run(main())
```

## Examples

Complete programs that use the whole API are in [`examples/`](examples): `download.cpp` (C++; built with
`-DPXSTEAMDL_BUILD_EXAMPLES=ON`), `download.py` (synchronous Python) and `download_async.py` (asyncio). Each takes a
directory and item IDs.

## Behaviour

- Updates are incremental: a file whose size and SHA-1 already match the manifest is not downloaded again. Files and directories that are not in the manifest are deleted, as steamcmd does.
- Files are assembled under a temporary name and renamed into place after the SHA-1 check, so an interrupted run never leaves a truncated file under its real name.
- Each item is built in a temporary sibling directory. All files and symlinks are prepared before the old directory
  is moved aside and the new one installed; an installation error restores the old directory. Unchanged files are
  reused by hard link, or copied locally if hard links are unavailable. Changing their executable permissions uses a
  separate copy so a failed update does not change the previous files.
- Downloads coordinate through a persistent `DIR/.<ITEM_ID>.lock` containing the owner's PID and an exclusive kernel
  lock (`flock` on Linux/macOS, `LockFileEx` on Windows). The lock covers planning through installation and cleanup,
  across clients and processes. An occupied destination is waited for, interruptibly. Process exit releases the
  kernel lock automatically; the next owner replaces the recorded PID, even if that PID has since been reused.
  Keep the lock file: deleting it while downloads are running would allow different processes to lock different files.
- Cancellation is checked before installation. Once directory replacement starts it finishes or rolls back; there
  is a brief interval between the two renames when the item directory is absent. Other downloads are locked out,
  but readers that do not take the lock can observe that interval. This is rollback for ordinary errors, not crash
  recovery: a killed process or power loss during installation can leave the old copy in the temporary sibling.
- Every chunk is checked (Adler-32 and size) after it is decrypted and decompressed.
- Manifest paths are untrusted: absolute paths and `..` components are rejected. On Windows, names Windows cannot
  represent faithfully (containing `:` or other reserved characters, device names such as `CON`, trailing dots or spaces) fail the item.
- Symlinks from the manifest are recreated as symlinks. Windows allows creating them only with Developer Mode enabled
  or with administrator rights; otherwise an item that contains symlinks fails with an error saying so.
- Transient failures (HTTP 429 and 5xx, network errors, Steam answering busy, timed out or rate limited) are retried up
  to four times with growing pauses (about 0.5 s, 1 s, 2 s); a CDN download also pauses before it tries the next host.
  Permanent failures (404, a rejected depot key, a corrupt chunk) are not retried, and a stop request ends a pause at once.
- Item details are looked up in batches of 100 while earlier items download: each item is queued as soon as its
  batch is answered. A failed details request fails only the items of its batch.
- A failed item is reported in `Result::error` and does not stop the other items in the batch. Steam's result codes
  are spelled out, e.g. `Steam rejected the item: not found (EResult 9)`.

## License

PxSteamDL is licensed under the GNU Lesser General Public License v3.0 or later (`LGPL-3.0-or-later`).
See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
