# PxSteamDL

A small C++20 library and CLI that replaces `steamcmd +login anonymous +workshop_download_item 294100 <id>`.
It logs in to Steam anonymously and downloads RimWorld Workshop items in parallel.

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

Dependencies: libcurl ≥ 8.11 (with WebSocket support), OpenSSL 3 (libcrypto), zlib, liblzma, libzstd.
[CPM](https://github.com/cpm-cmake/CPM.cmake) fetches nlohmann_json.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## CLI

```sh
pxsteamdl [-o DIR] [-j PARALLEL_ITEMS] [-t THREADS_PER_ITEM] ITEM_ID...
```

Each item goes into `DIR/<ITEM_ID>/`, which matches steamcmd's `steamapps/workshop/content/294100/<ITEM_ID>`.
The exit code is 1 if any item fails.
Ctrl-C stops the batch: requests in flight finish, unfinished items are reported as `cancelled`, and the exit code is 130.
A second Ctrl-C quits immediately.

## Library

```cpp
#include <pxsteamdl/pxsteamdl.hpp>

pxsteamdl::Client client;  // anonymous logon; throws on failure
std::vector<std::uint64_t> ids{2009463077, 818773962};
pxsteamdl::Options options;
options.on_progress = [](const pxsteamdl::Progress& p) { /* called from worker threads */ };
for (const auto& r : client.download(ids, "mods", options))
    if (!r.error.empty()) std::fprintf(stderr, "%llu: %s\n", (unsigned long long)r.item_id, r.error.c_str());
```

To cancel, pass a `std::stop_token` in `Options::stop`. Once stop is requested, in-flight chunk requests finish,
the remaining work is skipped and every unfinished item reports `Result::error == "cancelled"`; items that already
completed stay successful and `download()` returns normally. As with failures, temporary files are removed and the
existing files of unfinished items are not replaced.

```cpp
std::stop_source stop;
options.stop = stop.get_token();
// from another thread: stop.request_stop();
```

Link against the `PxSteamDL::pxsteamdl` CMake target (e.g. after `add_subdirectory` or `CPMAddPackage`).
The CLI executable is built as `build/pxsteamdl`.

## Python

```sh
pip install git+https://github.com/PyXiion/PxSteamDL   # or, from a checkout: pip install .
```

This builds the extension from source, so it needs the same system dependencies as the C++ build plus a C++20 compiler and CMake.
Wheels are not published: PxSteamDL needs libcurl ≥ 8.11 with WebSocket support, which the manylinux baseline does not provide.

```py
import pxsteamdl

client = pxsteamdl.Client()  # anonymous logon; raises RuntimeError on failure

def on_progress(p: pxsteamdl.Progress) -> None:  # called from worker threads
    print(f"{p.item_id}: {p.bytes_done}/{p.bytes_total}")

for r in client.download([2009463077, 818773962], "mods", on_progress=on_progress):
    print(r.item_id, r.title, r.path if r.ok else r.error)
```

`download` releases the GIL, and one `Client` may be used from several threads.
An exception raised by `on_progress` is reported like an exception in a thread (`sys.unraisablehook`) and does not stop the download.
The package ships type stubs.

`download` also takes `cancel=pxsteamdl.CancelToken()`; calling `token.cancel()` from another thread stops it the same way as `Options::stop` in C++.

### asyncio

`AsyncClient` runs logon and downloads in worker threads, so the event loop keeps running. Several downloads may run concurrently on one client.
`on_progress` is called on the event loop thread.
Cancelling the task (including via `asyncio.wait_for` timeouts) stops the download, waits until in-flight requests finish and temporary files are removed, then raises `CancelledError`.

```py
import asyncio
import pxsteamdl

async def main() -> None:
    client = await pxsteamdl.AsyncClient.create()
    harmony, rest = await asyncio.gather(
        client.download([2009463077], "mods"),
        client.download([818773962], "mods", on_progress=lambda p: print(p.item_id, p.bytes_done)),
    )
    try:
        await asyncio.wait_for(client.download([2016436324], "mods"), timeout=60)
    except asyncio.TimeoutError:  # TimeoutError on Python 3.11+
        print("gave up; run again to resume")

asyncio.run(main())
```

## Behaviour

- Updates are incremental: a file whose size and SHA-1 already match the manifest is not downloaded again. Files and directories that are not in the manifest are deleted, as steamcmd does.
- Files are assembled under a temporary name and renamed into place after the SHA-1 check, so an interrupted run never leaves a truncated file under its real name.
- Every chunk is checked (Adler-32 and size) after it is decrypted and decompressed.
- Manifest paths are untrusted: absolute paths and `..` components are rejected.
- A failed item is reported in `Result::error` and does not stop the other items in the batch.

## License

PxSteamDL is licensed under the GNU Lesser General Public License v3.0 or later (`LGPL-3.0-or-later`).
See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
