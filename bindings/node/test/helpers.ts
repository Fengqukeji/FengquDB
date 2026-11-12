import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { Aggregate, Column, Double, Expr, Field, Join, Op, Order, Query, QueryValue, hexToBytes } from "../src";

export function tmpDir(prefix = "fqdb-node-"): string {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}
export function rmDir(dir: string): void {
  fs.rmSync(dir, { recursive: true, force: true });
}

export interface Conformance {
  version: number;
  dataset: {
    records: Array<{ key: string; value: string }>;
    indexes: Array<{ name: string; prefix: string; fields: Array<[number, number]>; flags: number }>;
  };
  cases: Array<{
    id: string;
    spec: any;
    encoded: string;
    request: string;
    paginate?: { values: number[]; pages: number };
    result: any;
  }>;
}

export function loadConformance(): Conformance {
  const file = process.env.FQDB_CONFORMANCE;
  if (!file) throw new Error("set FQDB_CONFORMANCE to the path of conformance.json");
  return JSON.parse(fs.readFileSync(file, "utf8")) as Conformance;
}

const field = (f: any): Field => new Field(f.number, f.type, f.source, f.path, f.repeated);
const value = (v: any): QueryValue => {
  if ("int" in v) return BigInt(v.int);
  if ("bytes" in v) return hexToBytes(v.bytes);
  if ("f64" in v) return new Double(v.f64);
  if ("bool" in v) return !!v.bool;
  if ("null" in v) return null;
  throw new Error("unknown spec value " + JSON.stringify(v));
};
const expr = (e: any): Expr =>
  new Expr(
    e.op as Op,
    e.field ? field(e.field) : new Field(),
    (e.values ?? []).map(value),
    (e.children ?? []).map(expr),
    e.right ? field(e.right) : null,
  );

/** 语言无关的 spec -> 本绑定的底层 Query 模型。 */
export function queryFromSpec(s: any): Query {
  return new Query(hexToBytes(s.prefix), {
    where: expr(s.where),
    columns: s.columns.map((c: any) => new Column(field(c.field), c.aggregate as Aggregate, c.name, c.distinct)),
    order: s.order.map((o: any) => new Order(field(o.field), o.descending, o.nulls_first)),
    group: s.group.map(field),
    having: expr(s.having),
    joins: s.joins.map(
      (j: any) => new Join(hexToBytes(j.prefix), j.left.map(field), j.right.map(field), j.kind, expr(j.on)),
    ),
    limit: s.limit,
    offset: s.offset,
    memoryBytes: s.memory_bytes,
    maxCandidates: s.max_candidates,
    maxWork: s.max_work,
    timeoutMs: s.timeout_ms,
    allowScan: s.allow_scan,
    countTotal: s.count_total,
    distinct: s.distinct,
    indexHint: new TextEncoder().encode(s.index_hint),
    after: hexToBytes(s.after),
  });
}
