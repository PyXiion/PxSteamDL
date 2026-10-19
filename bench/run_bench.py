#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Runs bench-client binaries against bench-server and records wall time, CPU time, peak memory and thread counts.

    run_bench.py --server build-asio/bench/bench-server --pki DIR \\
        --variant curl=baseline/build/bench-client --variant asio=build-asio/bench/bench-client \\
        --out results.json [--reps 9]

The server and the client are pinned to different cores; the client's files go to tmpfs (/dev/shm) so the disk does not
add noise. Variants run interleaved (A B C A B C ...) so that drift in the machine hits all of them alike.
Needs: /etc/hosts pointing api.steampowered.com, cm1.bench.test and cdn[1-4].bench.test at 127.0.0.1, port 443 free,
and both builds made with PXSTEAMDL_CA_BUNDLE_FILE=<pki>/ca.pem.
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import threading
import time

BULK = "1000,1001,1002,1003"
SMALL = "2000,2001,2002,2003"

# name, ids, server latency (ms), parallel_items, threads_per_item
SCENARIOS = [
    ("idle", BULK, 0, 2, 4),  # connected, nothing to download (see --hold-ms below)
    ("bulk-lan", BULK, 0, 2, 4),
    ("bulk-wan", BULK, 25, 2, 4),
    ("small-lan", SMALL, 0, 2, 4),
    ("small-wan", SMALL, 25, 2, 4),
    ("bulk-lan-64", BULK, 0, 4, 16),
    ("small-wan-64", SMALL, 25, 4, 16),
    ("small-wan-256", SMALL, 25, 8, 32),
]


def sample_proc(pid, stop, out):
    """Reads Threads and VmRSS of pid every few ms until stop is set."""
    threads, rss = [], []
    path = f"/proc/{pid}/status"
    while not stop.is_set():
        try:
            with open(path) as f:
                for line in f:
                    if line.startswith("Threads:"):
                        threads.append(int(line.split()[1]))
                    elif line.startswith("VmRSS:"):
                        rss.append(int(line.split()[1]))
        except OSError:
            break
        time.sleep(0.004)
    out["threads"] = threads
    out["rss"] = rss


def run_client(binary, root, ids, parallel, threads, hold_ms, client_cpus, idle_only=False):
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    cmd = ["taskset", "-c", client_cpus, binary, "--root", root, "--ids", ids, "--parallel-items", str(parallel),
           "--threads-per-item", str(threads)]
    if hold_ms:
        cmd += ["--hold-ms", str(hold_ms)]
    if idle_only:
        cmd += ["--idle-only"]
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stop = threading.Event()
    sampled = {}
    sampler = threading.Thread(target=sample_proc, args=(proc.pid, stop, sampled))
    sampler.start()
    stdout = proc.stdout.read()
    stderr = proc.stderr.read()
    _, status, usage = os.wait4(proc.pid, 0)
    wall = time.monotonic() - start
    stop.set()
    sampler.join()
    proc.returncode = os.waitstatus_to_exitcode(status)
    shutil.rmtree(root, ignore_errors=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{binary} failed ({proc.returncode}): {stdout.decode()} {stderr.decode()}")
    report = json.loads(stdout.decode().strip().splitlines()[-1])
    th = sampled.get("threads") or [0]
    rs = sampled.get("rss") or [0]
    return {
        "wall_s": wall,
        "ctor_ms": report["ctor_ms"],
        "download_ms": report["download_ms"],
        "downloaded_bytes": report["downloaded_bytes"],
        "unpacked_bytes": report["unpacked_bytes"],
        "failed": report["failed"],
        "cpu_user_s": usage.ru_utime,
        "cpu_sys_s": usage.ru_stime,
        "cpu_s": usage.ru_utime + usage.ru_stime,
        "peak_rss_mib": usage.ru_maxrss / 1024.0,
        "mean_rss_mib": statistics.mean(rs) / 1024.0,
        "max_threads": max(th),
        "mean_threads": statistics.mean(th),
        "ctx_voluntary": usage.ru_nvcsw,
        "ctx_involuntary": usage.ru_nivcsw,
        "samples": len(th),
    }


def start_server(binary, pki, latency, server_cpus, extra):
    cmd = ["taskset", "-c", server_cpus, binary, "--pki", pki, "--latency-ms", str(latency)] + extra
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    if line.strip() != "READY":
        proc.kill()
        raise RuntimeError("server did not start: " + proc.stderr.read())
    return proc


def stop_server(proc):
    proc.terminate()
    try:
        err = proc.communicate(timeout=10)[1]
    except subprocess.TimeoutExpired:
        proc.kill()
        err = proc.communicate()[1]
    return err


def summarize(values):
    return {
        "median": statistics.median(values),
        "mean": statistics.mean(values),
        "min": min(values),
        "max": max(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--pki", required=True)
    parser.add_argument("--variant", action="append", required=True, help="name=path/to/bench-client")
    parser.add_argument("--out", required=True)
    parser.add_argument("--reps", type=int, default=9)
    parser.add_argument("--only", help="comma-separated scenario names")
    parser.add_argument("--server-cpus", default="0,1")
    parser.add_argument("--client-cpus", default="2,3")
    parser.add_argument("--root", default="/dev/shm/pxsteamdl-bench")
    args = parser.parse_args()

    variants = dict(v.split("=", 1) for v in args.variant)
    scenarios = [s for s in SCENARIOS if not args.only or s[0] in args.only.split(",")]
    results = {"env": {"machine": platform.machine(), "kernel": platform.release(), "cpu": "",
                       "cores": os.cpu_count(), "server_cpus": args.server_cpus, "client_cpus": args.client_cpus,
                       "reps": args.reps, "variants": variants},
               "runs": []}
    with open("/proc/cpuinfo") as f:
        for line in f:
            if line.startswith("model name"):
                results["env"]["cpu"] = line.split(":", 1)[1].strip()
                break

    for latency in sorted({s[2] for s in scenarios}):
        group = [s for s in scenarios if s[2] == latency]
        server = start_server(args.server, args.pki, latency, args.server_cpus, [])
        try:
            for name, ids, _, parallel, threads in group:
                idle = name == "idle"
                hold = 1500 if idle else 0
                # one unmeasured round to warm caches and the page cache of the binaries
                for vname, binary in variants.items():
                    run_client(binary, args.root, ids, parallel, threads, 300 if idle else 0, args.client_cpus, idle)
                for rep in range(args.reps):
                    for vname, binary in variants.items():
                        r = run_client(binary, args.root, ids, parallel, threads, hold, args.client_cpus, idle)
                        r.update({"scenario": name, "variant": vname, "rep": rep, "latency_ms": latency,
                                  "parallel_items": parallel, "threads_per_item": threads})
                        results["runs"].append(r)
                        print(f"{name:14s} {vname:12s} rep {rep}: wall {r['wall_s']:.2f}s cpu {r['cpu_s']:.2f}s "
                              f"rss {r['peak_rss_mib']:.0f}MiB threads {r['max_threads']}", flush=True)
        finally:
            print(stop_server(server), file=sys.stderr)

    summary = {}
    metrics = ["wall_s", "download_ms", "ctor_ms", "cpu_s", "cpu_user_s", "cpu_sys_s", "peak_rss_mib", "mean_rss_mib",
               "max_threads", "mean_threads", "ctx_voluntary", "ctx_involuntary"]
    for run in results["runs"]:
        slot = summary.setdefault(run["scenario"], {}).setdefault(run["variant"], {m: [] for m in metrics})
        for m in metrics:
            slot[m].append(run[m])
    results["summary"] = {sc: {v: {m: summarize(vals) for m, vals in ms.items()} for v, ms in by_v.items()}
                          for sc, by_v in summary.items()}
    with open(args.out, "w") as f:
        json.dump(results, f, indent=1)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
