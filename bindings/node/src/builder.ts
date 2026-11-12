/**
 * 类型化查询构建器（等价于 bindings/python/fengqudb_query.py）。
 *
 *   const uid = u64(1), cls = text(2), level = u64(3), gold = s64(4);
 *   const rows = from("p/").where(cls.eq("mage").and(level.between(20, 60)).and(gold.ge(1000)))
 *     .select(uid, gold).orderBy(gold.desc(), uid).limit(20).run(store);
 *   for (const row of rows) console.log(row.get(uid), row.get(gold));  // bigint | null
 *
 * JS 不能重载运算符，条件用 eq/ne/lt/le/gt/ge 与 and/or/not 组合。
 */
import {
  Aggregate, Column, Double, Expr, Field, FieldType, Join, JoinKind, Op, Order, PreparedQuery, Query,
  QueryValue, Selectable,
} from "./query";
import { QueryResult, ResultRow, ResultValue, AccessPlan, QueryStats } from "./result";

export type IntLike = number | bigint;

/** 条件：在构建查询时对已选择的列解析（HAVING 里的聚合会变成输出列引用）。 */
export class Cond {
  /** @internal */
  constructor(private readonly resolve: (columns: readonly Column[]) => Expr, readonly aggregate = false) {}
  build(columns: readonly Column[]): Expr { return this.resolve(columns); }
  and(o: Cond): Cond { return new Cond((c) => this.build(c).and(o.build(c)), this.aggregate || o.aggregate); }
  or(o: Cond): Cond { return new Cond((c) => this.build(c).or(o.build(c)), this.aggregate || o.aggregate); }
  not(): Cond { return new Cond((c) => this.build(c).not(), this.aggregate); }
}
export function and(...cs: Cond[]): Cond { return cs.reduce((a, b) => a.and(b)); }
export function or(...cs: Cond[]): Cond { return cs.reduce((a, b) => a.or(b)); }
export function not(c: Cond): Cond { return c.not(); }

/** 排序项。 */
export class Sort {
  /** @internal */
  constructor(readonly build: (columns: readonly Column[]) => Order) {}
}

type Cast = ((v: unknown) => QueryValue) | undefined;

function intCast(v: unknown): QueryValue {
  if (typeof v === "boolean" || typeof v === "bigint") return v;
  if (typeof v === "number") {
    if (!Number.isInteger(v)) throw new TypeError("integer column compared with a non-integer number");
    return v;
  }
  return v as QueryValue;
}
function doubleCast(v: unknown): QueryValue {
  if (typeof v === "number" || typeof v === "bigint") return new Double(Number(v));
  return v as QueryValue;
}

/** 某个源（0=主前缀，1..7=按顺序的连接）的类型化字段。 */
export class Col<V = QueryValue> {
  /** @internal */
  constructor(readonly field: Field, private readonly cast: Cast = undefined) {}

  /** repeated 字段：任一元素满足即为真。 */
  any(): Col<V> { return new Col<V>(this.field.any(), this.cast); }

  private lit(v: unknown): QueryValue {
    return this.cast ? this.cast(v) : (v as QueryValue);
  }
  private test(op: Op, ...values: unknown[]): Cond {
    const vs = values.map((v) => this.lit(v));
    return new Cond(() => new Expr(op, this.field, vs));
  }
  private cmp(op: Op, other: V | Col<unknown>): Cond {
    if (other instanceof Col) return new Cond(() => new Expr(op, this.field, [], [], other.field));
    return this.test(op, other);
  }

  eq(v: V | Col<unknown>): Cond { return this.cmp(Op.EQ, v); }
  ne(v: V | Col<unknown>): Cond { return this.cmp(Op.NE, v); }
  lt(v: V | Col<unknown>): Cond { return this.cmp(Op.LT, v); }
  le(v: V | Col<unknown>): Cond { return this.cmp(Op.LE, v); }
  gt(v: V | Col<unknown>): Cond { return this.cmp(Op.GT, v); }
  ge(v: V | Col<unknown>): Cond { return this.cmp(Op.GE, v); }
  between(low: V, high: V): Cond { return this.ge(low).and(this.le(high)); }
  in(values: readonly V[]): Cond { return this.test(Op.IN, ...values); }
  in_(values: readonly V[]): Cond { return this.in(values); }
  isNull(): Cond { return new Cond(() => new Expr(Op.IS_NULL, this.field)); }
  exists(): Cond { return new Cond(() => new Expr(Op.EXISTS, this.field)); }
  contains(s: string | Uint8Array): Cond { return this.test(Op.CONTAINS, s); }
  startsWith(s: string | Uint8Array): Cond { return this.test(Op.STARTS_WITH, s); }
  endsWith(s: string | Uint8Array): Cond { return this.test(Op.ENDS_WITH, s); }
  like(s: string | Uint8Array): Cond { return this.test(Op.LIKE, s); }
  regex(s: string | Uint8Array): Cond { return this.test(Op.REGEX, s); }
  icontains(s: string | Uint8Array): Cond { return this.test(Op.ICONTAINS, s); }
  ilike(s: string | Uint8Array): Cond { return this.test(Op.ILIKE, s); }
  /** 整字段 UTF-8 编辑距离，0..8。 */
  fuzzy(s: string | Uint8Array, distance: number): Cond {
    return new Cond(() => new Expr(Op.FUZZY, this.field, [s, Math.trunc(distance)]));
  }
  asc(nullsFirst = true): Sort { return new Sort(() => new Order(this.field, false, nullsFirst)); }
  desc(nullsFirst = true): Sort { return new Sort(() => new Order(this.field, true, nullsFirst)); }
  /** @internal */
  column(): Column { return new Column(this.field); }
}

function outputIndex(columns: readonly Column[], c: Column): number {
  for (let i = 0; i < columns.length; i++) {
    const x = columns[i];
    if (x.field.equals(c.field) && x.aggregate === c.aggregate && x.distinct === c.distinct) return i;
  }
  throw new Error("aggregate used in having/orderBy must also be selected");
}

/** 聚合输出列；比较只能用于 having()，orderBy 里引用的聚合也必须已 select。 */
export class Agg {
  /** @internal */
  constructor(readonly col: Column) {}
  private cmp(op: Op, v: QueryValue): Cond {
    return new Cond((c) => new Expr(op, Field.column(outputIndex(c, this.col)), [v]), true);
  }
  eq(v: IntLike | number): Cond { return this.cmp(Op.EQ, v); }
  ne(v: IntLike | number): Cond { return this.cmp(Op.NE, v); }
  lt(v: IntLike | number): Cond { return this.cmp(Op.LT, v); }
  le(v: IntLike | number): Cond { return this.cmp(Op.LE, v); }
  gt(v: IntLike | number): Cond { return this.cmp(Op.GT, v); }
  ge(v: IntLike | number): Cond { return this.cmp(Op.GE, v); }
  asc(nullsFirst = true): Sort {
    return new Sort((c) => new Order(Field.column(outputIndex(c, this.col)), false, nullsFirst));
  }
  desc(nullsFirst = true): Sort {
    return new Sort((c) => new Order(Field.column(outputIndex(c, this.col)), true, nullsFirst));
  }
  /** @internal */
  column(): Column { return this.col; }
}

// 字段工厂：名字即 protobuf 线类型。
export const u64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.UINT64, source), intCast);
export const i64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.INT64, source), intCast);
export const f64 = (n: number, source = 0): Col<number> => new Col(new Field(n, FieldType.DOUBLE, source), doubleCast);
export const text = (n: number, source = 0): Col<string | Uint8Array> => new Col(new Field(n, FieldType.BYTES, source));
export const s64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.SINT64, source), intCast);
export const f32 = (n: number, source = 0): Col<number> => new Col(new Field(n, FieldType.FLOAT, source), doubleCast);
export const fixed64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.FIXED64, source), intCast);
export const sfixed64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.SFIXED64, source), intCast);
export const fixed32 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.FIXED32, source), intCast);
export const sfixed32 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.SFIXED32, source), intCast);
export const boolean = (n: number, source = 0): Col<boolean> => new Col(new Field(n, FieldType.BOOL, source));
/** 记录键。 */
export const key = (source = 0): Col<string | Uint8Array> => new Col(Field.key(source));
/** 嵌套消息里的路径（字段号序列）与叶子类型（FieldType）。 */
export const nested = (path: readonly number[], type: number, source = 0): Col<QueryValue> =>
  new Col(new Field(0, type, source, path));

export const count = (): Agg => new Agg(new Column(Field.key(), Aggregate.COUNT_ALL));
export const countOf = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.COUNT));
export const countDistinct = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.COUNT, "", true));
export const sum = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.SUM));
export const avg = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.AVG));
export const min = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.MIN));
export const max = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.MAX));

type Selectation = Col<any> | Agg;

/** 带列信息的结果：用 select() 时的 Col/Agg 对象取值。 */
export class Rows implements Iterable<TypedRow> {
  constructor(private readonly columns: readonly Column[], readonly result: QueryResult) {}
  get length(): number { return this.result.length; }
  get cursor(): Uint8Array { return this.result.cursor; }
  get stats(): QueryStats { return this.result.stats; }
  get access(): AccessPlan[] { return this.result.access; }
  at(i: number): TypedRow { return new TypedRow(this.columns, this.result.row(i)); }
  *[Symbol.iterator](): Iterator<TypedRow> {
    for (let i = 0; i < this.length; i++) yield this.at(i);
  }
  close(): void { this.result.close(); }
}

export class TypedRow {
  constructor(private readonly columns: readonly Column[], readonly raw: ResultRow) {}
  /** 行键（原生缓冲上的视图）。 */
  get key(): Uint8Array { return this.raw.key; }
  get(item: Selectation, copy = false): ResultValue {
    const target = item.column();
    for (let i = 0; i < this.columns.length; i++) {
      const c = this.columns[i];
      if (c.field.equals(target.field) && c.aggregate === target.aggregate && c.distinct === target.distinct)
        return this.raw.get(i, copy);
    }
    throw new Error("column was not selected");
  }
}

export class QueryBuilder {
  private readonly prefix: Uint8Array | string;
  private whereCond: Cond | null = null;
  private havingCond: Cond | null = null;
  private columns: Column[] = [];
  private sorts: Sort[] = [];
  private groups: Field[] = [];
  private joins: Join[] = [];
  private opts: Record<string, unknown> = {};

  constructor(prefix: Uint8Array | string) {
    this.prefix = prefix;
  }

  where(c: Cond): this {
    if (c.aggregate) throw new Error("aggregates belong in having()");
    this.whereCond = c;
    return this;
  }
  select(...items: Selectation[]): this { this.columns = items.map((i) => i.column()); return this; }
  groupBy(...cols: Col<any>[]): this { this.groups = cols.map((c) => c.field); return this; }
  having(c: Cond): this { this.havingCond = c; return this; }
  orderBy(...items: Array<Sort | Col<any> | Agg>): this {
    this.sorts = items.map((i) => (i instanceof Sort ? i : i.asc()));
    return this;
  }
  /** 连接另一个前缀：left 属于已有源，right 属于新源（source 编号为连接顺序）。 */
  join(prefix: Uint8Array | string, left: Col<any>, right: Col<any>, kind: JoinKind = JoinKind.INNER, on?: Cond): this {
    this.joins.push(new Join(prefix, [left.field], [right.field], kind, on ? on.build([]) : new Expr()));
    return this;
  }
  limit(n: IntLike): this { this.opts.limit = n; return this; }
  offset(n: IntLike): this { this.opts.offset = n; return this; }
  after(cursor: Uint8Array): this { this.opts.after = cursor; return this; }
  distinct(on = true): this { this.opts.distinct = on; return this; }
  countTotal(on = true): this { this.opts.countTotal = on; return this; }
  allowScan(on: boolean): this { this.opts.allowScan = on; return this; }
  index(name: string | Uint8Array): this { this.opts.indexHint = name; return this; }
  memory(n: IntLike): this { this.opts.memoryBytes = n; return this; }
  maxCandidates(n: IntLike): this { this.opts.maxCandidates = n; return this; }
  maxWork(n: IntLike): this { this.opts.maxWork = n; return this; }
  timeoutMs(n: IntLike): this { this.opts.timeoutMs = n; return this; }

  /** 等价的底层 Query。 */
  build(): Query {
    const cols = this.columns;
    const q = new Query(this.prefix, {
      columns: [...cols], group: [...this.groups], joins: [...this.joins],
      order: this.sorts.map((s) => s.build(cols)), ...this.opts,
    });
    if (this.whereCond) q.where = this.whereCond.build(cols);
    if (this.havingCond) q.having = this.havingCond.build(cols);
    return q;
  }
  prepare(): PreparedBuilder { return new PreparedBuilder(this.build().prepare(), [...this.columns]); }
  run(db: Selectable): Rows { return new Rows(this.columns, this.build().execute(db)); }
  explain(db: Selectable): QueryResult { return this.build().explain(db); }
  /** 逐页执行（游标为空时结束）。 */
  *pages(db: Selectable): Generator<Rows, void, undefined> {
    const cols = this.columns;
    for (const page of this.build().pages(db)) yield new Rows(cols, page);
  }
}

/** 编码一次、可重复执行。 */
export class PreparedBuilder {
  constructor(readonly prepared: PreparedQuery, private readonly columns: readonly Column[]) {}
  run(db: Selectable): Rows { return new Rows(this.columns, this.prepared.execute(db)); }
}

/** 开始一个查询：`from("p/")`。 */
export function from(prefix: Uint8Array | string): QueryBuilder { return new QueryBuilder(prefix); }
export { from as from_ };
