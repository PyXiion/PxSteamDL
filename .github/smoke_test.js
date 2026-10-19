// SPDX-License-Identifier: LGPL-3.0-or-later
// Downloads small Workshop items through the Node.js package.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { Client } = require('..');

const ITEMS = [2009463077, 818773962]; // Harmony, HugsLib

// A hang fails the job instead of running into the CI timeout.
setTimeout(() => {
  console.error('smoke test timed out');
  process.exit(1);
}, 300_000).unref();

async function main() {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'pxsteamdl-'));
  try {
    const client = await Client.create();

    const resolved = [];
    const firstProgress = new Map();
    const results = await client.download(ITEMS, path.join(tmp, 'a'), {
      onResolved: (info) => resolved.push(info.itemId),
      onProgress: (p) => {
        if (!firstProgress.has(p.itemId)) firstProgress.set(p.itemId, resolved.length);
        assert.ok(resolved.includes(p.itemId), 'progress before onResolved');
      },
    });
    console.log(results);
    assert.deepEqual(resolved, ITEMS);
    assert.ok(firstProgress.size > 0);
    for (const r of results) {
      assert.ok(r.ok, r.error);
      assert.equal(r.errorKind, 'none');
      assert.ok(fs.statSync(path.join(r.path, 'About', 'About.xml')).isFile());
      assert.ok(r.downloadedBytes > 0 && r.unpackedBytes > 0);
    }

    // A second run finds everything in place and fetches nothing; a repeated ID is answered each time.
    const again = await client.download([...ITEMS, ITEMS[0]], path.join(tmp, 'a'));
    assert.deepEqual(again.map((r) => r.itemId), [...ITEMS, ITEMS[0]]);
    assert.ok(again.every((r) => r.ok && r.downloadedBytes === 0 && r.unpackedBytes === 0), JSON.stringify(again));

    // An item that does not exist is not found (or at least refused), and says so by kind, not by wording.
    const [missing] = await client.download([1], path.join(tmp, 'missing'));
    assert.ok(!missing.ok && ['notFound', 'rejected'].includes(missing.errorKind), JSON.stringify(missing));

    // Aborting stops the download and rejects once it has stopped.
    const controller = new AbortController();
    const aborted = client.download([2016436324], path.join(tmp, 'abort'), { signal: controller.signal });
    controller.abort();
    await assert.rejects(aborted, { name: 'AbortError' });

    // Concurrent downloads on one client.
    const [one, two] = await Promise.all([
      client.download([ITEMS[0]], path.join(tmp, 'b')),
      client.download([ITEMS[1]], path.join(tmp, 'b')),
    ]);
    assert.ok(one[0].ok && two[0].ok);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  console.log('smoke test passed');
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
