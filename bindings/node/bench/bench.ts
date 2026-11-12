/**
 * 与 tools/lang_conformance/bench_c.c 同数据、同操作的基准。
 * 用法: node dist/bench/bench.js DIR [SECONDS]
 * 每行一个 JSON: {"op":..., "ops":..., "ns_per_op":..., "sink":...}
 * 与 bench_c 同名的 op 使用“与 C 最接近”的接口形态；带 ":" 后缀的行是同一操作的其它接口形态，
 * 用来量化 JS↔原生边界各种做法的代价（README 有对照表）。
 * select_topn 的请求取自 FQDB_BENCH_SELECT_HEX，未设置时取 FQDB_CONFORMANCE 中 topn_mage_level 的 request。
 */
import * as fs from "node:fs";
import { setImmediate as tick } from "node:timers/promises";
import { QueryResult, Store, hexToBytes, releaseShared } from "../src";

const N = 32768;
const seconds = process.argv[3] ? parseFloat(process.argv[3]) : 1.0;
const dir = process.argv[2] ?? "/tmp/fqdb-bench-node";

const now = (): number => Number(process.hrtime.bigint()) * 1e-9;
function report(op: string, ops: number, secs: number, sink: number): void {
  console.log(JSON.stringify({ op, ops, ns_per_op: Math.round((secs * 1e9 * 10) / ops) / 10, sink }));
}

/** 键 'k' + 8 字节大端序号（高 4 字节恒为 0）。 */
function makeKey(k: Uint8Array, id: number): void {
  k[0] = 107;
  k[5] = (id >>> 24) & 255;
  k[6] = (id >>> 16) & 255;
  k[7] = (id >>> 8) & 255;
  k[8] = id & 255;
}
/** (id * 2654435761) % N，避开 2^53 精度问题（N 为 2 的幂时与 C 的 64 位乘法同值）。 */
const KMUL = 2654435761 % N;
const scatter = (id: number): number => ((id % N) * KMUL) % N;

/** 以 1000 次为一批计时，直到超过 seconds。fn(i) 返回要累加的 sink 值。 */
async function run(op: string, batch: number, perBatch: number, fn: () => number, yieldEach = false): Promise<void> {
  let ops = 0;
  let sink = 0;
  const t0 = now();
  let t: number;
  do {
    for (let i = 0; i < batch; i++) sink += fn();
    ops += perBatch;
    if (yieldEach) await tick(); // 让 GC finalizer 有机会归还租约
    t = now();
  } while (t - t0 < seconds);
  report(op, ops, t - t0, sink);
}

function putDoc(store: Store, i: number): void {
  const cls = i % 2 ? "mage" : "warrior";
  const body: number[] = [0x08, ...varint(i), 0x12, cls.length, ...Buffer.from(cls), 0x18, ...varint(i % 20)];
  store.put(`p/${i}`, Uint8Array.from(body));
}
function varint(v: number): number[] {
  const out: number[] = [];
  while (v >= 128) {
    out.push((v & 127) | 128);
    v = Math.floor(v / 128);
  }
  out.push(v);
  return out;
}

async function main(): Promise<void> {
  const store = Store.open(dir, {
    durability: "buffered", cacheBytes: 2n << 30n, deltaBytes: 16 << 20, writerBytes: 32 << 20,
    transactionBytes: 64 << 20, allTransactionBytes: 256 << 20, pinnedBytes: 2n << 30n, maxSnapshots: 1024,
  });
  const value = new Uint8Array(256);
  const key = new Uint8Array(9);
  for (let i = 0; i < N; i++) {
    value.fill(i & 255);
    makeKey(key, i);
    store.put(key, value);
  }
  store.sync();

  // 1. 准备键 + 共享读取（热点读）
  {
    makeKey(key, 4242);
    const pk = store.prepareKey(key);
    // 与 C 最接近：每次得到一个视图并显式归还租约
    await run("get_shared_prepared", 1000, 1000, () => {
      const v = pk.getSharedBytes()!;
      const s = v[v.length - 1];
      releaseShared(v);
      return s;
    });
    await run("get_shared_prepared:view_gc", 1000, 1000, () => {
      const v = pk.getSharedBytes()!;
      return v[v.length - 1];
    }, true);
    await run("get_shared_prepared:wrapper", 1000, 1000, () => {
      const v = pk.getShared()!;
      const s = v.bytes[v.length - 1];
      v.release();
      return s;
    });
    await run("get_shared_prepared:callback", 1000, 1000, () => pk.withShared((b) => b![b!.length - 1]));
    await run("get_shared_prepared:peek", 1000, 1000, () => pk.peekByte(-1));
    const scratch = new Uint8Array(256);
    await run("get_shared_prepared:copy_into", 1000, 1000, () => {
      pk.getInto(scratch);
      return scratch[255];
    });
    await run("get_shared_prepared:copy_alloc", 1000, 1000, () => {
      const b = pk.get()!;
      return b[b.length - 1];
    });
    pk.close();
  }

  // 2. 按键共享读取（每次传键）
  {
    let id = 0;
    await run("get_shared_key", 1000, 1000, () => {
      makeKey(key, scatter(id++));
      const v = store.getSharedBytes(key)!;
      const s = v[0];
      releaseShared(v);
      return s;
    });
    id = 0;
    await run("get_shared_key:callback", 1000, 1000, () => {
      makeKey(key, scatter(id++));
      return store.withShared(key, (b) => b![0]);
    });
  }

  // 3. 复制读取
  {
    let id = 0;
    await run("get_copy", 1000, 1000, () => {
      makeKey(key, scatter(id++));
      return store.get(key)![0];
    });
  }

  // 4. 32 键准备读取器，每个操作 = 1 条记录
  {
    const keys: Uint8Array[] = [];
    for (let i = 0; i < 32; i++) {
      const k = new Uint8Array(9);
      makeKey(k, i * 997);
      keys.push(k);
    }
    const rd = store.prepareReader(keys);
    await run("reader32_per_record", 200, 200 * 32, () => {
      rd.read();
      let s = 0;
      for (let j = 0; j < 32; j++) s += rd.value(j)![0];
      return s;
    });
    const scratch = new Uint8Array(256);
    await run("reader32_per_record:copy_into", 200, 200 * 32, () => {
      rd.read();
      let s = 0;
      for (let j = 0; j < 32; j++) {
        rd.copyValue(j, scratch);
        s += scratch[0];
      }
      return s;
    });
    await run("reader32_per_record:descriptor_only", 200, 200 * 32, () => {
      rd.read();
      let s = 0;
      for (let j = 0; j < 32; j++) s += rd.length(j);
      return s;
    });
    rd.close();
  }

  // 5. 查询
  {
    let hex = process.env.FQDB_BENCH_SELECT_HEX;
    if (!hex && process.env.FQDB_CONFORMANCE) {
      const conf = JSON.parse(fs.readFileSync(process.env.FQDB_CONFORMANCE, "utf8"));
      hex = conf.cases.find((c: { id: string }) => c.id === "topn_mage_level").request as string;
    }
    if (hex) {
      for (let i = 0; i < 64; i++) putDoc(store, i);
      const req = hexToBytes(hex);
      // 与 C 最接近：执行并只读响应字节数，然后归还缓冲
      await run("select_topn", 200, 200, () => {
        const v = store.selectRaw(req);
        const s = v.length;
        releaseShared(v);
        return s;
      });
      await run("select_topn:result_object", 200, 200, () => {
        const r = new QueryResult(store.selectRaw(req));
        const s = r.byteLength;
        r.close();
        return s;
      });
      await run("select_topn:decode_all_rows", 200, 200, () => {
        const r = new QueryResult(store.selectRaw(req));
        let s = 0;
        for (let i = 0; i < r.length; i++) {
          const row = r.row(i);
          s += Number(row.get(0)) + Number(row.get(1));
        }
        r.close();
        return s;
      });
    }
  }
  store.close();
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
