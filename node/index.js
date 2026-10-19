// SPDX-License-Identifier: LGPL-3.0-or-later
'use strict';

const { prebuiltPath } = require('./prebuild');
const { toId, boundedInt, optionalFunction, guard, MAX_WORKERS, MAX_POOL } = require('./util');

// A build from source (node/native) wins over the addon that ships in the package (prebuilds/).
function loadNative() {
  const candidates = ['./native/pxsteamdl-addon.node', prebuiltPath()].filter(Boolean);
  let failure;
  for (const candidate of candidates) {
    try {
      return require(candidate);
    } catch (error) {
      failure ??= error;
    }
  }
  throw new Error(
    'pxsteamdl: the native addon is not available for this platform and was not built. Building needs CMake and a ' +
      'C++20 compiler; newer npm versions skip install scripts of dependencies until they are approved. Run ' +
      '"npm run install" in the pxsteamdl package directory.',
    { cause: failure }
  );
}

const native = loadNative();

/** Thrown by Client.create() when the logon fails and by download() when the whole operation fails; kind says what went wrong. */
class PxSteamDLError extends Error {
  constructor(message, kind) {
    super(message);
    this.name = 'PxSteamDLError';
    this.kind = kind;
  }
}

const CREATE = Symbol('create');

/** Anonymous Steam session. One client may serve several downloads at once. */
class Client {
  #native;

  /** @private Use Client.create(). */
  constructor(nativeClient, token) {
    if (token !== CREATE) throw new TypeError('use Client.create() to make a Client');
    this.#native = nativeClient;
  }

  /**
   * Logs in to Steam anonymously without blocking the event loop; rejects with PxSteamDLError on failure.
   * `proxy`: a proxy URL such as "http://host:3128" or "socks5h://host:1080" for every connection; undefined uses
   * what libcurl finds in the environment (https_proxy, all_proxy), "" uses no proxy.
   */
  static async create({ proxy, connectTimeout = 10, stallTimeout = 30 } = {}) {
    boundedInt('connectTimeout', connectTimeout, 2 ** 31 - 1);
    boundedInt('stallTimeout', stallTimeout, 2 ** 31 - 1);
    if (proxy !== undefined && typeof proxy !== 'string') throw new TypeError('proxy must be a string');
    try {
      return new Client(await native.createClient({ proxy, connectTimeout, stallTimeout }), CREATE);
    } catch (error) {
      throw new PxSteamDLError(error.message, error.kind ?? 'other');
    }
  }

  /**
   * Downloads each item into root/<item id>/, updating existing copies incrementally.
   * Resolves with one result per entry of ids, in order; per-item failures are in result.error and
   * result.errorKind, not exceptions; a failure of the whole operation rejects with PxSteamDLError. If `signal` aborts, the download is stopped and awaited (temporary
   * files removed, finished items kept) and the promise rejects with signal.reason.
   */
  async download(ids, root, { parallelItems = 2, threadsPerItem = 4, onProgress, onResolved, signal } = {}) {
    if (!Array.isArray(ids)) throw new TypeError('ids must be an array');
    if (typeof root !== 'string') throw new TypeError('root must be a string');
    const bigIds = ids.map(toId);
    boundedInt('parallelItems', parallelItems, MAX_WORKERS);
    boundedInt('threadsPerItem', threadsPerItem, MAX_WORKERS);
    if (parallelItems * threadsPerItem > MAX_POOL) {
      throw new RangeError(`parallelItems * threadsPerItem must be at most ${MAX_POOL}`);
    }
    optionalFunction('onProgress', onProgress);
    optionalFunction('onResolved', onResolved);
    signal?.throwIfAborted();

    const token = new native.CancelToken();
    const abort = () => token.cancel();
    signal?.addEventListener('abort', abort, { once: true });
    try {
      let results;
      try {
        results = await this.#native.download(
          bigIds, root, parallelItems, threadsPerItem, guard(onProgress), guard(onResolved), token
        );
      } catch (error) {
        throw new PxSteamDLError(error.message, error.kind ?? 'other');
      }
      signal?.throwIfAborted();
      return results;
    } finally {
      signal?.removeEventListener('abort', abort);
    }
  }
}

module.exports = { Client, PxSteamDLError, version: native.version };
