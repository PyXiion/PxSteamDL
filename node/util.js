// SPDX-License-Identifier: LGPL-3.0-or-later
'use strict';

// Steam item IDs fit in 53 bits in practice; results carry them as numbers, so larger ones are refused up front
// instead of coming back rounded.
function toId(value) {
  let id;
  if (typeof value === 'bigint') id = value;
  else if (typeof value === 'number' && Number.isSafeInteger(value)) id = BigInt(value);
  else if (typeof value === 'string' && /^[0-9]{1,20}$/.test(value)) id = BigInt(value);
  if (id === undefined || id < 0n || id > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new TypeError(`invalid item ID: ${String(value)}`);
  }
  return id;
}

const MAX_WORKERS = 1024;

function boundedInt(name, value, max) {
  if (!Number.isInteger(value) || value < 1 || value > max) {
    throw new RangeError(`${name} must be an integer from 1 to ${max}`);
  }
  return value;
}

function optionalFunction(name, value) {
  if (value !== undefined && typeof value !== 'function') throw new TypeError(`${name} must be a function`);
  return value;
}

// A callback that throws must not be lost: the native layer would only warn (DEP0168) or drop the exception,
// depending on the Node version and flags. Rethrow it from a microtask, which makes it an uncaught exception
// everywhere, and let the download go on.
function guard(callback) {
  if (callback === undefined) return undefined;
  return (argument) => {
    try {
      callback(argument);
    } catch (error) {
      queueMicrotask(() => {
        throw error;
      });
    }
  };
}

module.exports = { toId, boundedInt, optionalFunction, guard, MAX_WORKERS };
