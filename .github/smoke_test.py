# SPDX-License-Identifier: LGPL-3.0-or-later
"""Downloads small Workshop items through the installed package, synchronously and with asyncio."""
import asyncio
import faulthandler
import pathlib
import tempfile
import threading

import pxsteamdl

ITEMS = [2009463077, 818773962]  # Harmony, HugsLib

# A hang fails the job with a traceback of every thread instead of running into the CI timeout.
faulthandler.dump_traceback_later(300, exit=True)

with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)

    caller = threading.get_ident()
    resolved = []
    first_progress = {}

    def on_resolved(info):
        assert threading.get_ident() == caller
        assert info.item_id not in first_progress, "progress before on_resolved"
        resolved.append(info.item_id)

    def on_progress(p):
        first_progress.setdefault(p.item_id, len(resolved))

    results = pxsteamdl.Client().download(ITEMS, root / "sync", on_progress=on_progress, on_resolved=on_resolved)
    print(results)
    assert resolved == ITEMS, resolved
    for result in results:
        assert result.ok, result.error
        assert result.error_kind == pxsteamdl.ErrorKind.NONE
        assert (result.path / "About" / "About.xml").is_file()
        assert result.downloaded_bytes > 0 and result.unpacked_bytes > 0

    # A second run finds everything in place and fetches nothing; a repeated ID is answered each time.
    again = pxsteamdl.Client().download(ITEMS + ITEMS[:1], root / "sync")
    assert [r.item_id for r in again] == ITEMS + ITEMS[:1]
    assert all(r.ok and r.downloaded_bytes == 0 and r.unpacked_bytes == 0 for r in again), again

    # An item that does not exist is not found (or at least refused), and says so by kind, not by wording.
    missing = pxsteamdl.Client().download([1], root / "missing")[0]
    assert not missing.ok and missing.error_kind in (pxsteamdl.ErrorKind.NOT_FOUND, pxsteamdl.ErrorKind.REJECTED), missing
    assert not missing.cancelled

    async def download_async():
        client = await pxsteamdl.AsyncClient.create()
        loop_thread = threading.get_ident()
        calls = []

        def on_progress(p):
            assert threading.get_ident() == loop_thread
            calls.append(p)

        results = await client.download(ITEMS[:1], root / "async", on_progress=on_progress)
        print(results)
        assert results[0].ok, results[0].error
        assert (results[0].path / "About" / "About.xml").is_file()
        assert calls

    asyncio.run(download_async())
print("smoke test passed")
