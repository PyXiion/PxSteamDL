# Experiment: network layer on standalone asio

An experiment, not a release candidate. The libcurl network layer is replaced by standalone asio
(C++20 coroutines, one io thread, OpenSSL for TLS). mbedTLS stays only for AES and SHA1.

## What changed

- `src/net.*`: io thread, TLS context, DNS cache, keep-alive pool, `Connection` with a stall watchdog.
- `src/http.*`: HTTP/1.1 client (content-length, chunked, read-to-close, redirects, keep-alive reuse).
- `src/session.*`: hand-written RFC 6455 WebSocket for the CM connection; `rpc`/`request` are awaitables.
- `src/downloader.cpp`: planners and workers are coroutines on the io thread. Blocking work (manifest parse,
  decode, file writes, progress callbacks, finalize) runs on an `asio::thread_pool`.
- `tests/http_test.cpp`: 16 tests of the real HTTP client against a scripted loopback server (plain http).

## Not covered

- Proxies (explicit and from the environment). A non-empty `ClientOptions::proxy` throws.
- Response compression, HTTP/2, happy eyeballs (IPv4 first).
- Install/merge CMake, the CI binary-size step, the Python binding and the CLI are not updated or verified.
- README and CHANGELOG still describe curl.
- TLS and WebSocket are exercised only by the benchmark server, not by unit tests.

## Benchmark

`bench/` holds a local TLS server that imitates the Web API, a CDN and a CM WebSocket, with a synthetic corpus
(encrypted ZIP-deflate chunks), a client that uses only the public API, and `run_bench.py`.
Results of the run behind the infographic: `bench/results/results.json` (9 repetitions, medians in `summary`).

Variants: curl + mbedTLS (release 0.4.0), curl + OpenSSL (control, same TLS library as asio), asio + OpenSSL.
Variants run interleaved; the server is pinned to cores 0-1, the client to cores 2-3; files go to `/dev/shm`.

Limits: one 4-core machine, server on the same host, localhost, synthetic data, ZIP-deflate only, "WAN" is
25 ms of server-side delay.

### Reproduce

```sh
bench/gen-pki.sh $PKI
# as root: map api.steampowered.com cm1.bench.test cdn1..4.bench.test to 127.0.0.1 in /etc/hosts

# asio build (this branch)
cmake -S . -B build-asio -G Ninja -DCMAKE_BUILD_TYPE=Release -DPXSTEAMDL_BUILD_BENCH=ON \
      -DPXSTEAMDL_CA_BUNDLE_FILE=$PKI/ca.pem
cmake --build build-asio

# baseline: checkout 23bacc6 (0.4.0), then
git apply bench/baseline.patch   # CA override + bench-client target; set PXSTEAMDL_BENCH_CLIENT=<abs path to bench/client.cpp>
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPXSTEAMDL_CA_BUNDLE_FILE=$PKI/ca.pem \
      -DPXSTEAMDL_BENCH_CLIENT=/abs/path/bench/client.cpp
# curl + OpenSSL variant: additionally build curl against OpenSSL in the baseline tree

python3 bench/run_bench.py --server build-asio/bench/bench-server --pki $PKI \
  --variant curl-mbedtls=baseline/build/bench-client --variant asio=build-asio/bench/bench-client \
  --out results.json --reps 9
```
