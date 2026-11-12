import test, { after, afterEach, beforeEach, describe } from "node:test";
import assert from "node:assert/strict";
import * as v8 from "node:v8";
import * as vm from "node:vm";
import { setImmediate as tick } from "node:timers/promises";
import {
  FengquDBError, Status, Store, from, text, u64, releaseShared, INDEX_REQUIRED,
} from "../src";
import { rmDir, tmpDir } from "./helpers";

const enc = (s: string) => new TextEncoder().encode(s);
const dec = (b: Uint8Array) => new TextDecoder().decode(b);
const isClosed = (e: unknown) => e instanceof FengquDBError && e.status === Status.CLOSED;

let dir: string;
let store: Store;
beforeEach(() => {
  dir = tmpDir();
  store = Store.open(dir, { durability: "buffered" });
});
afterEach(() => {
  store.close();
  rmDir(dir);
});

describe("read/write", () => {
  test("put/get/delete, miss, empty value, overwrite", () => {
    assert.equal(store.get("nope"), null);
    const v1 = store.put("a", "hello");
    assert.equal(typeof v1, "bigint");
    assert.equal(dec(store.get("a")!), "hello");
    const v2 = store.put(enc("a"), enc("world"));
    assert.ok(v2 > v1);
    assert.equal(dec(store.get("a")!), "world");
    store.put("empty", new Uint8Array(0));
    const e = store.get("empty");
    assert.ok(e !== null && e.length === 0);
    assert.equal(store.delete("a"), true);
    assert.equal(store.get("a"), null);
    assert.equal(store.delete("a"), false); // 重复删除不报错
  });

  test("binary keys and values, ArrayBuffer input, long string key", () => {
    const k = new Uint8Array([0, 1, 255, 0, 7]);
    const v = new Uint8Array(1000).map((_, i) => i & 255);
    store.put(k, v);
    assert.deepEqual(new Uint8Array(store.get(k)!), v);
    store.put(k.buffer.slice(0) as ArrayBuffer, "x");
    assert.equal(dec(store.get(k)!), "x");
    const long = "k".repeat(1000);
    store.put(long, "long");
    assert.equal(dec(store.get(long)!), "long");
  });

  test("get returns an independent copy", () => {
    store.put("a", "one");
    const copy = store.get("a")!;
    store.put("a", "two");
    assert.equal(dec(copy), "one");
  });

  test("expired keys read as missing", async () => {
    store.put("t", "x", { expiresAtMs: Date.now() + 150 });
    assert.equal(dec(store.get("t")!), "x");
    await new Promise((r) => setTimeout(r, 400));
    assert.equal(store.get("t"), null);
    assert.equal(store.getShared("t"), null);
  });

  test("bad arguments throw TypeError/RangeError, not crash", () => {
    assert.throws(() => store.put(123 as any, "v"), TypeError);
    assert.throws(() => store.put("k", new Float64Array(2) as any), TypeError);
    assert.throws(() => store.put("k", "v", { expiresAtMs: -1 }), RangeError);
  });

  test("native errors carry the numeric status", () => {
    const missing = tmpDir() + "/does-not-exist";
    try {
      Store.open(missing, { create: false });
      assert.fail("open should fail");
    } catch (e) {
      assert.ok(e instanceof FengquDBError);
      assert.ok((e as FengquDBError).status > 0);
      assert.notEqual((e as FengquDBError).statusName, "UNKNOWN");
    }
  });

  test("info()", () => {
    store.put("a", "b");
    const info = store.info();
    assert.equal(typeof info.version, "bigint");
    assert.ok(info.storedKeys >= 1n);
  });
});

describe("lifecycle", () => {
  test("use after close throws FengquDBError(CLOSED)", () => {
    const s = Store.open(tmpDir(), { durability: "buffered" });
    const key = s.prepareKey("k");
    s.close();
    s.close(); // 幂等
    assert.ok(s.closed);
    assert.throws(() => s.get("k"), isClosed);
    assert.throws(() => s.put("k", "v"), isClosed);
    assert.throws(() => s.getShared("k"), isClosed);
    assert.throws(() => s.transaction(), isClosed);
    // 按核心库约定，准备键在父 Store 关闭后仍可使用
    assert.equal(key.get(), null);
    key.close();
    assert.throws(() => key.get(), isClosed);
  });

  test("shared views survive writes, deletes and Store close", () => {
    const s = Store.open(tmpDir(), { durability: "buffered" });
    s.put("k", "payload-1");
    const view = s.getShared("k")!;
    assert.equal(dec(view.bytes), "payload-1");
    s.put("k", "payload-2");
    s.delete("k");
    assert.equal(dec(view.bytes), "payload-1");
    s.close();
    assert.equal(dec(view.bytes), "payload-1");
    assert.equal(typeof view.version, "bigint");
    view.release();
  });

  test("released view throws and detaches earlier references", () => {
    store.put("k", "abcdef");
    const view = store.getShared("k")!;
    const raw = view.bytes;
    const sub = raw.subarray(1, 3);
    assert.equal(view.length, 6);
    view.release();
    view.release(); // 幂等
    assert.ok(view.released);
    assert.throws(() => view.bytes, isClosed);
    assert.throws(() => view.length, isClosed);
    assert.equal(raw.length, 0);
    assert.equal(sub.length, 0);
    assert.equal(typeof view.version, "bigint"); // 元数据仍可读
  });

  test("raw view helpers: getSharedBytes + releaseShared", () => {
    store.put("k", "xyz");
    const b = store.getSharedBytes("k")!;
    assert.equal(dec(b), "xyz");
    assert.equal(releaseShared(b), true);
    assert.equal(releaseShared(b), false);
    assert.equal(b.length, 0);
    assert.throws(() => releaseShared(new Uint8Array(4)), TypeError);
    assert.equal(store.getSharedBytes("none"), null);
  });

  test("empty shared value", () => {
    store.put("e", "");
    const v = store.getShared("e")!;
    assert.equal(v.length, 0);
    v.release();
    store.withShared("e", (b) => assert.equal(b!.length, 0));
  });

  test("withShared: view valid inside, detached after, exceptions propagate", () => {
    store.put("k", "callback");
    let kept: Uint8Array | null = null;
    const r = store.withShared("k", (b) => {
      kept = b;
      return dec(b!);
    });
    assert.equal(r, "callback");
    assert.equal(kept!.length, 0);
    assert.equal(store.withShared("miss", (b) => b), null);
    assert.throws(() => store.withShared("k", () => { throw new Error("boom"); }), /boom/);
    // 异常路径也归还了租约：能继续正常读
    assert.equal(store.withShared("k", (b) => b!.length), 8);
  });

  test("lease return: pinned budget is given back by release() and by GC", async () => {
    const dir2 = tmpDir();
    const s = Store.open(dir2, { durability: "buffered", pinnedBytes: 4 << 20 });
    s.put("big", new Uint8Array(256 * 1024));
    const held: Uint8Array[] = [];
    let limit: unknown;
    try {
      for (let i = 0; i < 100; i++) held.push(s.getSharedBytes("big")!);
    } catch (e) {
      limit = e;
    }
    assert.ok(limit instanceof FengquDBError && limit.status === Status.LIMIT, "budget should be exhausted");
    for (const h of held.splice(0, 4)) releaseShared(h); // 显式释放腾出预算
    assert.ok(s.getSharedBytes("big"));
    // 其余租约交给 GC：丢弃引用后 finalizer 归还
    held.length = 0;
    v8.setFlagsFromString("--expose-gc");
    const gc = vm.runInNewContext("gc") as () => void;
    let ok = false;
    for (let i = 0; i < 20 && !ok; i++) {
      gc();
      await tick();
      try {
        for (let j = 0; j < 8; j++) releaseShared(s.getSharedBytes("big")!);
        ok = true;
      } catch {
        /* 继续等 finalizer */
      }
    }
    assert.ok(ok, "GC finalizers should return leases");
    s.close();
    rmDir(dir2);
  });

  test("store close with outstanding lease: lease valid, directory reusable after release", () => {
    const d = tmpDir();
    const s = Store.open(d, { durability: "buffered" });
    s.put("k", "v");
    s.sync();
    const lease = s.getShared("k")!;
    s.close();
    assert.equal(dec(lease.bytes), "v");
    lease.release();
    const again = Store.open(d, { durability: "buffered" });
    assert.equal(dec(again.get("k")!), "v");
    again.close();
    rmDir(d);
  });
});

describe("prepared key", () => {
  test("shared / callback / copy / getInto / peekByte / miss", () => {
    store.put("k", "abcdef");
    const pk = store.prepareKey("k");
    assert.equal(dec(pk.getShared()!.bytes), "abcdef");
    assert.equal(dec(pk.get()!), "abcdef");
    assert.equal(pk.withShared((b) => b!.length), 6);
    const buf = new Uint8Array(16);
    assert.equal(pk.getInto(buf), 6);
    assert.equal(dec(buf.subarray(0, 6)), "abcdef");
    assert.throws(() => pk.getInto(new Uint8Array(2)), (e: any) => e.status === Status.BUFFER_TOO_SMALL);
    assert.equal(pk.peekByte(0), 97);
    assert.equal(pk.peekByte(-1), 102);
    assert.equal(pk.peekByte(99), -1);
    store.delete("k");
    assert.equal(pk.getShared(), null);
    assert.equal(pk.peekByte(0), -1);
    assert.equal(pk.getInto(buf), -1);
    store.put("k", "again");
    assert.equal(dec(pk.get()!), "again"); // 准备键看到新值
    pk.close();
    assert.ok(pk.closed);
  });
});

describe("prepared reader", () => {
  test("rows, misses, empty values, view invalidation", () => {
    store.put("a", "AAA");
    store.put("c", "");
    store.put("d", "DDDD");
    const r = store.prepareReader(["a", "b", "c", "d", "a"]);
    assert.equal(r.read(), 5);
    assert.deepEqual([0, 1, 2, 3, 4].map((i) => r.found(i)), [true, false, true, true, true]);
    assert.deepEqual([0, 1, 2, 3, 4].map((i) => r.length(i)), [3, 0, 0, 4, 3]);
    const first = r.value(0)!;
    assert.equal(dec(first), "AAA");
    assert.equal(r.value(1), null);
    assert.equal(r.value(2)!.length, 0);
    const tgt = new Uint8Array(8);
    assert.equal(r.copyValue(3, tgt), 4);
    assert.equal(dec(tgt.subarray(0, 4)), "DDDD");
    assert.equal(r.copyValue(1, tgt), -1);
    assert.throws(() => r.copyValue(3, new Uint8Array(1)), RangeError);
    assert.equal(typeof r.version(0), "bigint");
    const seen: string[] = [];
    r.forEach((i, v) => seen.push(v ? dec(v) : "-"));
    assert.deepEqual(seen, ["AAA", "-", "", "DDDD", "AAA"]);
    // 写入后再读：看到新值，旧视图被 detach 而非悬垂
    store.put("a", "NEW");
    assert.equal(r.read(), 5);
    assert.equal(first.length, 0);
    assert.equal(dec(r.value(0)!), "NEW");
    const second = r.value(3)!;
    r.release();
    assert.equal(second.length, 0);
    assert.throws(() => r.length(0), RangeError);
    r.read();
    r.close();
    assert.throws(() => r.read(), isClosed);
    assert.throws(() => store.prepareReader([]), RangeError);
  });

  test("reader survives Store close; views detach on close", () => {
    const d = tmpDir();
    const s = Store.open(d, { durability: "buffered" });
    s.put("a", "AAA");
    const r = s.prepareReader(["a"]);
    r.read();
    const v = r.value(0)!;
    s.close();
    assert.equal(dec(v), "AAA");
    r.read();
    assert.equal(dec(r.value(0)!), "AAA");
    r.close();
    assert.equal(v.length, 0);
    rmDir(d);
  });

  test("a reader dropped without close() detaches its views when collected", async () => {
    store.put("a", "AAA");
    let v: Uint8Array;
    (() => {
      const r = store.prepareReader(["a"]);
      r.read();
      v = r.value(0)!;
    })();
    v = v!;
    v8.setFlagsFromString("--expose-gc");
    const gc = vm.runInNewContext("gc") as () => void;
    for (let i = 0; i < 10 && v.length !== 0; i++) {
      gc();
      await tick();
    }
    assert.equal(v.length, 0);
  });
});

describe("transactions", () => {
  test("read own writes, commit makes them visible, close discards", () => {
    store.put("base", "1");
    const t = store.transaction();
    t.put("x", "10");
    t.delete("base");
    assert.equal(dec(t.get("x")!), "10");
    assert.equal(t.get("base"), null);
    assert.equal(store.get("x"), null); // 未提交不可见
    const version = t.commit();
    assert.equal(typeof version, "bigint");
    assert.ok(t.closed);
    assert.throws(() => t.get("x"), isClosed);
    assert.equal(dec(store.get("x")!), "10");
    assert.equal(store.get("base"), null);

    const t2 = store.transaction();
    t2.put("y", "no");
    t2.close(); // 回滚
    assert.equal(store.get("y"), null);
  });

  test("putMany/deleteMany, savepoints, transactional select sees private writes", () => {
    const t = store.transaction();
    t.putMany([["m1", "a"], ["m2", "b", 0], [enc("m3"), enc("c")]]);
    const sp = t.savepoint();
    t.put("m4", "d");
    t.rollbackTo(sp);
    assert.equal(t.get("m4"), null);
    t.deleteMany(["m1"]);
    t.commit();
    assert.equal(store.get("m1"), null);
    assert.equal(dec(store.get("m2")!), "b");
    assert.equal(dec(store.get("m3")!), "c");

    const t3 = store.transaction();
    t3.put("p/1", new Uint8Array([0x08, 0x01, 0x12, 0x01, 0x78]));
    const res = from("p/").select(u64(1)).run(t3);
    assert.equal(res.length, 1);
    res.close();
    assert.equal(from("p/").select(u64(1)).run(store).length, 0); // Store 的 select 走只读快照
    t3.close();
  });

  test("write conflict surfaces as FengquDBError(CONFLICT)", () => {
    store.put("c", "0");
    const t1 = store.transaction();
    const t2 = store.transaction();
    t1.get("c");
    t2.get("c");
    t1.put("c", "1");
    t2.put("c", "2");
    t1.commit();
    assert.throws(() => t2.commit(), (e: any) => e instanceof FengquDBError && e.status === Status.CONFLICT);
    t2.close();
  });

  test("Store.close() with an open transaction defers the real close", () => {
    const d = tmpDir();
    const s = Store.open(d, { durability: "buffered" });
    const t = s.transaction();
    t.put("k", "v");
    s.close(); // 事务还在：底层 Store 保持存活
    assert.equal(dec(t.get("k")!), "v");
    t.commit();
    rmDir(d);
  });

  test("snapshot is a stable point-in-time view", () => {
    store.put("k", "old");
    const snap = store.snapshot();
    store.put("k", "new");
    assert.equal(dec(snap.get("k")!), "old");
    const sh = snap.getShared("k")!;
    assert.equal(dec(sh.bytes), "old");
    assert.equal(snap.get("none"), null);
    snap.close();
    assert.throws(() => snap.get("k"), isClosed);
    assert.equal(dec(sh.bytes), "old");
  });
});

describe("indexes", () => {
  test("define / query / drop a compound index", () => {
    for (let i = 0; i < 20; i++) {
      // 字段 1 uid(varint)，字段 2 class(bytes)
      store.put(`p/${i}`, new Uint8Array([0x08, i, 0x12, 1, i % 2 ? 0x6d : 0x77]));
    }
    store.sync();
    const info = store.defineCompoundIndex("cls", "p/", [[2, 4], [1, 1]], INDEX_REQUIRED);
    assert.equal(typeof info.finalVersion, "bigint");
    const rows = from("p/").where(text(2).eq("m")).select(u64(1)).run(store);
    assert.equal(rows.length, 10);
    assert.ok(rows.access.some((a) => a.index === "cls"));
    rows.close();
    store.dropIndex("cls");
    assert.throws(() => store.dropIndex("cls"), FengquDBError);
    assert.throws(() => store.defineCompoundIndex("x", "p/", [], 0), RangeError);
  });
});

after(() => {
  /* 保证 node:test 进程能自然退出 */
});
