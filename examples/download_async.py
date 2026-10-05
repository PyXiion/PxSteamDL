# SPDX-License-Identifier: LGPL-3.0-or-later
"""Uses the asyncio API: AsyncClient.create(), download() with callbacks, concurrent downloads, cancellation.

Usage: python download_async.py DIR ITEM_ID...
"""
import asyncio
import sys

import pxsteamdl


async def main() -> int:
    root, ids = sys.argv[1], [int(arg) for arg in sys.argv[2:]]
    try:
        client = await pxsteamdl.AsyncClient.create()  # logs in in a worker thread; takes Client's arguments
    except pxsteamdl.Error as e:
        print(f"cannot log in ({e.kind.name}): {e}")
        return 1

    # Callbacks run on the event loop thread, so they may touch loop state freely.
    def on_resolved(info: pxsteamdl.ItemInfo) -> None:
        print(f"skipped {info.item_id}: {info.error}" if info.error else f"found {info.item_id} {info.title!r}")

    def on_progress(progress: pxsteamdl.Progress) -> None:
        print(f"\r{progress.item_id}: {progress.unpacked_bytes}/{progress.unpacked_total}", end="")

    # One client serves several downloads at once.
    halves = [ids[::2], ids[1::2]]
    downloads = asyncio.gather(
        *(
            client.download(half, root, on_progress=on_progress, on_resolved=on_resolved)
            for half in halves
            if half
        )
    )
    try:
        batches = await downloads
    except asyncio.CancelledError:
        # Cancelling the task stops the downloads and waits for them: finished items stay on disk and
        # temporary files are removed before CancelledError propagates.
        print("\ncancelled")
        raise

    print()
    failed = 0
    for result in (r for batch in batches for r in batch):
        if result.ok:
            print(f"ok     {result.item_id} -> {result.path}")
        else:
            failed += 1
            print(f"failed {result.item_id} [{result.error_kind.name}]: {result.error}")
    return int(failed > 0)


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
