# SPDX-License-Identifier: LGPL-3.0-or-later
"""Downloads a small Workshop item through the installed package, synchronously and with asyncio."""
import asyncio
import pathlib
import tempfile
import threading

import pxsteamdl

ITEM = 2009463077

with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)

    results = pxsteamdl.Client().download([ITEM], root / "sync", on_progress=lambda p: None)
    print(results)
    assert results[0].ok, results[0].error
    assert (results[0].path / "About" / "About.xml").is_file()

    async def download_async():
        client = await pxsteamdl.AsyncClient.create()
        loop_thread = threading.get_ident()
        calls = []

        def on_progress(p):
            assert threading.get_ident() == loop_thread
            calls.append(p)

        results = await client.download([ITEM], root / "async", on_progress=on_progress)
        print(results)
        assert results[0].ok, results[0].error
        assert (results[0].path / "About" / "About.xml").is_file()
        assert calls

    asyncio.run(download_async())
