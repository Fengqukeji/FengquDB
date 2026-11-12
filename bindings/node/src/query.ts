/**
 * 底层 Query 模型与编码。字节与 bindings/python/fengqudb_select.py 完全一致
 * （字段号、值编码、请求外层）。纯 TypeScript，无原生依赖。
 */
import { Writer, toBytes } from "./wire";
import { QueryResult } from "./result";

export enum Op {
  TRUE = 0, FALSE = 1, AND = 2, OR = 3, NOT = 4, EQ = 5, NE = 6, LT = 7, LE = 8, GT = 9, GE = 10,
  IN = 11, EXISTS = 12, IS_NULL = 13, CONTAINS = 14, STARTS_WITH = 15, ENDS_WITH = 16,
  LIKE = 17, REGEX = 18, ICONTAINS = 19, ILIKE = 20, IREGEX = 21, FUZZY = 22,
}
export enum Aggregate { NONE = 0, COUNT_ALL = 1, COUNT = 2, SUM = 3, MIN = 4, MAX = 5, AVG = 6 }
export enum JoinKind { INNER = 0, LEFT = 1, SEMI = 2, ANTI = 3 }

/** protobuf 字段类型号（与索引字段类型相同）。 */
export const FieldType = {
  RECORD: 0, UINT64: 1, INT64: 2, DOUBLE: 3, BYTES: 4, SINT64: 5, FLOAT: 6,
  FIXED64: 7, SFIXED64: 8, FIXED32: 9, SFIXED32: 10, BOOL: 11,
} as const;

/** 强制编码为 double（裸 number 若是整数会按整数编码）。 */
export class Double {
  constructor(readonly value: number) {}
}
/**
 * 查询值。number 为整数时编码成整数（uint 或 zigzag sint），非整数编码成 double；
 * 需要整数值的 double 请用 `new Double(5)`。bigint 总是整数。
 */
export type QueryValue = null | boolean | bigint | number | string | Uint8Array | Double;

const U64 = (1n << 64n) - 1n;
const I64_MIN = -(1n << 63n);

function encodeValue(v: QueryValue): Uint8Array {
  const w = new Writer(16);
  if (v === null || v === undefined) {
    w.u(1, 0);
  } else if (typeof v === "boolean") {
    w.u(1, 5);
    w.u(2, v ? 1 : 0);
  } else if (typeof v === "bigint" || (typeof v === "number" && Number.isInteger(v))) {
    const b = BigInt(v);
    if (b >= 0n) {
      if (b > U64) throw new RangeError("query integer outside uint64");
      w.u(1, 1);
      w.u(2, b);
    } else {
      if (b < I64_MIN) throw new RangeError("query integer outside int64");
      w.u(1, 2);
      w.u(3, ((b << 1n) ^ (b >> 63n)) & U64);
    }
  } else if (typeof v === "number" || v instanceof Double) {
    const d = typeof v === "number" ? v : v.value;
    if (!Number.isFinite(d)) throw new RangeError("non-finite query number");
    w.u(1, 3);
    w.double(4, d);
  } else {
    w.u(1, 4);
    w.b(5, toBytes(v));
  }
  return w.finish();
}

/** 字段引用：number 为 protobuf 字段号，type 见 FieldType，source 0=主前缀、1..7=连接、255=输出列。 */
export class Field {
  constructor(
    readonly number = 0,
    readonly type = 4,
    readonly source = 0,
    readonly path: readonly number[] = [],
    /** repeated 字段：任一元素满足即为真，只能用于主源 WHERE 叶子条件。 */
    readonly repeated = false,
  ) {}
  encode(): Uint8Array {
    const w = new Writer(24);
    w.u(1, this.source);
    w.u(2, this.type);
    const path = this.path.length ? this.path : this.number ? [this.number] : [];
    for (const n of path) w.u(3, n);
    if (this.repeated) w.u(4, 1);
    return w.finish();
  }
  any(): Field {
    return new Field(this.number, this.type, this.source, this.path, true);
  }
  equals(o: Field): boolean {
    return (
      this.number === o.number && this.type === o.type && this.source === o.source &&
      this.repeated === o.repeated && this.path.length === o.path.length &&
      this.path.every((x, i) => x === o.path[i])
    );
  }
  static key(source = 0): Field { return new Field(0, 4, source); }
  static record(source = 0): Field { return new Field(0, 0, source); }
  static column(index: number): Field { return new Field(index + 1, 0, 255); }
}

export class Expr {
  constructor(
    readonly op: Op = Op.TRUE,
    readonly field: Field = new Field(),
    readonly values: readonly QueryValue[] = [],
    readonly children: readonly Expr[] = [],
    readonly right: Field | null = null,
  ) {}
  encode(depth = 0): Uint8Array {
    if (depth > 64) throw new RangeError("query expression too deep");
    const w = new Writer(64);
    w.u(1, this.op);
    if (this.op >= 5) w.b(2, this.field.encode());
    for (const v of this.values) w.b(3, encodeValue(v));
    for (const c of this.children) w.b(4, c.encode(depth + 1));
    if (this.right !== null) w.b(5, this.right.encode());
    return w.finish();
  }
  and(o: Expr): Expr { return new Expr(Op.AND, undefined, [], [this, o]); }
  or(o: Expr): Expr { return new Expr(Op.OR, undefined, [], [this, o]); }
  not(): Expr { return new Expr(Op.NOT, undefined, [], [this]); }
}

export class Column {
  constructor(
    readonly field: Field = new Field(),
    readonly aggregate: Aggregate = Aggregate.NONE,
    readonly name = "",
    readonly distinct = false,
  ) {}
  encode(): Uint8Array {
    const w = new Writer(32);
    w.b(1, this.field.encode());
    w.u(2, this.aggregate);
    w.b(3, toBytes(this.name));
    w.u(4, this.distinct ? 1 : 0);
    return w.finish();
  }
}

export class Order {
  constructor(readonly field: Field, readonly descending = false, readonly nullsFirst = true) {}
  encode(): Uint8Array {
    const w = new Writer(24);
    w.b(1, this.field.encode());
    w.u(2, this.descending ? 1 : 0);
    w.u(3, this.nullsFirst ? 1 : 0);
    return w.finish();
  }
}

export class Join {
  constructor(
    readonly prefix: Uint8Array | string,
    readonly left: readonly Field[],
    readonly right: readonly Field[],
    readonly kind: JoinKind = JoinKind.INNER,
    readonly on: Expr = new Expr(),
  ) {}
  encode(): Uint8Array {
    const w = new Writer(64);
    w.b(1, toBytes(this.prefix));
    for (const f of this.left) w.b(2, f.encode());
    for (const f of this.right) w.b(3, f.encode());
    w.u(4, this.kind);
    w.b(5, this.on.encode());
    return w.finish();
  }
}

export interface QueryOptions {
  where?: Expr;
  columns?: Column[];
  order?: Order[];
  group?: Field[];
  having?: Expr;
  joins?: Join[];
  limit?: number | bigint;
  offset?: number | bigint;
  memoryBytes?: number | bigint;
  maxCandidates?: number | bigint;
  maxWork?: number | bigint;
  timeoutMs?: number | bigint;
  allowScan?: boolean;
  countTotal?: boolean;
  distinct?: boolean;
  indexHint?: Uint8Array | string;
  after?: Uint8Array;
}

/** FQDB_SELECT / FQDB_SELECT_EXPLAIN 的操作号。 */
export const OP_SELECT = 53;
export const OP_EXPLAIN = 54;

/** 能执行原始 SELECT 请求的对象（Store 与事务）。返回的视图位于原生缓冲上。 */
export interface Selectable {
  selectRaw(request: Uint8Array): Uint8Array;
}

export class Query {
  prefix: Uint8Array | string;
  where: Expr;
  columns: Column[];
  order: Order[];
  group: Field[];
  having: Expr;
  joins: Join[];
  limit: number | bigint;
  offset: number | bigint;
  memoryBytes: number | bigint;
  maxCandidates: number | bigint;
  maxWork: number | bigint;
  timeoutMs: number | bigint;
  allowScan: boolean;
  countTotal: boolean;
  distinct: boolean;
  indexHint: Uint8Array | string;
  after: Uint8Array;

  constructor(prefix: Uint8Array | string, o: QueryOptions = {}) {
    this.prefix = prefix;
    this.where = o.where ?? new Expr();
    this.columns = o.columns ?? [];
    this.order = o.order ?? [];
    this.group = o.group ?? [];
    this.having = o.having ?? new Expr();
    this.joins = o.joins ?? [];
    this.limit = o.limit ?? 100;
    this.offset = o.offset ?? 0;
    this.memoryBytes = o.memoryBytes ?? 16 << 20;
    this.maxCandidates = o.maxCandidates ?? 1000000;
    this.maxWork = o.maxWork ?? 100000000;
    this.timeoutMs = o.timeoutMs ?? 1000;
    this.allowScan = o.allowScan ?? true;
    this.countTotal = o.countTotal ?? false;
    this.distinct = o.distinct ?? false;
    this.indexHint = o.indexHint ?? new Uint8Array(0);
    this.after = o.after ?? new Uint8Array(0);
  }

  /** Query 的 protobuf 编码（请求外层的 value）。 */
  encode(): Uint8Array {
    const w = new Writer(256);
    w.b(1, toBytes(this.prefix));
    w.b(2, this.where.encode());
    for (const c of this.columns) w.b(3, c.encode());
    for (const o of this.order) w.b(4, o.encode());
    for (const g of this.group) w.b(5, g.encode());
    w.b(6, this.having.encode());
    for (const j of this.joins) w.b(7, j.encode());
    const nums = [this.limit, this.offset, this.memoryBytes, this.maxCandidates, this.maxWork, this.timeoutMs,
      this.allowScan ? 1 : 0, this.countTotal ? 1 : 0, this.distinct ? 1 : 0];
    nums.forEach((v, i) => {
      if (typeof v === "number" ? !Number.isSafeInteger(v) || v < 0 : v < 0n || v > U64)
        throw new RangeError("query integer outside uint64");
      w.u(8 + i, v);
    });
    w.b(17, toBytes(this.indexHint));
    w.b(18, this.after);
    return w.finish();
  }

  /** 完整请求（含 fqdb_store_select 的外层）。 */
  request(explain = false): Uint8Array {
    return buildRequest(explain ? OP_EXPLAIN : OP_SELECT, this.encode());
  }

  /** 返回同一查询但从给定续页游标开始。 */
  withAfter(cursor: Uint8Array): Query {
    const q = Object.assign(Object.create(Query.prototype) as Query, this);
    q.after = cursor;
    return q;
  }

  prepare(): PreparedQuery { return new PreparedQuery(this.encode()); }
  execute(db: Selectable): QueryResult { return this.prepare().execute(db); }
  explain(db: Selectable): QueryResult { return this.prepare().explain(db); }

  /** 逐页执行，直到游标为空。每页是独立的 QueryResult。 */
  *pages(db: Selectable): Generator<QueryResult, void, undefined> {
    let q: Query = this;
    for (;;) {
      const page = q.execute(db);
      yield page;
      const cursor = page.cursor;
      if (cursor.length === 0) return;
      q = q.withAfter(cursor);
    }
  }
}

/** 外层请求：operation(1) scope(2) key(3) value(4) argument(5) score(6) limit(7)=256 flags(8)=0。 */
export function buildRequest(operation: number, value: Uint8Array): Uint8Array {
  const w = new Writer(value.length + 40);
  w.u(1, operation);
  w.b(2, new Uint8Array(0));
  w.b(3, new Uint8Array(0));
  w.b(4, value);
  w.fixed64(5, 0n);
  w.double(6, 0);
  w.fixed64(7, 256n);
  w.u(8, 0);
  return w.finish();
}

/** 编码一次、可重复执行的查询。 */
export class PreparedQuery {
  readonly encoded: Uint8Array;
  private selectRequest: Uint8Array | undefined;
  private explainRequest: Uint8Array | undefined;
  constructor(encoded: Uint8Array) {
    this.encoded = encoded.slice();
  }
  /** 完整的 SELECT 请求字节（缓存）。 */
  request(explain = false): Uint8Array {
    if (explain) return (this.explainRequest ??= buildRequest(OP_EXPLAIN, this.encoded));
    return (this.selectRequest ??= buildRequest(OP_SELECT, this.encoded));
  }
  execute(db: Selectable): QueryResult { return new QueryResult(db.selectRaw(this.request(false))); }
  explain(db: Selectable): QueryResult { return new QueryResult(db.selectRaw(this.request(true))); }
}
