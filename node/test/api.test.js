// SPDX-License-Identifier: LGPL-3.0-or-later
// Offline checks of the argument handling; the download itself is covered by .github/smoke_test.js.
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Client, PxSteamDLError, version } = require('..');

test('exports the library version', () => {
  assert.match(version, /^\d+\.\d+\.\d+/);
});

test('Client.create validates its options', async () => {
  await assert.rejects(Client.create({ connectTimeout: 0 }), RangeError);
  await assert.rejects(Client.create({ stallTimeout: 1.5 }), RangeError);
  await assert.rejects(Client.create({ proxy: 1 }), TypeError);
});

test('a logon that cannot connect rejects with PxSteamDLError', async () => {
  await assert.rejects(Client.create({ proxy: 'http://127.0.0.1:1', connectTimeout: 1 }), (error) => {
    assert.ok(error instanceof PxSteamDLError);
    assert.ok(error instanceof Error);
    assert.equal(typeof error.kind, 'string');
    return true;
  });
});
