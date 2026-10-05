# SPDX-License-Identifier: LGPL-3.0-or-later
"""Uses all of the synchronous Python API: Client, download() options, callbacks, CancelToken, Result, ErrorKind.

Usage: python download.py DIR ITEM_ID...
"""
import signal
import sys
import threading

import pxsteamdl


def main() -> int:
    root, ids = sys.argv[1], [int(arg) for arg in sys.argv[2:]]
    print("pxsteamdl", pxsteamdl.__version__)

    cancel = pxsteamdl.CancelToken()
    # Ctrl-C stops the download cleanly: finished items stay, the rest report error == "cancelled".
    interrupted = threading.Event()

    def on_sigint(*_: object) -> None:
        interrupted.set()
        cancel.cancel()

    signal.signal(signal.SIGINT, on_sigint)

    # Anonymous login; raises pxsteamdl.Error (a RuntimeError, with .kind) on failure. All arguments are optional:
    # proxy (None: from the environment, "": no proxy), connect_timeout and stall_timeout in seconds.
    try:
        client = pxsteamdl.Client(connect_timeout=10, stall_timeout=30)
    except pxsteamdl.Error as e:
        print(f"cannot log in ({e.kind.name}): {e}")
        return 1

    def on_resolved(info: pxsteamdl.ItemInfo) -> None:
        # Called on the calling thread, once per item, in order, before the item's bytes are downloaded.
        if info.error:
            print(f"skipped {info.item_id}: {info.error}")
        else:
            print(f"found {info.item_id} {info.title!r}")

    lock = threading.Lock()

    def on_progress(progress: pxsteamdl.Progress) -> None:
        # Called from worker threads.
        with lock:
            # downloaded_*: what crossed the network (compressed); unpacked_*: what is written to disk.
            print(
                f"\r{progress.item_id} {progress.title}: downloaded {progress.downloaded_bytes}/{progress.downloaded_total},"
                f" unpacked {progress.unpacked_bytes}/{progress.unpacked_total}",
                end="",
            )

    results = client.download(
        ids,
        root,
        parallel_items=2,
        threads_per_item=4,
        on_progress=on_progress,
        on_resolved=on_resolved,
        cancel=cancel,
    )

    print()
    for result in results:
        if result.ok:
            print(
                f"ok     {result.item_id} {result.title!r} -> {result.path}"
                f" ({result.downloaded_bytes} bytes downloaded, {result.unpacked_bytes} unpacked)"
            )
        else:
            # error_kind says what kind of failure it was, e.g. to retry only the NETWORK ones.
            print(f"failed {result.item_id} [{result.error_kind.name}]: {result.error}")
    if any(not r.ok for r in results):
        return 130 if interrupted.is_set() else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
