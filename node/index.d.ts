// SPDX-License-Identifier: LGPL-3.0-or-later

/** Why an item or an operation failed; the messages are for people and may change, the kinds are the API. */
export type ErrorKind =
  | 'none'
  | 'cancelled' // stopped on request; the message is "cancelled"
  | 'notFound' // Steam does not have the item
  | 'rejected' // Steam or a CDN refused (private item, access denied, HTTP 4xx)
  | 'network' // failed even after the automatic retries; trying again later may work
  | 'data' // Steam sent data that cannot be used
  | 'filesystem' // a local file or directory could not be written
  | 'other';

/** Download progress of one item. Counts cover only the data that had to be fetched in this run. */
export interface Progress {
  itemId: number;
  title: string;
  /** Bytes received over the network so far (the chunks as the CDN serves them: encrypted, compressed). */
  downloadedBytes: number;
  /** How many bytes will be received in all. */
  downloadedTotal: number;
  /** Bytes decrypted, decompressed and written to disk so far. */
  unpackedBytes: number;
  /** How many bytes will be written in all. */
  unpackedTotal: number;
}

/** What Steam says about an item, known before the item's bytes are downloaded. Not a download outcome. */
export interface ItemInfo {
  itemId: number;
  title: string;
  /** Empty if Steam accepted the item; otherwise why it was rejected, or "cancelled". */
  error: string;
  errorKind: ErrorKind;
  ok: boolean;
  cancelled: boolean;
}

/** Outcome of downloading one item. */
export interface Result {
  itemId: number;
  title: string;
  /** The item's directory, root/<item id>; set for failed items too (it then holds the previous copy). */
  path: string;
  /** Empty on success. Match on errorKind, the wording may change. */
  error: string;
  errorKind: ErrorKind;
  /** Bytes this run received over the network for the item; 0 if up to date. */
  downloadedBytes: number;
  /** Bytes this run decrypted, decompressed and wrote for the item; 0 if up to date. */
  unpackedBytes: number;
  ok: boolean;
  cancelled: boolean;
}

export interface ClientOptions {
  /**
   * A proxy URL such as "http://host:3128" or "socks5h://host:1080" for every connection. Undefined uses what
   * libcurl finds in the environment (https_proxy, all_proxy), "" uses no proxy.
   */
  proxy?: string;
  /** Seconds to wait for a connection (at least 1). Default 10. */
  connectTimeout?: number;
  /** Seconds a transfer may stay below 1 byte/s before it is given up and retried (at least 1). Default 30. */
  stallTimeout?: number;
}

export interface DownloadOptions {
  /** Items downloaded side by side, 1 to 1024. Default 2. */
  parallelItems?: number;
  /** Chunk workers per parallel item, shared across all items, 1 to 1024; parallelItems * threadsPerItem is at most 256. Default 4. */
  threadsPerItem?: number;
  /** Called on the main thread. An exception it throws is an uncaught exception and does not stop the download. */
  onProgress?: (progress: Progress) => void;
  /** Called once per item, in order, before the item's bytes are downloaded. Same exception rules as onProgress. */
  onResolved?: (info: ItemInfo) => void;
  /** Aborting stops the download; the promise rejects with signal.reason once everything has stopped. */
  signal?: AbortSignal;
}

/** Item IDs are numbers, bigints or decimal strings, at most Number.MAX_SAFE_INTEGER (results carry them as numbers). */
export type ItemId = number | bigint | string;

/** Thrown by Client.create() when the logon fails, and by download() when the whole operation fails. */
export class PxSteamDLError extends Error {
  kind: ErrorKind;
  constructor(message: string, kind: ErrorKind);
}

/** Anonymous Steam session. One client may serve several downloads at once. */
export class Client {
  private constructor();
  /** Logs in to Steam anonymously without blocking the event loop; rejects with PxSteamDLError on failure. */
  static create(options?: ClientOptions): Promise<Client>;
  /**
   * Downloads each item into root/<item id>/, updating existing copies incrementally. Resolves with one result
   * per entry of ids, in order; per-item failures are in result.error and result.errorKind, not exceptions.
   */
  download(ids: readonly ItemId[], root: string, options?: DownloadOptions): Promise<Result[]>;
}

/** The version of the library. */
export const version: string;
