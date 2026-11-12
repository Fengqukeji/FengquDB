import { FengquDBError, Status } from "./errors";
import { Handle, native } from "./native";
import { OP_EXPLAIN, OP_SELECT, PreparedQuery, Query, Selectable } from "./query";
import { QueryResult } from "./result";

/** 键/值输入：Uint8Array（含 Buffer）、ArrayBuffer 或 UTF-8 字符串。 */
export type BytesLike = Uint8Array | ArrayBuffer | string;
/** 毫秒级 Unix 时间（绝对到期时间）；0 或省略表示不过期。 */
export type Millis = number | bigint;

export interface OpenOptions {
  /** "sync"（默认）或 "buffered"（手动 sync）。 */
  durability?: "sync" | "buffered";
  /** 目录不存在时创建（默认 true）。 */
  create?: boolean;
  segmented?: boolean;
  cacheBytes?: Millis;
  deltaBytes?: Millis;
  writerBytes?: Millis;
  transactionBytes?: Millis;
  allTransactionBytes?: Millis;
  /** 共享读取的固定内存预算（默认 128 MiB）；超出返回 LIMIT。 */
  pinnedBytes?: Millis;
  maxSnapshots?: Millis;
}

export interface WriteOptions {
  expiresAtMs?: Millis;
  /** 立即做可靠落盘屏障。 */
  forceReliable?: boolean;
}

export interface StoreInfo {
  version: bigint; durableVersion: bigint; fileBytes: bigint; indexedKeys: bigint; storedKeys: bigint;
  storedValueBytes: bigint; deltaBytes: bigint; cacheBytes: bigint; pinnedBytes: bigint;
  transactionBytes: bigint; recoveredTransactions: bigint; repairedBytes: bigint;
}
export interface MaintenanceInfo {
  snapshotVersion: bigint; finalVersion: bigint; sourceFileBytes: bigint; outputFileBytes: bigint;
  storedKeys: bigint; storedValueBytes: bigint; copiedKeys: bigint; catchupRounds: bigint;
}

/** 复合索引字段：[protobuf 字段号, 类型号(1 uint64,2 int64,3 double,4 bytes,5 sint64,6 float,7 fixed64,8 sfixed64,9 fixed32,10 sfixed32,11 bool)]。 */
export type IndexField = readonly [field: number, type: number];
export const INDEX_UNIQUE = 1;
export const INDEX_REQUIRED = 2;

/** 跨 worker 传递 Store 的一次性令牌，必须被 Store.attach 或 Store.discard 恰好消费一次。 */
export type StoreToken = bigint & { readonly __fqdbStoreToken: unique symbol };

function closedError(what: string): FengquDBError {
  return new FengquDBError(Status.CLOSED, `${what} is closed or released`);
}

/**
 * 共享读取结果：bytes 直接指向原生内存（只读语义，不要写入）。
 * 租约在 release() 或视图被 GC 时归还；释放后 bytes 抛 FengquDBError(CLOSED)，
 * 先前取得的 Uint8Array 及其 subarray 被 detach（长度变 0）。Store 关闭后仍然有效。
 */
export class SharedValue {
  private view: Uint8Array;
  private gone = false;
  /** @internal */
  constructor(view: Uint8Array) {
    this.view = view;
  }
  get bytes(): Uint8Array {
    if (this.gone) throw closedError("shared value");
    return this.view;
  }
  get length(): number {
    return this.bytes.length;
  }
  get released(): boolean {
    return this.gone;
  }
  /** 这份不可变字节的版本号。 */
  get version(): bigint {
    return native().sharedInfo(this.view)[0];
  }
  /** 绝对到期时间（Unix ms），0 为不过期。 */
  get expiresAtMs(): bigint {
    return native().sharedInfo(this.view)[1];
  }
  /** 独立副本。 */
  copy(): Uint8Array {
    return this.bytes.slice();
  }
  /** 归还租约（幂等）。 */
  release(): void {
    if (this.gone) return;
    this.gone = true;
    native().sharedRelease(this.view);
  }
}

/** 释放由 getSharedBytes() 等返回的裸视图（幂等）。 */
export function releaseShared(view: Uint8Array): boolean {
  return native().sharedRelease(view);
}

/** 准备键：键只封送一次；每次读取不再传键。 */
export class PreparedKey {
  private h: Handle | null;
  /** @internal */
  constructor(h: Handle) {
    this.h = h;
  }
  private handle(): Handle {
    if (!this.h) throw closedError("prepared key");
    return this.h;
  }
  /** 共享读取；未命中返回 null。 */
  getShared(): SharedValue | null {
    const v = native().keyGetShared(this.handle());
    return v === null ? null : new SharedValue(v);
  }
  /** 共享读取，返回裸 Uint8Array 视图（少一个包装对象）。用 releaseShared() 或交给 GC 归还。 */
  getSharedBytes(): Uint8Array | null {
    return native().keyGetShared(this.handle());
  }
  /**
   * 同步回调读取：视图只在回调期间有效，回调返回（或抛出）后立即 detach 并归还租约。
   * 未命中时以 null 调用。不为视图注册 finalizer。
   */
  withShared<T>(fn: (bytes: Uint8Array | null) => T): T {
    return native().keyWithShared(this.handle(), fn);
  }
  /** 复制读取。 */
  get(): Uint8Array | null {
    return native().keyGet(this.handle());
  }
  /** 复制到调用方缓冲；返回字节数，未命中 -1，容量不足抛 BUFFER_TOO_SMALL。 */
  getInto(target: Uint8Array): number {
    return native().keyGetInto(this.handle(), target);
  }
  /** 零拷贝、零 JS 分配地读一个字节（index<0 从末尾算）；未命中或越界返回 -1。 */
  peekByte(index: number): number {
    return native().keyPeekByte(this.handle(), index);
  }
  get closed(): boolean {
    return this.h === null;
  }
  close(): void {
    if (!this.h) return;
    const h = this.h;
    this.h = null;
    native().keyClose(h);
  }
}

/**
 * 批量读取器：键只封送一次；read() 复用行描述。
 * 行的 found/length 存放在复用的 Uint32Array 中（无分配）；值通过 value(i)（视图）、
 * copyValue(i, target)（复制）或 forEach 回调访问。视图有效到下一次 read/release/close，
 * 之后被 detach（长度 0），不会悬垂。一个读取器只能由一个线程串行使用；每个 worker 创建自己的读取器。
 */
export class PreparedReader {
  private h: Handle | null;
  private readonly info: Uint32Array;
  private n = 0;
  /** 读取器的键数。 */
  readonly capacity: number;
  /** @internal */
  constructor(store: Handle, keys: BytesLike[]) {
    this.capacity = keys.length;
    this.info = new Uint32Array(2 * keys.length);
    this.h = native().readerCreate(store, keys, this.info);
  }
  private handle(): Handle {
    if (!this.h) throw closedError("reader");
    return this.h;
  }
  /** 读取当前发布视图，返回行数（等于键数）。 */
  read(): number {
    this.n = 0;
    this.n = native().readerRead(this.handle());
    return this.n;
  }
  private check(i: number): void {
    if (!(i >= 0 && i < this.n)) throw new RangeError("row index out of range (call read() first)");
  }
  found(i: number): boolean {
    this.check(i);
    return this.info[2 * i] !== 0;
  }
  /** 值的字节数（未命中为 0；空值也为 0，用 found 区分）。 */
  length(i: number): number {
    this.check(i);
    return this.info[2 * i + 1];
  }
  /** 值的零拷贝视图；未命中 null。 */
  value(i: number): Uint8Array | null {
    return native().readerValue(this.handle(), i);
  }
  /** 复制到调用方缓冲；返回字节数，未命中 -1。 */
  copyValue(i: number, target: Uint8Array): number {
    return native().readerCopy(this.handle(), i, target);
  }
  version(i: number): bigint {
    return native().readerMeta(this.handle(), i)[0];
  }
  expiresAtMs(i: number): bigint {
    return native().readerMeta(this.handle(), i)[1];
  }
  /** 回调式遍历（视图只在本次 read 内有效）。 */
  forEach(fn: (index: number, value: Uint8Array | null) => void): void {
    const h = this.handle();
    const nat = native();
    for (let i = 0; i < this.n; i++) fn(i, nat.readerValue(h, i));
  }
  /** 释放当前结果（保留键），之前交出的视图被 detach。 */
  release(): void {
    native().readerRelease(this.handle());
    this.n = 0;
  }
  get closed(): boolean {
    return this.h === null;
  }
  close(): void {
    if (!this.h) return;
    const h = this.h;
    this.h = null;
    this.n = 0;
    native().readerClose(h);
  }
}

/** 只读快照。 */
export class Snapshot {
  private h: Handle | null;
  /** @internal */
  constructor(h: Handle) {
    this.h = h;
  }
  private handle(): Handle {
    if (!this.h) throw closedError("snapshot");
    return this.h;
  }
  get(key: BytesLike): Uint8Array | null {
    return native().snapshotGet(this.handle(), key);
  }
  getShared(key: BytesLike): SharedValue | null {
    const v = native().snapshotGetShared(this.handle(), key);
    return v === null ? null : new SharedValue(v);
  }
  close(): void {
    if (!this.h) return;
    const h = this.h;
    this.h = null;
    native().snapshotClose(h);
  }
}

/**
 * 事务：单线程使用。commit() 成功后句柄自动关闭；否则请调用 close()（等价回滚）。
 * 未结束的事务会让 Store.close() 推迟真正的关闭，直到事务关闭。
 */
export class Transaction implements Selectable {
  private h: Handle | null;
  /** @internal */
  constructor(h: Handle) {
    this.h = h;
  }
  private handle(): Handle {
    if (!this.h) throw closedError("transaction");
    return this.h;
  }
  /** 读取（包含本事务的未提交写入）；未命中 null。 */
  get(key: BytesLike): Uint8Array | null {
    return native().txnGet(this.handle(), key);
  }
  put(key: BytesLike, value: BytesLike, expiresAtMs?: Millis): void {
    native().txnPut(this.handle(), key, value, expiresAtMs);
  }
  delete(key: BytesLike): void {
    native().txnDelete(this.handle(), key);
  }
  putMany(items: ReadonlyArray<readonly [BytesLike, BytesLike, Millis?]>): void {
    native().txnPutMany(this.handle(), items as Array<[BytesLike, BytesLike, Millis?]>);
  }
  deleteMany(keys: BytesLike[]): void {
    native().txnDeleteMany(this.handle(), keys);
  }
  savepoint(): bigint {
    return native().txnSavepoint(this.handle());
  }
  rollbackTo(id: bigint): void {
    native().txnRollbackTo(this.handle(), id);
  }
  releaseSavepoint(id: bigint): void {
    native().txnReleaseSavepoint(this.handle(), id);
  }
  /** 提交并关闭句柄，返回提交版本。失败时句柄仍打开，需 close()。 */
  commit(forceReliable = false): bigint {
    const version = native().txnCommit(this.handle(), forceReliable);
    this.close();
    return version;
  }
  /** 能看到本事务私有写入的查询。 */
  selectRaw(request: Uint8Array): Uint8Array {
    return native().txnSelect(this.handle(), request);
  }
  select(q: Query | PreparedQuery): QueryResult {
    return q.execute(this);
  }
  get closed(): boolean {
    return this.h === null;
  }
  close(): void {
    if (!this.h) return;
    const h = this.h;
    this.h = null;
    native().txnClose(h);
  }
}

export class Store implements Selectable {
  private h: Handle | null;
  private constructor(h: Handle) {
    this.h = h;
  }

  static open(path: string, options?: OpenOptions): Store {
    let o: Record<string, unknown> | undefined;
    if (options) {
      const { durability, ...rest } = options;
      o = { ...rest, durability: durability === "buffered" ? 1 : 0 };
    }
    return new Store(native().open(path, o));
  }

  /** 在另一个线程（worker_threads）使用同一个 Store：此处取得令牌，传给 worker 后调用 Store.attach。 */
  share(): StoreToken {
    return native().storeShare(this.handle()) as StoreToken;
  }
  static attach(token: StoreToken): Store {
    return new Store(native().storeAttach(token));
  }
  static discard(token: StoreToken): void {
    native().storeDiscard(token);
  }

  private handle(): Handle {
    if (!this.h) throw closedError("store");
    return this.h;
  }
  get closed(): boolean {
    return this.h === null;
  }
  /**
   * 关闭本句柄。已取得的共享视图、准备键和读取器按核心库约定继续有效；
   * 未结束的事务/快照/其它线程的句柄会推迟底层 Store 的真正关闭。
   */
  close(): void {
    if (!this.h) return;
    const h = this.h;
    this.h = null;
    native().storeClose(h);
  }

  put(key: BytesLike, value: BytesLike, options?: WriteOptions): bigint {
    return native().storePut(this.handle(), key, value, options?.expiresAtMs, options?.forceReliable);
  }
  /** 返回是否删除了存在的键。 */
  delete(key: BytesLike, options?: { forceReliable?: boolean }): boolean {
    return native().storeDelete(this.handle(), key, options?.forceReliable);
  }
  /** 复制读取（调用方拥有副本）；未命中 null。 */
  get(key: BytesLike): Uint8Array | null {
    return native().storeGet(this.handle(), key);
  }
  /** 共享读取（零拷贝）；未命中 null。 */
  getShared(key: BytesLike): SharedValue | null {
    const v = native().storeGetShared(this.handle(), key);
    return v === null ? null : new SharedValue(v);
  }
  /** 共享读取，返回裸视图（少一个包装对象）；用 releaseShared() 或交给 GC 归还。 */
  getSharedBytes(key: BytesLike): Uint8Array | null {
    return native().storeGetShared(this.handle(), key);
  }
  /** 同步回调式共享读取，见 PreparedKey.withShared。 */
  withShared<T>(key: BytesLike, fn: (bytes: Uint8Array | null) => T): T {
    return native().storeWithShared(this.handle(), key, fn);
  }
  prepareKey(key: BytesLike): PreparedKey {
    return new PreparedKey(native().keyPrepare(this.handle(), key));
  }
  /** 1..4096 个键。 */
  prepareReader(keys: BytesLike[]): PreparedReader {
    return new PreparedReader(this.handle(), keys);
  }
  transaction(timeoutMs?: Millis): Transaction {
    return new Transaction(native().txnBegin(this.handle(), timeoutMs));
  }
  snapshot(): Snapshot {
    return new Snapshot(native().snapshotCreate(this.handle()));
  }
  sync(): void {
    native().storeSync(this.handle());
  }
  info(): StoreInfo {
    return native().storeInfo(this.handle()) as unknown as StoreInfo;
  }
  defineCompoundIndex(name: BytesLike, keyPrefix: BytesLike, fields: readonly IndexField[], flags = 0): MaintenanceInfo {
    if (fields.length < 1 || fields.length > 8) throw new RangeError("a compound index has 1..8 fields");
    const flat: number[] = [];
    for (const [f, t] of fields) flat.push(f, t);
    return native().storeDefineIndex(this.handle(), name, keyPrefix, flat, flags) as unknown as MaintenanceInfo;
  }
  dropIndex(name: BytesLike): MaintenanceInfo {
    return native().storeDropIndex(this.handle(), name) as unknown as MaintenanceInfo;
  }
  /** 只读快照上的查询请求（原生响应缓冲视图）；一般用 select()/builder.run()。 */
  selectRaw(request: Uint8Array): Uint8Array {
    return native().storeSelect(this.handle(), request);
  }
  select(q: Query | PreparedQuery): QueryResult {
    return q.execute(this);
  }
  explain(q: Query | PreparedQuery): QueryResult {
    return q.explain(this);
  }
}

export { OP_SELECT, OP_EXPLAIN };
