// SPDX-License-Identifier: LGPL-3.0-or-later
// Offline checks of the argument handling; the download itself is covered by .github/smoke_test.js.
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Client, PxSteamDLError, version } = require('..');
const { toId, boundedInt, guard } = require('../util');

test('exports the library version', () => {
  assert.match(version, /^\d+\.\d+\.\d+/);
});

test('Client.create validates its options', async () => {
  await assert.rejects(Client.create({ connectTimeout: 0 }), RangeError);
  await assert.rejects(Client.create({ stallTimeout: 1.5 }), RangeError);
  await assert.rejects(Client.create({ proxy: 1 }), TypeError);
  await assert.rejects(Client.create({ connectTimeout: 2 ** 32 }), RangeError);
});

test('a logon that cannot connect rejects with PxSteamDLError', async () => {
  await assert.rejects(Client.create({ proxy: 'http://127.0.0.1:1', connectTimeout: 1 }), (error) => {
    assert.ok(error instanceof PxSteamDLError);
    assert.ok(error instanceof Error);
    assert.equal(typeof error.kind, 'string');
    return true;
  });
});

test('Client cannot be constructed directly', () => {
  assert.throws(() => new Client({}), TypeError);
});

test('toId accepts numbers, bigints and decimal strings up to 2^53 - 1', () => {
  assert.equal(toId(2009463077), 2009463077n);
  assert.equal(toId(2009463077n), 2009463077n);
  assert.equal(toId('2009463077'), 2009463077n);
  assert.equal(toId(Number.MAX_SAFE_INTEGER), BigInt(Number.MAX_SAFE_INTEGER));
});

test('toId rejects what would not survive the round trip', () => {
  for (const bad of [-1, 1.5, NaN, 2 ** 53, 2n ** 53n, '1152921504606846977', '', 'abc', '-1', null, undefined, {}]) {
    assert.throws(() => toId(bad), TypeError, String(bad));
  }
});

test('boundedInt enforces both ends', () => {
  assert.equal(boundedInt('x', 1, 8), 1);
  assert.equal(boundedInt('x', 8, 8), 8);
  for (const bad of [0, 9, 2 ** 32, 1.5, '2', NaN]) assert.throws(() => boundedInt('x', bad, 8), RangeError);
});

test('download validates its arguments before touching the network', async () => {
  const client = Object.create(Client.prototype);
  await assert.rejects(client.download('1', 'root'), TypeError);
  await assert.rejects(client.download([1], 42), TypeError);
  await assert.rejects(client.download([1.5], 'root'), TypeError);
  await assert.rejects(client.download([1], 'root', { parallelItems: 2 ** 32 }), RangeError);
  await assert.rejects(client.download([1], 'root', { parallelItems: 1024, threadsPerItem: 1024 }), RangeError);
  await assert.rejects(client.download([1], 'root', { onProgress: 'nope' }), TypeError);
});

test('an exception from a callback becomes an uncaught exception and does not propagate to the caller', async () => {
  const seen = new Promise((resolve) => {
    const original = process.listeners('uncaughtException');
    process.removeAllListeners('uncaughtException');
    process.once('uncaughtException', (error) => {
      for (const listener of original) process.on('uncaughtException', listener);
      resolve(error);
    });
  });
  const wrapped = guard(() => {
    throw new Error('boom');
  });
  assert.doesNotThrow(() => wrapped({}));
  assert.equal((await seen).message, 'boom');
  assert.equal(guard(undefined), undefined);
});
