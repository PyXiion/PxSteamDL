// SPDX-License-Identifier: LGPL-3.0-or-later
'use strict';

const native = require('./native/pxsteamdl.node');

/** Thrown by Client.create() when the logon fails; kind says what went wrong. */
class PxSteamDLError extends Error {
  constructor(message, kind) {
    super(message);
    this.name = 'PxSteamDLError';
    this.kind = kind;
  }
}

function toId(value) {
  if (typeof value === 'bigint') {
    if (value >= 0n && value < 2n ** 64n) return value;
  } else if (typeof value === 'number') {
    if (Number.isSafeInteger(value) && value >= 0) return BigInt(value);
  } else if (typeof value === 'string' && /^[0-9]{1,20}$/.test(value)) {
    const id = BigInt(value);
    if (id < 2n ** 64n) return id;
  }
  throw new TypeError(`invalid item ID: ${String(value)}`);
}

function positiveInt(name, value) {
  if (!Number.isInteger(value) || value < 1) throw new RangeError(`${name} must be an integer >= 1`);
  return value;
}

function optionalFunction(name, value) {
  if (value !== undefined && typeof value !== 'function') throw new TypeError(`${name} must be a function`);
  return value;
}

/** Anonymous Steam session. One client may serve several downloads at once. */
class Client {
  #native;

  constructor(nativeClient) {
    this.#native = nativeClient;
  }

  /**
   * Logs in to Steam anonymously without blocking the event loop; rejects with PxSteamDLError on failure.
   * `proxy`: a proxy URL such as "http://host:3128" or "socks5h://host:1080" for every connection; undefined uses
   * what libcurl finds in the environment (https_proxy, all_proxy), "" uses no proxy.
   */
  static async create({ proxy, connectTimeout = 10, stallTimeout = 30 } = {}) {
    positiveInt('connectTimeout', connectTimeout);
    positiveInt('stallTimeout', stallTimeout);
    if (proxy !== undefined && typeof proxy !== 'string') throw new TypeError('proxy must be a string');
    try {
      return new Client(await native.createClient({ proxy, connectTimeout, stallTimeout }));
    } catch (error) {
      throw new PxSteamDLError(error.message, error.kind ?? 'other');
    }
  }

  /**
   * Downloads each item into root/<item id>/, updating existing copies incrementally.
   * Resolves with one result per entry of ids, in order; per-item failures are in result.error and
   * result.errorKind, not exceptions. If `signal` aborts, the download is stopped and awaited (temporary
   * files removed, finished items kept) and the promise rejects with signal.reason.
   */
  async download(ids, root, { parallelItems = 2, threadsPerItem = 4, onProgress, onResolved, signal } = {}) {
    if (!Array.isArray(ids)) throw new TypeError('ids must be an array');
    if (typeof root !== 'string') throw new TypeError('root must be a string');
    const bigIds = ids.map(toId);
    positiveInt('parallelItems', parallelItems);
    positiveInt('threadsPerItem', threadsPerItem);
    optionalFunction('onProgress', onProgress);
    optionalFunction('onResolved', onResolved);
    signal?.throwIfAborted();

    const token = new native.CancelToken();
    const abort = () => token.cancel();
    signal?.addEventListener('abort', abort, { once: true });
    try {
      const results = await this.#native.download(bigIds, root, parallelItems, threadsPerItem, onProgress, onResolved, token);
      signal?.throwIfAborted();
      return results;
    } finally {
      signal?.removeEventListener('abort', abort);
    }
  }
}

module.exports = { Client, PxSteamDLError, version: native.version };
