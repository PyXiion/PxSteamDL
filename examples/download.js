// SPDX-License-Identifier: LGPL-3.0-or-later
// Usage: node examples/download.js <directory> <item id>...
'use strict';

const { Client } = require('..');

async function main() {
  const [root, ...ids] = process.argv.slice(2);
  if (!root || ids.length === 0) {
    console.error('usage: node download.js <directory> <item id>...');
    process.exit(2);
  }

  const controller = new AbortController();
  process.once('SIGINT', () => controller.abort());

  const client = await Client.create();
  const results = await client.download(ids, root, {
    signal: controller.signal,
    onResolved: (info) => console.log(info.ok ? `queued ${info.itemId}: ${info.title}` : `${info.itemId}: ${info.error}`),
    onProgress: (p) => process.stdout.write(`\r${p.itemId} ${p.title}: ${p.unpackedBytes}/${p.unpackedTotal}   `),
  });

  console.log();
  let failed = 0;
  for (const r of results) {
    if (r.ok) console.log(r.itemId, r.title, r.path);
    else {
      console.log(r.itemId, r.title, `${r.errorKind}: ${r.error}`);
      failed++;
    }
  }
  process.exitCode = failed ? 1 : 0;
}

main().catch((error) => {
  if (error.name === 'AbortError') console.error('cancelled');
  else console.error(error);
  process.exit(1);
});
