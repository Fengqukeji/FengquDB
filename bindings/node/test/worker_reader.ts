// worker_threads 中的读取者：附着到主线程共享的 Store，使用自己的读取器、准备键和共享读。
import { parentPort, workerData } from "node:worker_threads";
import { Store, StoreToken } from "../src";

const { token, id, rounds, keys } = workerData as { token: StoreToken; id: number; rounds: number; keys: string[] };
const store = Store.attach(token);
let checked = 0;
let bad = 0;
const dec = new TextDecoder();
const reader = store.prepareReader(keys);
const pk = store.prepareKey(keys[0]);
for (let r = 0; r < rounds; r++) {
  reader.read();
  for (let i = 0; i < keys.length; i++) {
    const v = reader.value(i);
    if (v === null || dec.decode(v) !== "value-" + keys[i]) bad++;
    checked++;
  }
  const s = pk.getShared();
  if (s === null || dec.decode(s.bytes) !== "value-" + keys[0]) bad++;
  s?.release();
  checked++;
  const own = store.get(keys[id % keys.length]);
  if (own === null || dec.decode(own) !== "value-" + keys[id % keys.length]) bad++;
  checked++;
}
reader.close();
pk.close();
store.close();
parentPort!.postMessage({ id, checked, bad });
