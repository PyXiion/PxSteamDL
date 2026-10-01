# SPDX-License-Identifier: LGPL-3.0-or-later
"""Uses all of the synchronous Python API: Client, download() options, callbacks, CancelToken, Result.

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

    client = pxsteamdl.Client()  # anonymous login; raises RuntimeError on failure

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
            print(f"\r{progress.item_id} {progress.title}: {progress.bytes_done}/{progress.bytes_total}", end="")

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
            print(f"ok     {result.item_id} {result.title!r} -> {result.path}")
        else:
            print(f"failed {result.item_id}: {result.error}")
    if any(not r.ok for r in results):
        return 130 if interrupted.is_set() else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
