import * as fs from "node:fs";
import * as path from "node:path";
import { FengquDBError } from "./errors";

/** 原生句柄是不透明对象。 */
export type Handle = object;
export type Bytes = Uint8Array | ArrayBuffer | string;

/** N-API 扩展导出的函数表（见 native/*.cpp）。 */
export interface Native {
  abiVersion(): number;
  setErrorClass(cls: new (status: number, message: string) => Error): void;
  open(path: string, options?: Record<string, unknown>): Handle;
  storeClose(s: Handle): void;
  storeIsClosed(s: Handle): boolean;
  storeShare(s: Handle): bigint;
  storeAttach(token: bigint): Handle;
  storeDiscard(token: bigint): void;
  storePut(s: Handle, key: Bytes, value: Bytes, expiresAtMs?: number | bigint, force?: boolean): bigint;
  storeDelete(s: Handle, key: Bytes, force?: boolean): boolean;
  storeGet(s: Handle, key: Bytes): Uint8Array | null;
  storeGetShared(s: Handle, key: Bytes): Uint8Array | null;
  storeWithShared<T>(s: Handle, key: Bytes, fn: (v: Uint8Array | null) => T): T;
  storeSync(s: Handle): void;
  storeInfo(s: Handle): Record<string, bigint>;
  storeDefineIndex(s: Handle, name: Bytes, prefix: Bytes, fields: number[], flags: number): Record<string, bigint>;
  storeDropIndex(s: Handle, name: Bytes): Record<string, bigint>;
  storeSelect(s: Handle, request: Uint8Array): Uint8Array;
  sharedRelease(view: Uint8Array): boolean;
  sharedInfo(view: Uint8Array): [bigint, bigint, boolean];
  keyPrepare(s: Handle, key: Bytes): Handle;
  keyClose(k: Handle): void;
  keyGetShared(k: Handle): Uint8Array | null;
  keyWithShared<T>(k: Handle, fn: (v: Uint8Array | null) => T): T;
  keyGet(k: Handle): Uint8Array | null;
  keyGetInto(k: Handle, target: Uint8Array): number;
  keyPeekByte(k: Handle, index: number): number;
  readerCreate(s: Handle, keys: Bytes[], info: Uint32Array): Handle;
  readerRead(r: Handle): number;
  readerValue(r: Handle, i: number): Uint8Array | null;
  readerCopy(r: Handle, i: number, target: Uint8Array): number;
  readerMeta(r: Handle, i: number): [bigint, bigint];
  readerRelease(r: Handle): void;
  readerClose(r: Handle): void;
  txnBegin(s: Handle, timeoutMs?: number | bigint): Handle;
  txnClose(t: Handle): void;
  txnGet(t: Handle, key: Bytes): Uint8Array | null;
  txnPut(t: Handle, key: Bytes, value: Bytes, expiresAtMs?: number | bigint): void;
  txnDelete(t: Handle, key: Bytes): void;
  txnPutMany(t: Handle, items: Array<[Bytes, Bytes, (number | bigint)?]>): void;
  txnDeleteMany(t: Handle, keys: Bytes[]): void;
  txnCommit(t: Handle, force?: boolean): bigint;
  txnSavepoint(t: Handle): bigint;
  txnRollbackTo(t: Handle, id: bigint): void;
  txnReleaseSavepoint(t: Handle, id: bigint): void;
  txnSelect(t: Handle, request: Uint8Array): Uint8Array;
  snapshotCreate(s: Handle): Handle;
  snapshotClose(s: Handle): void;
  snapshotGet(s: Handle, key: Bytes): Uint8Array | null;
  snapshotGetShared(s: Handle, key: Bytes): Uint8Array | null;
}

const FILE = "fengqudb_native.node";

function candidates(): string[] {
  const list: string[] = [];
  if (process.env.FQDB_NATIVE_PATH) list.push(process.env.FQDB_NATIVE_PATH);
  // dist/src/native.js -> 包根目录是 ../..
  const root = path.resolve(__dirname, "..", "..");
  list.push(path.join(root, "build", FILE), path.join(root, "build", "Release", FILE));
  return list;
}

function load(): Native {
  const tried: string[] = [];
  for (const file of candidates()) {
    tried.push(file);
    if (!fs.existsSync(file)) continue;
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const addon = require(file) as Native;
    addon.setErrorClass(FengquDBError);
    return addon;
  }
  throw new Error(
    `FengquDB native addon not found. Build it (npm run build:native) or set FQDB_NATIVE_PATH. Tried: ${tried.join(", ")}`,
  );
}

/** 懒加载：仅查询编码等纯 TS 功能不需要原生库。 */
let cached: Native | undefined;
export function native(): Native {
  return (cached ??= load());
}
