import test from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";
import { Worker } from "node:worker_threads";
import { Store } from "../src";
import { rmDir, tmpDir } from "./helpers";

test("worker_threads: each worker attaches to the shared Store and uses its own reader", async () => {
  const dir = tmpDir();
  const store = Store.open(dir, { durability: "buffered" });
  const keys = Array.from({ length: 16 }, (_, i) => "key" + i);
  for (const k of keys) store.put(k, "value-" + k);
  const workers = 4;
  const jobs: Promise<{ id: number; checked: number; bad: number }>[] = [];
  for (let id = 0; id < workers; id++) {
    const token = store.share();
    jobs.push(
      new Promise((resolve, reject) => {
        const w = new Worker(path.join(__dirname, "worker_reader.js"), {
          workerData: { token, id, rounds: 2000, keys },
        });
        w.once("message", resolve);
        w.once("error", reject);
        w.once("exit", (code) => code !== 0 && reject(new Error("worker exit " + code)));
      }),
    );
  }
  // 主线程同时写入其他键并读取，验证写与多线程读互不干扰
  for (let i = 0; i < 200; i++) store.put("other" + i, "x");
  const results = await Promise.all(jobs);
  for (const r of results) {
    assert.equal(r.bad, 0, `worker ${r.id} saw wrong data`);
    assert.ok(r.checked > 0);
  }
  store.sync(); // buffered 模式需要手动 sync 才能在重新打开后看到数据
  store.close();
  // 所有 worker 句柄都已关闭：目录锁被释放，可以重新打开
  const again = Store.open(dir, { durability: "buffered" });
  assert.equal(new TextDecoder().decode(again.get("key3")!), "value-key3");
  again.close();
  rmDir(dir);
});

test("an unused share token can be discarded", () => {
  const dir = tmpDir();
  const store = Store.open(dir, { durability: "buffered" });
  const token = store.share();
  Store.discard(token);
  store.close();
  const again = Store.open(dir, { durability: "buffered" });
  again.close();
  rmDir(dir);
});
