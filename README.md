# PxSteamDL

A fast, standalone C++20 library and CLI for downloading RimWorld Steam Workshop items anonymously—a lightweight replacement for:

```sh
steamcmd +login anonymous +workshop_download_item 294100 <id>
```

Built for [PyXiion/PxModRim](https://github.com/PyXiion/PxModRim), with bindings for **Python** (sync + `asyncio`) and **Node.js** (with TypeScript types).

> **Disclaimer:** PxSteamDL is not affiliated with or endorsed by Valve. It only downloads public Workshop content that Steam serves to anonymous accounts.

## Features

- **No `steamcmd` or `.NET` runtime required** — talks directly to Steam's CM WebSocket, public Web API, and SteamPipe CDN over HTTPS.
- **Parallel & incremental** — downloads multiple items and chunks concurrently, skips files whose size and SHA-1 already match, and removes deleted files.
- **Safe updates** — stages each item in a temporary directory (reusing unchanged files via hard links where possible) and swaps it into place atomically, with cross-process file locking on the destination directory.
- **Resilient** — verifies chunk checksums and retries transient network/CDN errors automatically.
- **Cross-platform** — supports Linux (x86_64), macOS (arm64), and Windows (x64).

---

## CLI

Prebuilt standalone binaries for Linux x86_64, macOS arm64, and Windows x64 are available on [GitHub Releases](https://github.com/PyXiion/PxSteamDL/releases).

```sh
pxsteamdl -o mods 2009463077 818773962
```

Each item is saved to `DIR/<ITEM_ID>/`, matching `steamcmd`'s `steamapps/workshop/content/294100/<ITEM_ID>` layout.

### Options

```text
pxsteamdl [-o DIR] [-j PARALLEL_ITEMS] [-t THREADS_PER_ITEM] [--] ITEM_ID...
pxsteamdl --version | -h | --help
```

| Option | Description |
|---|---|
| `-o DIR` | Output directory (default: `.`) |
| `-j PARALLEL_ITEMS` | Number of items to download concurrently |
| `-t THREADS_PER_ITEM` | Download threads per item |
| `--` | End of options |
| `-h`, `--help` | Print usage and exit |
| `--version` | Print version and exit |

- **Ctrl-C** stops the batch gracefully: in-flight requests finish, unfinished items are reported as `cancelled`, and the process exits with `130`. Press Ctrl-C a second time to quit immediately.
- **Exit codes:** `0` on success, `1` if any item failed, `2` on invalid usage, `130` when interrupted.

---

## Python

```sh
pip install pxsteamdl
```

Prebuilt wheels are published on PyPI (and [GitHub Releases](https://github.com/PyXiion/PxSteamDL/releases)) for CPython 3.10–3.14 on Linux x86_64, macOS arm64, and Windows x64. On other platforms, `pip` builds from source (requires CMake ≥ 3.24 and a C++20 compiler).

### Synchronous usage

```py
import pxsteamdl

client = pxsteamdl.Client()  # anonymous logon; raises pxsteamdl.Error on failure

def on_resolved(info: pxsteamdl.ItemInfo) -> None:
    print(f"queued {info.item_id}: {info.title}" if info.ok else f"{info.item_id}: {info.error}")

def on_progress(p: pxsteamdl.Progress) -> None:
    print(f"{p.item_id} {p.title}: {p.unpacked_bytes}/{p.unpacked_total}")

for r in client.download([2009463077, 818773962], "mods", on_resolved=on_resolved, on_progress=on_progress):
    print(r.item_id, r.title, r.path if r.ok else f"{r.error_kind.name}: {r.error}")
```

- **Thread-safe & GIL-free:** `download()` releases the GIL, and one `Client` can be shared across threads.
- **Cancellation:** pass `cancel=pxsteamdl.CancelToken()` and call `token.cancel()` from another thread.
- **Connection settings:** `Client(proxy=None, connect_timeout=10, stall_timeout=30)` (`proxy=""` disables any environment proxy).

### `asyncio` usage

`AsyncClient` runs logon and downloads on worker threads so the event loop stays responsive. Cancelling the task (or hitting `asyncio.wait_for`) stops the download cleanly and cleans up temporary files before raising `CancelledError`.

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
    except asyncio.TimeoutError:
        print("timed out; run again to resume")

asyncio.run(main())
```

Type stubs are included. Full examples are in [`examples/download.py`](examples/download.py) and [`examples/download_async.py`](examples/download_async.py).

---

## Node.js

```sh
npm install pxsteamdl
```

Prebuilt Node-API addons (Node.js 18+ on Linux x64, macOS arm64, and Windows x64) are bundled in the npm package. When installing from git or on another platform, npm compiles the addon from source (requires CMake, a C++20 compiler, and Node.js 20.17+ / 22.9+ for `cmake-js`; if npm blocks install scripts on a source build, approve the script or run `npm run install` inside `node_modules/pxsteamdl`).

```js
const { Client } = require('pxsteamdl');

const client = await Client.create(); // anonymous logon; rejects with PxSteamDLError on failure

const results = await client.download([2009463077, 818773962], 'mods', {
  onResolved: (info) => console.log(info.ok ? `queued ${info.itemId}: ${info.title}` : `${info.itemId}: ${info.error}`),
  onProgress: (p) => console.log(`${p.itemId} ${p.title}: ${p.unpackedBytes}/${p.unpackedTotal}`),
});

for (const r of results) {
  console.log(r.itemId, r.title, r.ok ? r.path : `${r.errorKind}: ${r.error}`);
}
```

- Everything is asynchronous and runs off the event loop; one `Client` can handle concurrent `download()` calls.
- Pass an `AbortSignal` as `signal` to cancel a download cleanly.
- Item IDs accept numbers, `bigint`s, or decimal strings.
- TypeScript declarations are included in [`node/index.d.ts`](node/index.d.ts). See [`examples/download.js`](examples/download.js) for a complete example.

---

## C++ Library

### Quick example

```cpp
#include <pxsteamdl/pxsteamdl.hpp>

pxsteamdl::Client client;  // anonymous logon; throws pxsteamdl::Error on failure

pxsteamdl::Options options;
options.on_resolved = [](const pxsteamdl::ItemInfo& i) { /* called on calling thread */ };
options.on_progress = [](const pxsteamdl::Progress& p) { /* called from worker threads */ };

for (const auto& r : client.download({2009463077, 818773962}, "mods", options)) {
  if (!r.ok()) {
    std::fprintf(stderr, "%llu: %s\n", static_cast<unsigned long long>(r.item_id), r.error.c_str());
  }
}
```

`download()` returns one `Result` per input ID, in order (duplicate IDs are only downloaded once). See [`examples/download.cpp`](examples/download.cpp) for a complete program.

### Cancellation & connection options

- **Cancellation:** pass a `std::stop_token` in `options.stop`. In-flight chunk requests finish, unfinished items report `error_kind == ErrorKind::kCancelled` (`r.cancelled()`), and temporary files are cleaned up without touching existing item directories.
- **Connection settings:** pass `pxsteamdl::ClientOptions{ .proxy = ..., .connect_timeout = ..., .stall_timeout = ... }` to `Client`. Leave `proxy` unset to use libcurl's environment variables, or set it to `""` to disable proxying.
- **Progress bytes:** `Progress` and `Result` track both `downloaded_*` (encrypted/compressed bytes over the wire, good for transfer speed) and `unpacked_*` (bytes written to disk, good for progress bars). Up-to-date items report `0`.

### Error handling

`Result::error` is human-readable and its wording may change. Branch on `Result::error_kind` (also available on `ItemInfo` and `Error::kind()`):

| C++ (`ErrorKind`) | Python (`ErrorKind`) | Node.js (`errorKind`) | Meaning |
|---|---|---|---|
| `kNone` | `NONE` | `'none'` | Success |
| `kCancelled` | `CANCELLED` | `'cancelled'` | Cancelled via stop token / `AbortSignal` |
| `kNotFound` | `NOT_FOUND` | `'notFound'` | Steam does not have the item |
| `kRejected` | `REJECTED` | `'rejected'` | Refused by Steam or CDN (private item, access denied, HTTP 4xx) |
| `kNetwork` | `NETWORK` | `'network'` | Network or service failure after automatic retries |
| `kData` | `DATA` | `'data'` | Malformed data, checksum mismatch, or unsafe path in manifest |
| `kFilesystem` | `FILESYSTEM` | `'filesystem'` | Could not write local file or directory |
| `kOther` | `OTHER` | `'other'` | Anything else |

---

## Building from Source

**Requirements:** CMake ≥ 3.24 and a C++20 compiler (GCC, Clang/Apple Clang, or MSVC).

All dependencies (`curl`, `mbedTLS`, `zlib`, `zstd`, `liblzma`, `nlohmann_json`) are fetched automatically via [CPM](https://github.com/cpm-cmake/CPM.cmake) and linked statically—no system libraries beyond the C/C++ runtime are needed. Set `CPM_SOURCE_CACHE` to reuse downloads across build directories.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the CLI at `build/pxsteamdl`. Add `-DPXSTEAMDL_BUILD_EXAMPLES=ON` to build [`examples/download.cpp`](examples/download.cpp), or `-DPXSTEAMDL_BUILD_TESTS=ON` to build the unit tests (`ctest --test-dir build`).

### Using in a CMake project

Link against `PxSteamDL::pxsteamdl` via `add_subdirectory`, `CPMAddPackage`, or an installed prefix:

```sh
cmake --install build --prefix /some/prefix
```

```cmake
find_package(PxSteamDL 1.0 CONFIG REQUIRED)  # pass -DCMAKE_PREFIX_PATH=/some/prefix
target_link_libraries(app PRIVATE PxSteamDL::pxsteamdl)
```

For code style, architecture notes, and release/versioning rules, see [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Why not an existing library?

| Project | Language | Why not |
|---|---|---|
| DepotDownloader / SteamKit2 | C# | Does this exact job (`-pubfile`), but requires the .NET runtime |
| ValvePython/steam, node-steam-user | Python / JS | Full clients, wrong runtime for a native library |
| steamdepot, steam-vent | Rust | Depot downloaders, not C++ |
| SteamPP | C++ | Unmaintained; chat/logon only with legacy TCP and no depot/CDN support |

---

## License

PxSteamDL is licensed under the GNU Lesser General Public License v3.0 or later (`LGPL-3.0-or-later`).
See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
