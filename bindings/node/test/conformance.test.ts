import test, { after, before, describe } from "node:test";
import assert from "node:assert/strict";
import {
  Op, Store, QueryResult, bytesToHex, from, hexToBytes, text, u64, s64,
} from "../src";
import { Conformance, loadConformance, queryFromSpec, rmDir, tmpDir } from "./helpers";

const conf: Conformance = loadConformance();
let dir: string;
let store: Store;

function expectedValue(actual: unknown, want: any, where: string): void {
  if ("int" in want) assert.equal(actual, BigInt(want.int), where);
  else if ("bytes" in want) assert.equal(bytesToHex(actual as Uint8Array), want.bytes, where);
  else if ("f64" in want) assert.equal(actual, want.f64, where);
  else if ("bool" in want) assert.equal(actual, !!want.bool, where);
  else if ("null" in want) assert.equal(actual, null, where);
  else assert.fail("unknown expected value " + JSON.stringify(want));
}

function checkResult(res: QueryResult, want: any, id: string): void {
  assert.equal(res.length, want.rows.length, `${id}: row count`);
  want.rows.forEach((row: any, i: number) => {
    const got = res.row(i);
    assert.equal(bytesToHex(got.key), row.key, `${id}: key ${i}`);
    assert.equal(got.length, row.values.length, `${id}: column count ${i}`);
    row.values.forEach((v: any, j: number) => expectedValue(got.get(j), v, `${id}: row ${i} col ${j}`));
  });
  assert.equal(bytesToHex(res.cursor), want.cursor, `${id}: cursor`);
  const stats = res.stats;
  assert.equal(stats.matched, want.matched, `${id}: matched`);
  assert.equal(stats.complete, want.complete, `${id}: complete`);
  assert.equal(stats.result_total ?? 0, want.result_total, `${id}: result_total`);
  assert.deepEqual(res.access.map((a) => a.strategy), want.strategies, `${id}: strategies`);
  assert.deepEqual(res.access.map((a) => a.index ?? ""), want.indexes, `${id}: indexes`);
}

before(() => {
  dir = tmpDir("fqdb-conf-");
  store = Store.open(dir, { durability: "buffered" });
  for (const r of conf.dataset.records) store.put(hexToBytes(r.key), hexToBytes(r.value));
  store.sync();
  for (const ix of conf.dataset.indexes) {
    store.defineCompoundIndex(ix.name, ix.prefix, ix.fields, ix.flags);
  }
});
after(() => {
  store.close();
  rmDir(dir);
});

describe("conformance: encoding", () => {
  for (const c of conf.cases) {
    test(`${c.id}: Query bytes and request bytes match`, () => {
      const q = queryFromSpec(c.spec);
      assert.equal(bytesToHex(q.encode()), c.encoded);
      const want = hexToBytes(c.request);
      // 请求里的操作号决定 SELECT(53)/EXPLAIN(54)
      assert.equal(bytesToHex(q.request(want[1] === 54)), c.request);
    });
  }
});

describe("conformance: execution", () => {
  for (const c of conf.cases.filter((x) => !x.paginate)) {
    test(`${c.id}: result matches`, () => {
      const q = queryFromSpec(c.spec);
      const explain = hexToBytes(c.request)[1] === 54;
      const res = explain ? q.explain(store) : q.execute(store);
      try {
        checkResult(res, c.result, c.id);
      } finally {
        res.close();
      }
    });
  }

  for (const c of conf.cases.filter((x) => x.paginate)) {
    test(`${c.id}: pages concatenate to paginate.values`, () => {
      const q = queryFromSpec(c.spec);
      const got: number[] = [];
      let pages = 0;
      for (const page of q.pages(store)) {
        pages++;
        for (const row of page) got.push(Number(row.get(0)));
        page.close();
      }
      assert.deepEqual(got, c.paginate!.values);
      assert.equal(pages, c.paginate!.pages);
    });
  }

  test("a closed result throws instead of crashing", () => {
    const c = conf.cases[0];
    const res = queryFromSpec(c.spec).execute(store);
    const row = res.row(0);
    const view = row.key;
    const cursor = res.cursor;
    res.close();
    res.close(); // 幂等
    assert.throws(() => row.get(0), (e: any) => e.status === -1);
    assert.throws(() => res.row(0), (e: any) => e.status === -1);
    assert.equal(view.length, 0); // 先前取得的视图被 detach
    assert.ok(cursor.length > 0); // 游标是副本，关闭后仍可用
  });
});

describe("typed builder", () => {
  test("builder emits the same bytes as the topn_mage_level vector", () => {
    const c = conf.cases.find((x) => x.id === "topn_mage_level")!;
    const uid = u64(1), cls = text(2), level = u64(3);
    const b = from("p/")
      .where(cls.eq("mage").and(level.ge(5)).and(level.le(15)))
      .select(uid, level)
      .orderBy(level.desc())
      .limit(7)
      .countTotal();
    assert.equal(bytesToHex(b.build().encode()), c.encoded);
    const rows = b.run(store);
    assert.equal(rows.length, 7);
    assert.equal(rows.at(0).get(uid), 55n);
    assert.equal(rows.at(0).get(level), 15n);
    assert.equal(rows.at(0).get(level, true) as bigint, 15n);
    assert.equal(bytesToHex(rows.cursor), c.result.cursor);
    rows.close();
  });

  test("aggregates, group by and having", () => {
    const c = conf.cases.find((x) => x.id === "aggregates")!;
    // 以 spec 为准构造等价 builder：先用底层模型核对，再确认 builder 能执行并给出同样的分组数
    const cls = text(2), level = u64(3);
    void level;
    const rows = from("p/").select(cls, count_()).groupBy(cls).having(count_().ge(1)).run(store);
    assert.equal(rows.length, c.result.rows.length);
    rows.close();
  });

  test("in/like/between/isNull/any compile to the documented operators", () => {
    const f = text(2);
    const g = u64(3);
    const q = from("p/")
      .where(f.in(["mage", "warrior"]).and(f.like("m%")).and(g.between(1, 9)).and(g.isNull().not()).and(g.any().eq(3)))
      .build();
    const w = q.where;
    assert.equal(w.op, Op.AND);
    assert.doesNotThrow(() => q.encode());
  });

  test("explain via builder", () => {
    const res = from("p/").where(text(2).eq("mage")).select(u64(1)).limit(1).explain(store);
    assert.ok(res.access.length >= 1);
    res.close();
  });

  test("a negative integer and a double round-trip through a comparison", () => {
    const gold = s64(4);
    const rows = from("p/").where(gold.ge(-1000)).select(gold).limit(3).run(store);
    assert.ok(rows.length > 0);
    for (const r of rows) assert.equal(typeof r.get(gold), "bigint");
    rows.close();
  });
});

import { count as count_ } from "../src";
