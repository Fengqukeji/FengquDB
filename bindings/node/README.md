# fengqudb: Node.js / TypeScript binding

N-API addon (only `node_api.h`, no node-addon-api, no node-gyp download) plus a TypeScript layer with the
query model, a typed query builder and `.d.ts` files. The runtime has no npm dependencies.
Everything is synchronous. A `sync` durability write blocks the event loop, so use `worker_threads` for heavy work.

Start with the [current integration guide](../../docs/languages/node.md). A new
database defaults to performance mode (50 ms / 1 MiB): ordinary success confirms
in-memory publication. Use `forceReliable: true` for critical writes. The low-level
`durability` option and the effective write policy are separate settings; `buffered`
means manual synchronization, not concurrent operation.
The test/benchmark commands below target the development repository; those
fixtures and programs are intentionally excluded from the small client SDK.

**Verified on:** Linux (container `fqdb-lang-dev:1`, Node 24, g++ 15, CMake 4.2).
**Windows / MSVC:** the prebuilt SDK was built and verified for module loading, CRUD and reliable sync on 2026-11-14. Windows worker/conformance suites and macOS runtime remain unverified.

## Build and test (in the Docker image)

```sh
# native addon -> build/fengqudb_native.node
docker run --rm --entrypoint sh -v $REPO/build/lang:/lang -v $REPO/bindings/node:/src -w /src fqdb-lang-dev:1 -c \
  "cmake -S . -B build -DFQDB_INCLUDE_DIR=/lang/linux/include -DFQDB_LIB_DIR=/lang/linux/lib -DNODE_INCLUDE_DIR=/usr/local/include/node && cmake --build build"

# TypeScript -> dist/ (typescript and @types/node installed in a docker volume, outside the repo)
docker run --rm --entrypoint sh -v fqdb-node-tools:/opt/tsdev -v $REPO/bindings/node:/src -w /src fqdb-lang-dev:1 -c \
  "/opt/tsdev/node_modules/.bin/tsc -p tsconfig.json --typeRoots /opt/tsdev/node_modules/@types"

# tests
docker run --rm --entrypoint sh -v $REPO/build/lang:/lang -v $REPO/bindings/node:/src -w /src \
  -e LD_LIBRARY_PATH=/lang/linux/lib -e FQDB_CONFORMANCE=/lang/conformance.json fqdb-lang-dev:1 -c "node --test 'dist/test/*.test.js'"

# benchmark, C baseline and Node in the same container (also needs -v .../tools/lang_conformance:/tools:ro)
sh bench/run.sh 2
```

Outside Docker: `npm install && npm run build && npm test` (set `FQDB_NATIVE_PATH` to use a `.node` in another place).
The addon is found at `build/fengqudb_native.node`; `libfengqudb` must be loadable (`LD_LIBRARY_PATH` or rpath).

## Usage

```ts
import { Store, from, u64, text, s64, count } from "fengqudb";

const db = Store.open("/data/game");  // new database: performance mode
db.put("player/42", bytes, { expiresAtMs: Date.now() + 60_000 });
const copy = db.get("player/42");                                  // Uint8Array copy or null

const shared = db.getShared("player/42");                          // zero-copy view or null
if (shared) { consume(shared.bytes); shared.release(); }

db.withShared("player/42", (bytes) => bytes ? parse(bytes) : null); // view valid only inside the callback

const key = db.prepareKey("player/42");                            // key marshalled once
key.withShared((b) => b && b[0]);   key.peekByte(-1);   key.getInto(scratch);

const reader = db.prepareReader(["player/1", "player/2"]);         // 1..4096 keys
reader.read();                                                     // returns the row count, reuses descriptors
for (let i = 0; i < 2; i++) if (reader.found(i)) consume(reader.value(i)!);   // view valid until next read()

const tx = db.transaction(); tx.put("a", "1"); tx.commit();        // commit closes; otherwise tx.close() rolls back
db.defineCompoundIndex("class-level", "p/", [[2, 4], [1, 1]], INDEX_REQUIRED);
db.dropIndex("class-level");
```

Keys and values may be `Uint8Array`/`Buffer`, `ArrayBuffer` or UTF-8 `string`. Errors are `FengquDBError` with a numeric
`status` (core status codes, see `Status`) and `statusName`. Status `-1` (`Status.CLOSED`) is the binding's own
"handle closed or released" error. Wrong argument types throw `TypeError`/`RangeError`.
Misses are `null` (`get`, `getShared`, `txn.get`), and `delete` returns whether a key was removed.

### Queries

```ts
const uid = u64(1), cls = text(2), level = u64(3), gold = s64(4);
const rows = from("p/")
  .where(cls.eq("mage").and(level.between(20, 60)).and(gold.ge(1000)))
  .select(uid, gold).orderBy(gold.desc(), uid).limit(20).run(db);   // db: Store or Transaction
for (const row of rows) console.log(row.get(uid), row.get(gold));   // bigint | number | Uint8Array | boolean | null
rows.close();                                                       // optional: returns the native buffer now
```

Available: `eq/ne/lt/le/gt/ge`, `between`, `in`, `isNull`, `exists`, `like/ilike/regex/contains/startsWith/endsWith/icontains/fuzzy`,
`and/or/not`, `.any()` for repeated fields, `nested(path, type)`, `join(prefix, left, right, kind)`,
`count/countOf/countDistinct/sum/avg/min/max`, `groupBy/having`, `offset/after/distinct/countTotal/index/...`,
`build()` (low-level `Query`), `prepare()`, `explain()`, `pages(db)` (paging until the cursor is empty).
The low-level `Query` / `Expr` / `Field` / `Column` / `Order` / `Join` model is in `src/query.ts`.
It encodes byte-for-byte like `fengqudb_select.py`, checked against `conformance.json`.

- Integers (uint64/int64) always decode to **`bigint`**, doubles to `number`.
- In a query value a bare integer `number` is encoded as an integer and a non-integer as a double. Use `new Double(5)`
  to force a double. Typed `f64/f32` columns do this for you.
- A result is decoded **in place on the native response buffer**. Construction scans the top level once to record row
  offsets. `row.get(i)`/`row.key` decode on demand. `bytes` values and keys are zero-copy views (pass `copy=true` or use
  `keyCopy()` to copy). `result.cursor` is copied at construction, so it survives `close()`.

### worker_threads

A `Store` handle belongs to one thread. To share a database: `const token = store.share()` (once per worker), pass it in
`workerData`, then `Store.attach(token)` in the worker. Each worker uses its own `prepareReader`/`prepareKey`.
A token must be consumed exactly once (`Store.attach` or `Store.discard`). The underlying store is closed when the last
handle on any thread has been closed (see `test/worker.test.ts`).

## Lifecycle rules

- A shared view points at native memory and holds a lease that counts against `pinnedBytes` (128 MiB by default).
  The lease is returned by `release()`/`releaseShared()`, by the end of a `withShared` callback, or by GC when the
  `ArrayBuffer` is collected (a native finalizer calls `fqdb_store_value_close`).
- **After `release()`, or after the callback returns, the `ArrayBuffer` is detached.** Earlier `Uint8Array`s and
  `subarray`s over it have length 0, so there is no dangling pointer. `SharedValue.bytes` throws `FengquDBError(CLOSED)`.
  Copies made with `.slice()` stay valid.
- **Do not rely on GC to return leases in a hot loop.** GC returns them late, because finalizers are deferred to the
  event loop. Measured: a synchronous loop of `getSharedBytes` on a 256 KiB value without `release()` and with
  `pinnedBytes` = 4 MiB failed with `LIMIT` after 15 reads. Release explicitly, or use `withShared`, `peekByte` or a reader.
  Test `lease return` also checks that `release()` and (after `gc()` plus an event-loop turn) the finalizers give the budget back.
- A closed `Store` handle makes further calls on that handle throw. Views, prepared keys and readers obtained earlier
  stay valid after `Store.close()` (core guarantee). Transactions and snapshots hold a reference, so `Store.close()`
  with an open transaction postpones the real `fqdb_store_close` until it is closed. The directory lock stays held until
  every lease, prepared key and reader is gone (tested: reopening works after releasing the lease).
- Reader views are valid until the next `read()/release()/close()` and are detached then (also when the reader is
  collected). A reader is single-threaded.
- `durability: "buffered"` is a manual-sync store: `close()` does **not** sync (observed). Call `sync()` first.
- Electron (V8 sandbox) does not allow external ArrayBuffers; this addon targets plain Node (checked on Node 24 only).

## Benchmarks

Command: `sh bench/run.sh 2` in the container. Data and operations are the same as `tools/lang_conformance/bench_c.c`:
32768 records of 256 B, a 9-byte key, 2 s per op, and the same 64 `p/` documents with the `topn_mage_level` request.
The C baseline ran in the same container, right before the Node run. Each figure is **a single 2-second run** on one
machine; run-to-run noise of 10-20 % is plausible, so differences between the view forms below (≈ 800-1300 ns)
should not be over-read.

Rows whose op name matches `bench_c` use the interface closest to C. Rows with `:suffix` are other interface forms
of the same operation.

| op | C ns/op | Node ns/op | Node / C | interface form |
| --- | ---: | ---: | ---: | --- |
| get_shared_prepared | 42.3 | 1050.2 | 24.8x | `pk.getSharedBytes()` + `releaseShared()` (new view per read) |
| get_shared_prepared:view_gc |  | 1281.2 | 30.3x | same, lease returned by GC (yields to the event loop every 1000) |
| get_shared_prepared:wrapper |  | 919.8 | 21.7x | `pk.getShared()` (SharedValue object) + `release()` |
| get_shared_prepared:callback |  | 778.8 | 18.4x | `pk.withShared(fn)` (view still created, no finalizer) |
| get_shared_prepared:peek |  | 83.8 | 2.0x | `pk.peekByte(-1)` (native call only, no JS object) |
| get_shared_prepared:copy_into |  | 61.4 | 1.5x | `pk.getInto(scratch)` (copies 256 B) |
| get_shared_prepared:copy_alloc |  | 641.9 | 15.2x | `pk.get()` (allocates a Buffer and copies) |
| get_shared_key | 333.5 | 1912.2 | 5.7x | `store.getSharedBytes(key)` + `releaseShared()` |
| get_shared_key:callback |  | 1622.3 | 4.9x | `store.withShared(key, fn)` |
| get_copy | 361.5 | 1114.2 | 3.1x | `store.get(key)` |
| reader32_per_record | 0.7 | 815.2 | ~1160x | `reader.read()` + `reader.value(i)[0]` (a view per row) |
| reader32_per_record:copy_into |  | 33.3 | 48x | `reader.copyValue(i, scratch)` |
| reader32_per_record:descriptor_only |  | 2.3 | 3.3x | `reader.read()` + `reader.length(i)` (no value access) |
| select_topn | 11593.4 | 13719.0 | 1.18x | `store.selectRaw(req)` + `releaseShared` (bytes only) |
| select_topn:result_object |  | 13852.7 | 1.19x | `new QueryResult(...)`: top-level scan, `byteLength`, `close()` |
| select_topn:decode_all_rows |  | 14086.1 | 1.21x | the above plus decoding both columns of all 7 rows |

(The spec's C baseline figures, 41.9 / 360 / 370 / 11.3 µs, agree with this same-run baseline.)

### Why the gaps

- **Creating a view costs about 1 µs.** An external `ArrayBuffer`, its `Uint8Array`, the lease wrap and the later
  detach or finalizer each allocate V8 objects. The 1050 ns for `get_shared_prepared` against 84 ns for `peekByte`
  (same native lease, no JS object) puts the object creation at roughly 1 µs. So the 25x ratio against the 42 ns C call
  is mostly JS object creation, not the database. A plain native call from JS costs about 60-80 ns here.
- `withShared` avoids the finalizer and the `napi_wrap`, which gave 779 ns against 1050 ns. It still creates the
  `ArrayBuffer` and `Uint8Array`, so it does not remove the main cost. The `wrapper` and `view_gc` rows are within
  noise of the plain view row. I did not find a form that exposes native memory as a typed array without paying that
  cost. Only `peekByte`, `getInto` and `copyValue` avoid it, and they copy or read bytes instead of returning a view.
- `get_shared_key` / `get_copy` add the key lookup (about 290 ns in C). Node adds more than the C difference over the
  prepared form (1912 − 1050 = 860 ns against 333 − 42 = 290 ns). I did not isolate why; key marshalling
  (`napi_get_typedarray_info`) is cheap, so the extra time is presumably cache misses and object churn at random keys,
  not measured. `get_copy` (3.1x) is the smallest ratio because C also allocates and copies there.
- **`reader32_per_record` is the worst ratio (about 1160x) and I did not improve it beyond the forms listed.**
  C reads the first byte of rows the reader already holds, which is almost free (0.7 ns). Node needs a view per row
  (about 800 ns: external buffer, a weak reference so the view can be detached on the next `read()`, and the
  detach). If the caller only needs the descriptor the cost is 2.3 ns/record, and reading bytes through
  `copyValue` costs 33 ns/record. For batch reads, prefer `copyValue`/`length`, or parse straight from `value(i)` only
  when a view is really needed.
- `select_topn` is only 18-21 % above C. The query itself costs 11.6 µs, so call overhead and view creation (~1 µs)
  are small. Lazy decoding of all 7 rows adds 0.4 µs.

Rule of thumb from these numbers: any API that returns a typed array over native memory costs about 0.8-1.3 µs. If a
handler makes thousands of reads per request, use `peekByte`, `getInto`, `copyValue` or a reader and keep views for
larger payloads, where one microsecond is small against a copy.

## Files

- `native/`: `common.h`, `module.cpp` (entry, errors, input marshalling, lease wrapping), `store.cpp` (store, transactions,
  snapshots, indexes, select), `shared.cpp` (shared reads, prepared keys, readers).
- `src/`: `native.ts` (loader and native typings), `store.ts` (API classes), `query.ts` (low-level model and encoding),
  `result.ts` (lazy response decoder), `builder.ts` (typed builder), `wire.ts`, `errors.ts`, `index.ts`.
- `test/`: node:test suites (conformance, store/lifecycle, worker_threads). `bench/`: `bench.ts`, `run.sh`.

## Known limits

- Windows/MSVC and macOS are unverified, and no prebuilt binaries are provided (build from source).
- Not wrapped: patch, compare-exchange/atomic counters, watch, backup/restore/compact, history holds, `get_many`,
  project readers, `fqdb_store_txn_execute` for non-SELECT operations, `put_submit`/commits, runtime configuration.
- `QueryStats` counters are `number` (they are small counters, not 64-bit identifiers).
- The index-field and `Status` constants are copied from the headers by hand.
