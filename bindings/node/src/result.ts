/**
 * 查询结果：直接在原生返回的响应缓冲上解析，不整体复制。
 * 构造时只做一遍顶层扫描（记录每行的偏移）；行内的键和值在访问时才解码。
 * 整数一律解码为 bigint，double 为 number，bytes 为原生缓冲上的视图（需要时 copy）。
 */
import { FengquDBError, Status } from "./errors";
import { native } from "./native";

export type ResultValue = null | bigint | number | boolean | Uint8Array;

function truncated(): FengquDBError {
  return new FengquDBError(Status.CORRUPT, "truncated feature response");
}

/** 解析状态（模块级，避免热路径分配）。 */
let P = 0;

function varint(d: Uint8Array): number {
  let b = d[P++];
  if (b === undefined) throw truncated();
  if (b < 128) return b;
  let x = b & 127;
  let mul = 128;
  for (;;) {
    b = d[P++];
    if (b === undefined) throw truncated();
    x += (b & 127) * mul;
    if (b < 128) return x;
    mul *= 128;
    if (mul > 2 ** 70) throw new FengquDBError(Status.CORRUPT, "invalid feature response");
  }
}

function varintBig(d: Uint8Array): bigint {
  let b = d[P++];
  if (b === undefined) throw truncated();
  if (b < 128) return BigInt(b);
  let x = BigInt(b & 127);
  let shift = 7n;
  for (;;) {
    b = d[P++];
    if (b === undefined) throw truncated();
    x |= BigInt(b & 127) << shift;
    if (b < 128) return x & 0xffffffffffffffffn;
    shift += 7n;
    if (shift >= 70n) throw new FengquDBError(Status.CORRUPT, "invalid feature response");
  }
}

function skip(d: Uint8Array, wire: number): void {
  if (wire === 0) varint(d);
  else if (wire === 1) P += 8;
  else if (wire === 5) P += 4;
  else if (wire === 2) {
    const n = varint(d);
    P += n;
  } else throw new FengquDBError(Status.CORRUPT, "invalid feature response");
}

const f64 = new DataView(new ArrayBuffer(8));

/** 解码 [p, end) 内的一个 QueryValue。 */
function valueAt(d: Uint8Array, p: number, end: number, copy: boolean): ResultValue {
  let kind = 0;
  let u = 0n;
  let i = 0n;
  let real = 0;
  let rawStart = 0;
  let rawEnd = 0;
  P = p;
  while (P < end) {
    const tag = varint(d);
    const n = Math.floor(tag / 8);
    const wire = tag & 7;
    if (wire === 0) {
      if (n === 1) kind = varint(d);
      else if (n === 2) u = varintBig(d);
      else if (n === 3) i = varintBig(d);
      else varint(d);
    } else if (wire === 1) {
      if (P + 8 > end) throw truncated();
      if (n === 4) {
        for (let k = 0; k < 8; k++) f64.setUint8(k, d[P + k]);
        real = f64.getFloat64(0, true);
      }
      P += 8;
    } else if (wire === 2) {
      const len = varint(d);
      if (P + len > end) throw truncated();
      if (n === 5) {
        rawStart = P;
        rawEnd = P + len;
      }
      P += len;
    } else skip(d, wire);
  }
  if (P !== end) throw truncated();
  switch (kind) {
    case 0: return null;
    case 1: return u;
    case 2: return (i >> 1n) ^ -(i & 1n);
    case 3: return real;
    case 4: return copy ? d.slice(rawStart, rawEnd) : d.subarray(rawStart, rawEnd);
    case 5: return u !== 0n;
    default: throw new FengquDBError(Status.CORRUPT, "invalid query value kind");
  }
}

export interface QueryStats {
  candidates?: number; matched?: number; result_total?: number; work?: number; peak_memory?: number;
  payload_reads?: number; join_probes?: number; complete?: boolean;
}
export interface AccessPlan {
  source?: number; strategy?: string; index?: string; reason?: string;
  covering?: boolean; ordered?: boolean; needs_scan?: boolean; sampled_candidates?: number;
}

const utf8 = new TextDecoder();

/** 查询结果的一行；只在所属 QueryResult 未关闭时可用。 */
export class ResultRow {
  /** @internal */
  constructor(private readonly r: QueryResult, private readonly start: number, private readonly end: number) {}

  /** 行键，原生缓冲上的零拷贝视图（结果关闭后长度变 0）。 */
  get key(): Uint8Array {
    const d = this.r.data();
    P = this.start;
    while (P < this.end) {
      const tag = varint(d);
      if ((tag & 7) !== 2) { skip(d, tag & 7); continue; }
      const len = varint(d);
      if (tag === 10) return d.subarray(P, P + len);
      P += len;
    }
    return d.subarray(0, 0);
  }
  /** 行键的独立副本。 */
  keyCopy(): Uint8Array { return this.key.slice(); }

  /** 逐列解码，只解码被请求的第 index 列；bytes 默认是视图，copy=true 时复制。 */
  get(index: number, copy = false): ResultValue {
    const d = this.r.data();
    let seen = 0;
    P = this.start;
    while (P < this.end) {
      const tag = varint(d);
      if ((tag & 7) !== 2) { skip(d, tag & 7); continue; }
      const len = varint(d);
      const stop = P + len;
      if (stop > this.end) throw truncated();
      if (tag === 18) {
        if (seen === index) return valueAt(d, P, stop, copy);
        seen++;
      }
      P = stop;
    }
    throw new RangeError(`column ${index} out of range (row has ${seen})`);
  }
  /** 列数。 */
  get length(): number {
    const d = this.r.data();
    let n = 0;
    P = this.start;
    while (P < this.end) {
      const tag = varint(d);
      if ((tag & 7) !== 2) { skip(d, tag & 7); continue; }
      const len = varint(d);
      if (tag === 18) n++;
      P += len;
    }
    return n;
  }
  /** 一次解码所有列。 */
  values(copy = false): ResultValue[] {
    const d = this.r.data();
    const out: ResultValue[] = [];
    P = this.start;
    while (P < this.end) {
      const tag = varint(d);
      if ((tag & 7) !== 2) { skip(d, tag & 7); continue; }
      const len = varint(d);
      const stop = P + len;
      if (stop > this.end) throw truncated();
      if (tag === 18) {
        const save = stop;
        out.push(valueAt(d, P, stop, copy));
        P = save;
      } else P = stop;
    }
    return out;
  }
}

export class QueryResult implements Iterable<ResultRow> {
  private view: Uint8Array;
  private closed = false;
  private rowOffsets: number[] = [];
  private accessRanges: number[] = [];
  private statsRange: [number, number] | null = null;
  private cursorRange: [number, number] | null = null;
  private cursorCache: Uint8Array | undefined;

  /**
   * @param response `fqdb_store_select` 的原生响应缓冲视图（所有权转交给本对象，close()/GC 时归还）。
   */
  constructor(response: Uint8Array) {
    this.view = response;
    try {
      this.index();
    } catch (e) {
      this.close();
      throw e;
    }
  }

  /** @internal 返回当前有效的缓冲；关闭后抛错。 */
  data(): Uint8Array {
    if (this.closed) throw new FengquDBError(Status.CLOSED, "query result is closed");
    return this.view;
  }

  /** 外层响应：字段 1 是内层 Query 结果；然后一遍顶层扫描，记录行/访问计划/统计/游标范围。 */
  private index(): void {
    const outer = this.view;
    let p = 0;
    let end = outer.length;
    P = 0;
    let found = false;
    while (P < outer.length) {
      const tag = varint(outer);
      if (tag === 10) {
        const len = varint(outer);
        if (P + len > outer.length) throw truncated();
        p = P;
        end = P + len;
        found = true;
        break;
      }
      skip(outer, tag & 7);
    }
    if (!found) throw new FengquDBError(Status.CORRUPT, "missing query response");
    P = p;
    while (P < end) {
      const tag = varint(outer);
      const n = Math.floor(tag / 8);
      if ((tag & 7) !== 2) { skip(outer, tag & 7); continue; }
      const len = varint(outer);
      const stop = P + len;
      if (stop > end) throw truncated();
      if (n === 1) this.rowOffsets.push(P, stop);
      else if (n === 2) this.accessRanges.push(P, stop);
      else if (n === 3) this.statsRange = [P, stop];
      else if (n === 4) this.cursorRange = [P, stop];
      P = stop;
    }
    // 游标很小，构造时复制一份：结果关闭后仍可用于请求下一页。
    this.cursorCache = this.cursorRange
      ? outer.slice(this.cursorRange[0], this.cursorRange[1])
      : new Uint8Array(0);
  }

  /** 行数。 */
  get length(): number { return this.rowOffsets.length / 2; }
  /** 第 i 行（按需解码）。 */
  row(i: number): ResultRow {
    if (!Number.isInteger(i) || i < 0 || i >= this.length) throw new RangeError("row index out of range");
    this.data();
    return new ResultRow(this, this.rowOffsets[2 * i], this.rowOffsets[2 * i + 1]);
  }
  *[Symbol.iterator](): Iterator<ResultRow> {
    for (let i = 0; i < this.length; i++) yield this.row(i);
  }

  /** 原生响应缓冲的字节数。 */
  get byteLength(): number { return this.closed ? 0 : this.view.length; }

  /** 续页游标的副本（空表示没有下一页）；关闭结果后仍可用。 */
  get cursor(): Uint8Array {
    if (this.cursorCache) return this.cursorCache;
    const r = this.cursorRange;
    return (this.cursorCache = r ? this.data().slice(r[0], r[1]) : new Uint8Array(0));
  }

  /** 统计信息（每次访问时从缓冲解码）。 */
  get stats(): QueryStats {
    const out: Record<string, number | boolean> = {};
    const r = this.statsRange;
    if (!r) return out;
    const names = ["", "candidates", "matched", "result_total", "work", "peak_memory", "payload_reads", "join_probes", "complete"];
    const d = this.data();
    P = r[0];
    while (P < r[1]) {
      const tag = varint(d);
      if ((tag & 7) !== 0) { skip(d, tag & 7); continue; }
      const k = Math.floor(tag / 8);
      const x = varint(d);
      if (k >= 1 && k <= 8) out[names[k]] = k === 8 ? x !== 0 : x;
    }
    return out as QueryStats;
  }

  /** 访问计划（EXPLAIN 与 SELECT 都带）。 */
  get access(): AccessPlan[] {
    const names: Record<number, string> = { 1: "source", 2: "strategy", 3: "index", 4: "reason", 5: "covering", 6: "ordered", 7: "needs_scan", 8: "sampled_candidates" };
    const d = this.data();
    const out: AccessPlan[] = [];
    for (let a = 0; a < this.accessRanges.length; a += 2) {
      const plan: Record<string, string | number | boolean> = {};
      const stop = this.accessRanges[a + 1];
      P = this.accessRanges[a];
      while (P < stop) {
        const tag = varint(d);
        const k = Math.floor(tag / 8);
        const wire = tag & 7;
        if (wire === 0) {
          const x = varint(d);
          if (names[k]) plan[names[k]] = k >= 5 && k <= 7 ? x !== 0 : x;
        } else if (wire === 2) {
          const len = varint(d);
          if (names[k]) plan[names[k]] = utf8.decode(d.subarray(P, P + len));
          P += len;
        } else skip(d, wire);
      }
      out.push(plan as AccessPlan);
    }
    return out;
  }

  /** 立即归还原生缓冲；之后访问行会抛 FengquDBError(CLOSED)。幂等。 */
  close(): void {
    if (this.closed) return;
    this.closed = true;
    native().sharedRelease(this.view);
  }
}
