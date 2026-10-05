# SPDX-License-Identifier: LGPL-3.0-or-later
"""asyncio front end for Client."""

from __future__ import annotations

import asyncio
import contextlib
import functools
import os
from collections.abc import Callable, Sequence

from ._pxsteamdl import CancelToken, Client, ItemInfo, Progress, Result


class AsyncClient:
    """Anonymous Steam session for asyncio; downloads run in worker threads without blocking the loop.

    Several downloads may run concurrently on one client (e.g. with asyncio.gather).
    """

    def __init__(self, client: Client) -> None:
        """Wraps an existing Client; use create() to log in without blocking the event loop."""
        self._client = client

    @classmethod
    async def create(
        cls,
        *,
        proxy: str | None = None,
        connect_timeout: int = 10,
        stall_timeout: int = 30,
    ) -> AsyncClient:
        """Logs in to Steam anonymously in a worker thread; raises Error (a RuntimeError) on failure.

        The arguments are those of Client.
        """
        return cls(
            await asyncio.to_thread(Client, proxy=proxy, connect_timeout=connect_timeout, stall_timeout=stall_timeout)
        )

    async def download(
        self,
        ids: Sequence[int],
        root: str | os.PathLike[str],
        *,
        parallel_items: int = 2,
        threads_per_item: int = 4,
        on_progress: Callable[[Progress], object] | None = None,
        on_resolved: Callable[[ItemInfo], object] | None = None,
    ) -> list[Result]:
        """Downloads each item into root/<item id>/, like Client.download.

        on_progress and on_resolved are called on the event loop thread; on_resolved fires once per item as soon
        as its title is known, before that item's bytes are downloaded. If the task is cancelled, the download is
        stopped and awaited (temporary files removed, finished items kept) before CancelledError propagates.
        """
        loop = asyncio.get_running_loop()
        callback = None
        if on_progress is not None:

            def callback(progress: Progress) -> None:
                loop.call_soon_threadsafe(on_progress, progress)

        token = CancelToken()
        future = loop.run_in_executor(
            None,
            functools.partial(
                self._client.download,
                ids,
                root,
                parallel_items=parallel_items,
                threads_per_item=threads_per_item,
                on_progress=callback,
                on_resolved=None if on_resolved is None else functools.partial(loop.call_soon_threadsafe, on_resolved),
                cancel=token,
            ),
        )
        try:
            return await asyncio.shield(future)
        except asyncio.CancelledError:
            token.cancel()
            while not future.done():
                # Keep waiting through repeated cancellation so the files are consistent on return.
                with contextlib.suppress(asyncio.CancelledError):
                    await asyncio.wait([future])
            future.exception()  # mark retrieved; the cancellation wins
            raise
