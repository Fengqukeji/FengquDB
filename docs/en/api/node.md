# Node.js / TypeScript API and examples

[Getting started](../languages/node.md) · [简体中文](../../api/node.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read

```js
const data = db.get('student/7');
const student = data === null ? null : Codec.decode(data).value;
```

A missing record returns null; an empty Uint8Array is valid data. Codec is StudentFqCodec generated in the getting-started guide.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.ResultRow` | `get(index: number, copy = false): ResultValue` | Read data or status |
| `fengqudb.Store` | `get(key: BytesLike): Uint8Array \| null` | Read data or status |

</details>

## 2. Insert, save, and store strings or integers

```js
db.put(Codec.key(student), Codec.encode(student));
db.put('name', 'Alice');
const number = Buffer.alloc(8); number.writeBigInt64LE(95n);
db.put('score', number);
const score = Buffer.from(db.get('score')).readBigInt64LE();
```

put overwrites the previous value at the same key. Use bigint for 64-bit integers.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `put(key: BytesLike, value: BytesLike, options?: WriteOptions): bigint` | Save or set data |

</details>

## 3. Update

```js
const bytes = db.get('student/7');
if (bytes !== null) {
  const row = Codec.decode(bytes);
  row.value.score = 96;
  db.put(row.fqKey(), row.fqEncode());
}
```

Put multi-key updates in a transaction. The Node package currently has no convenience methods for atomic counters or binary patching.

## 4. Delete

```js
const removed = db.delete('student/7');
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `delete(key: BytesLike, options?: { forceReliable?: boolean }): boolean` | Delete a record or member |

</details>

## 5. Batch updates

```js
const tx = db.transaction();
try { tx.putMany([['a','one'],['b','two']]); tx.commit(); }
finally { tx.close(); }
```

The whole batch is committed as one transaction.

## 6. Transactions and savepoints

```js
const tx = db.transaction();
try {
  tx.put('a','one');
  const point = tx.savepoint();
  tx.put('temporary','discard');
  tx.rollbackTo(point);
  tx.commit();
} finally { tx.close(); }
```

commit closes the transaction; closing without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute.

<details>
<summary>Functions and parameters (15)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `transaction(timeoutMs?: Millis): Transaction` | Transaction operation |
| `fengqudb.Transaction` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.Transaction` | `get closed(): boolean` | Release an object or borrowed resource |
| `fengqudb.Transaction` | `commit(forceReliable = false): bigint` | Commit a transaction or inspect its result |
| `fengqudb.Transaction` | `constructor(h: Handle)` | Transaction operation |
| `fengqudb.Transaction` | `delete(key: BytesLike): void` | Delete a record or member |
| `fengqudb.Transaction` | `deleteMany(keys: BytesLike[]): void` | Delete a record or member |
| `fengqudb.Transaction` | `get(key: BytesLike): Uint8Array \| null` | Read data or status |
| `fengqudb.Transaction` | `put(key: BytesLike, value: BytesLike, expiresAtMs?: Millis): void` | Save or set data |
| `fengqudb.Transaction` | `putMany(items: ReadonlyArray<readonly [BytesLike, BytesLike, Millis?]>): void` | Save or set data |
| `fengqudb.Transaction` | `releaseSavepoint(id: bigint): void` | Release an object or borrowed resource |
| `fengqudb.Transaction` | `rollbackTo(id: bigint): void` | Roll back to a savepoint |
| `fengqudb.Transaction` | `savepoint(): bigint` | Create or manage a transaction savepoint |
| `fengqudb.Transaction` | `select(q: Query \| PreparedQuery): QueryResult` | Transaction operation |
| `fengqudb.Transaction` | `selectRaw(request: Uint8Array): Uint8Array` | Transaction operation |

</details>

## 7. Filtering, sorting, and aggregation

```js
const {from, s64, text} = require('./FengquDB/node');
const score = s64(Codec.fields.score), name = text(Codec.fields.name);
const rows = from('student/').where(score.ge(90n))
  .select(name,score).orderBy(score.desc()).limit(10).run(db);
try { for(const row of rows) console.log(row.get(name),row.get(score)); }
finally { rows.close(); }
```

and/or/not combine conditions, groupBy/having aggregates groups, and join performs equality joins. prepare reuses a query, and explain shows index use. Column types must match the encoding; ordinary number fields use f64 by default.

<details>
<summary>Functions and parameters (111)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `export function and(...cs: Cond[]): Cond` | Build a query or read its results |
| `fengqudb` | `export function from(prefix: Uint8Array \| string): QueryBuilder` | Build a query or read its results |
| `fengqudb` | `export function not(c: Cond): Cond` | Build a query or read its results |
| `fengqudb` | `export function or(...cs: Cond[]): Cond` | Build a query or read its results |
| `fengqudb.Agg` | `asc(nullsFirst = true): Sort` | Build a query or read its results |
| `fengqudb.Agg` | `column(): Column` | Build a query or read its results |
| `fengqudb.Agg` | `constructor(readonly col: Column)` | Build a query or read its results |
| `fengqudb.Agg` | `desc(nullsFirst = true): Sort` | Build a query or read its results |
| `fengqudb.Agg` | `eq(v: IntLike \| number): Cond` | Build a query or read its results |
| `fengqudb.Agg` | `ge(v: IntLike \| number): Cond` | Build a query or read its results |
| `fengqudb.Agg` | `gt(v: IntLike \| number): Cond` | Build a query or read its results |
| `fengqudb.Agg` | `le(v: IntLike \| number): Cond` | Build a query or read its results |
| `fengqudb.Agg` | `lt(v: IntLike \| number): Cond` | Build a query or read its results |
| `fengqudb.Agg` | `ne(v: IntLike \| number): Cond` | Build a query or read its results |
| `fengqudb.Col` | `any(): Col<V>` | Build a query or read its results |
| `fengqudb.Col` | `asc(nullsFirst = true): Sort` | Build a query or read its results |
| `fengqudb.Col` | `between(low: V, high: V): Cond` | Build a query or read its results |
| `fengqudb.Col` | `column(): Column` | Build a query or read its results |
| `fengqudb.Col` | `constructor(readonly field: Field, private readonly cast: Cast = undefined)` | Build a query or read its results |
| `fengqudb.Col` | `contains(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Col` | `desc(nullsFirst = true): Sort` | Build a query or read its results |
| `fengqudb.Col` | `endsWith(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Col` | `eq(v: V \| Col<unknown>): Cond` | Build a query or read its results |
| `fengqudb.Col` | `exists(): Cond` | Build a query or read its results |
| `fengqudb.Col` | `fuzzy(s: string \| Uint8Array, distance: number): Cond` | Build a query or read its results |
| `fengqudb.Col` | `ge(v: V \| Col<unknown>): Cond` | Build a query or read its results |
| `fengqudb.Col` | `gt(v: V \| Col<unknown>): Cond` | Build a query or read its results |
| `fengqudb.Col` | `icontains(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Col` | `ilike(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Col` | `in(values: readonly V[]): Cond` | Build a query or read its results |
| `fengqudb.Col` | `in_(values: readonly V[]): Cond` | Build a query or read its results |
| `fengqudb.Col` | `isNull(): Cond` | Build a query or read its results |
| `fengqudb.Col` | `le(v: V \| Col<unknown>): Cond` | Build a query or read its results |
| `fengqudb.Col` | `like(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Col` | `lt(v: V \| Col<unknown>): Cond` | Build a query or read its results |
| `fengqudb.Col` | `ne(v: V \| Col<unknown>): Cond` | Build a query or read its results |
| `fengqudb.Col` | `regex(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Col` | `startsWith(s: string \| Uint8Array): Cond` | Build a query or read its results |
| `fengqudb.Column` | `constructor( readonly field: Field = new Field(), readonly aggregate: Aggregate = Aggregate.NONE, readonly name = "", readonly distinct = false, )` | Build a query or read its results |
| `fengqudb.Column` | `encode(): Uint8Array` | Build a query or read its results |
| `fengqudb.Cond` | `and(o: Cond): Cond` | Build a query or read its results |
| `fengqudb.Cond` | `build(columns: readonly Column[]): Expr` | Build a query or read its results |
| `fengqudb.Cond` | `constructor(private readonly resolve: (columns: readonly Column[]) => Expr, readonly aggregate = false)` | Build a query or read its results |
| `fengqudb.Cond` | `not(): Cond` | Build a query or read its results |
| `fengqudb.Cond` | `or(o: Cond): Cond` | Build a query or read its results |
| `fengqudb.Expr` | `and(o: Expr): Expr` | Build a query or read its results |
| `fengqudb.Expr` | `not(): Expr` | Build a query or read its results |
| `fengqudb.Expr` | `or(o: Expr): Expr` | Build a query or read its results |
| `fengqudb.PreparedBuilder` | `constructor(readonly prepared: PreparedQuery, private readonly columns: readonly Column[])` | Build a query or read its results |
| `fengqudb.PreparedBuilder` | `run(db: Selectable): Rows` | Build a query or read its results |
| `fengqudb.PreparedQuery` | `constructor(encoded: Uint8Array)` | Build a query or read its results |
| `fengqudb.PreparedQuery` | `execute(db: Selectable): QueryResult` | Build a query or read its results |
| `fengqudb.PreparedQuery` | `explain(db: Selectable): QueryResult` | Inspect the query plan |
| `fengqudb.PreparedQuery` | `request(explain = false): Uint8Array` | Build a query or read its results |
| `fengqudb.Query` | `constructor(prefix: Uint8Array \| string, o: QueryOptions = {})` | Build a query or read its results |
| `fengqudb.Query` | `encode(): Uint8Array` | Build a query or read its results |
| `fengqudb.Query` | `execute(db: Selectable): QueryResult` | Build a query or read its results |
| `fengqudb.Query` | `explain(db: Selectable): QueryResult` | Inspect the query plan |
| `fengqudb.Query` | `*pages(db: Selectable): Generator<QueryResult, void, undefined>` | Build a query or read its results |
| `fengqudb.Query` | `prepare(): PreparedQuery` | Build a query or read its results |
| `fengqudb.Query` | `request(explain = false): Uint8Array` | Build a query or read its results |
| `fengqudb.Query` | `withAfter(cursor: Uint8Array): Query` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `after(cursor: Uint8Array): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `allowScan(on: boolean): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `build(): Query` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `constructor(prefix: Uint8Array \| string)` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `countTotal(on = true): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `distinct(on = true): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `explain(db: Selectable): QueryResult` | Inspect the query plan |
| `fengqudb.QueryBuilder` | `groupBy(...cols: Col<any>[]): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `having(c: Cond): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `index(name: string \| Uint8Array): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `join(prefix: Uint8Array \| string, left: Col<any>, right: Col<any>, kind: JoinKind = JoinKind.INNER, on?: Cond): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `limit(n: IntLike): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `maxCandidates(n: IntLike): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `maxWork(n: IntLike): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `memory(n: IntLike): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `offset(n: IntLike): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `orderBy(...items: Array<Sort \| Col<any> \| Agg>): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `*pages(db: Selectable): Generator<Rows, void, undefined>` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `prepare(): PreparedBuilder` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `run(db: Selectable): Rows` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `select(...items: Selectation[]): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `timeoutMs(n: IntLike): this` | Build a query or read its results |
| `fengqudb.QueryBuilder` | `where(c: Cond): this` | Build a query or read its results |
| `fengqudb.QueryResult` | `*[Symbol.iterator](): Iterator<ResultRow>` | Build a query or read its results |
| `fengqudb.QueryResult` | `get access(): AccessPlan[]` | Build a query or read its results |
| `fengqudb.QueryResult` | `get byteLength(): number` | Build a query or read its results |
| `fengqudb.QueryResult` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.QueryResult` | `constructor(response: Uint8Array)` | Build a query or read its results |
| `fengqudb.QueryResult` | `get cursor(): Uint8Array` | Build a query or read its results |
| `fengqudb.QueryResult` | `data(): Uint8Array` | Build a query or read its results |
| `fengqudb.QueryResult` | `get length(): number` | Build a query or read its results |
| `fengqudb.QueryResult` | `row(i: number): ResultRow` | Build a query or read its results |
| `fengqudb.QueryResult` | `get stats(): QueryStats` | Build a query or read its results |
| `fengqudb.Rows` | `*[Symbol.iterator](): Iterator<TypedRow>` | Build a query or read its results |
| `fengqudb.Rows` | `get access(): AccessPlan[]` | Build a query or read its results |
| `fengqudb.Rows` | `at(i: number): TypedRow` | Build a query or read its results |
| `fengqudb.Rows` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.Rows` | `constructor(private readonly columns: readonly Column[], readonly result: QueryResult)` | Build a query or read its results |
| `fengqudb.Rows` | `get cursor(): Uint8Array` | Build a query or read its results |
| `fengqudb.Rows` | `get length(): number` | Build a query or read its results |
| `fengqudb.Rows` | `get stats(): QueryStats` | Build a query or read its results |
| `fengqudb.Selectable` | `selectRaw(request: Uint8Array): Uint8Array;` | Build a query or read its results |
| `fengqudb.Sort` | `constructor(readonly build: (columns: readonly Column[]) => Order)` | Build a query or read its results |
| `fengqudb.Store` | `explain(q: Query \| PreparedQuery): QueryResult` | Inspect the query plan |
| `fengqudb.Store` | `select(q: Query \| PreparedQuery): QueryResult` | Build a query or read its results |
| `fengqudb.Store` | `selectRaw(request: Uint8Array): Uint8Array` | Build a query or read its results |
| `fengqudb.TypedRow` | `constructor(private readonly columns: readonly Column[], readonly raw: ResultRow)` | Build a query or read its results |
| `fengqudb.TypedRow` | `get(item: Selectation, copy = false): ResultValue` | Read data or status |
| `fengqudb.TypedRow` | `get key(): Uint8Array` | Build a query or read its results |

</details>

## 8. Indexes

```js
db.defineCompoundIndex('student-score', 'student/', [[Codec.fields.score,5]], 0);
```

5 means a signed ZigZag integer.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `defineCompoundIndex(name: BytesLike, keyPrefix: BytesLike, fields: readonly IndexField[], flags = 0): MaintenanceInfo` | Define or use an index |
| `fengqudb.Store` | `dropIndex(name: BytesLike): MaintenanceInfo` | Define or use an index |

</details>

## 9. Critical writes: wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```js
db.put('payment/receipt', 'paid', {forceReliable:true});
db.sync();
```

Use commit(true) for transactions. Switch the whole database with `db.configureWrite({mode:'reliable'})`, or select performance mode with `db.configureWrite({mode:'performance', periodMs:50, thresholdBytes:1024*1024})`. Omitted parameters retain their current values. `db.writeOptions()` reads the settings, and `db.resetWriteOptions()` restores default performance mode. Settings are saved and survive reopening.

The API is synchronous. Run reliable writes and large queries in a worker to avoid blocking the event loop. On COMMIT_UNKNOWN, reconcile by business operation ID instead of charging again.

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `configureWrite(options: WritePolicyOptions): void` | Control durability or wait for a commit |
| `fengqudb.Store` | `resetWriteOptions(): void` | Control durability or wait for a commit |
| `fengqudb.Store` | `sync(): void` | Control durability or wait for a commit |
| `fengqudb.Store` | `writeOptions(): WritePolicy` | Control durability or wait for a commit |

</details>

## 10. Expiring data

```js
db.put('verification', '1234', {expiresAtMs:Date.now()+300_000});
```

Expiry is an absolute Unix timestamp in milliseconds; 0 means no expiry.

## 11. Hot-path reads and shared bytes

```js
db.withShared('student/7', bytes => { if(bytes) consume(bytes); });
const reader = db.prepareReader(['student/7','student/8']);
try { reader.read(); if(reader.found(0)) consume(reader.value(0)); }
finally { reader.close(); }
```

Shared bytes are read-only and must not be used after the callback ends or the lease is released. Reader views remain valid only until the next read/release/close. Copy them if they must live longer.

<details>
<summary>Functions and parameters (35)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `export function releaseShared(view: Uint8Array): boolean` | Release an object or borrowed resource |
| `fengqudb.PreparedKey` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.PreparedKey` | `get closed(): boolean` | Release an object or borrowed resource |
| `fengqudb.PreparedKey` | `constructor(h: Handle)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedKey` | `get(): Uint8Array \| null` | Read data or status |
| `fengqudb.PreparedKey` | `getInto(target: Uint8Array): number` | Read data or status |
| `fengqudb.PreparedKey` | `getShared(): SharedValue \| null` | Read data or status |
| `fengqudb.PreparedKey` | `getSharedBytes(): Uint8Array \| null` | Read data or status |
| `fengqudb.PreparedKey` | `peekByte(index: number): number` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedKey` | `withShared<T>(fn: (bytes: Uint8Array \| null) => T): T` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.PreparedReader` | `get closed(): boolean` | Release an object or borrowed resource |
| `fengqudb.PreparedReader` | `constructor(store: Handle, keys: BytesLike[])` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `copyValue(i: number, target: Uint8Array): number` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `expiresAtMs(i: number): bigint` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `forEach(fn: (index: number, value: Uint8Array \| null) => void): void` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `found(i: number): boolean` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `length(i: number): number` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `read(): number` | Read data or status |
| `fengqudb.PreparedReader` | `release(): void` | Release an object or borrowed resource |
| `fengqudb.PreparedReader` | `value(i: number): Uint8Array \| null` | Reuse keys, buffers, or shared bytes |
| `fengqudb.PreparedReader` | `version(i: number): bigint` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `get bytes(): Uint8Array` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `constructor(view: Uint8Array)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `copy(): Uint8Array` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `get expiresAtMs(): bigint` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `get length(): number` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `release(): void` | Release an object or borrowed resource |
| `fengqudb.SharedValue` | `get released(): boolean` | Release an object or borrowed resource |
| `fengqudb.SharedValue` | `get version(): bigint` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Store` | `getShared(key: BytesLike): SharedValue \| null` | Read data or status |
| `fengqudb.Store` | `getSharedBytes(key: BytesLike): Uint8Array \| null` | Read data or status |
| `fengqudb.Store` | `prepareKey(key: BytesLike): PreparedKey` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Store` | `prepareReader(keys: BytesLike[]): PreparedReader` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Store` | `withShared<T>(key: BytesLike, fn: (bytes: Uint8Array \| null) => T): T` | Reuse keys, buffers, or shared bytes |

</details>

## 12. Consistent snapshots

```js
const snapshot = db.snapshot();
try { const value = snapshot.get('student/7'); }
finally { snapshot.close(); }
```

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Snapshot` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.Snapshot` | `constructor(h: Handle)` | Consistent reads or range iteration |
| `fengqudb.Snapshot` | `get(key: BytesLike): Uint8Array \| null` | Read data or status |
| `fengqudb.Snapshot` | `getShared(key: BytesLike): SharedValue \| null` | Read data or status |
| `fengqudb.Store` | `snapshot(): Snapshot` | Consistent reads or range iteration |

</details>

## maintenance

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `info(): StoreInfo` | Inspect status or maintain the database |

</details>

## 13. Entity encoding and decoding

Codec.encode/decode uses your Student. Use the record's fqEncode when rewriting it to preserve unknown fields. encode(value, destination) reuses a Uint8Array and returns the actual encoded range. npm prebuild runs generation; the SDK includes the generator.

## Other utilities

<details>
<summary>Functions and parameters (30)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `export function buildRequest(operation: number, value: Uint8Array): Uint8Array` | Utility operation |
| `fengqudb` | `export function bytesToHex(b: Uint8Array): string` | Utility operation |
| `fengqudb` | `export function hexToBytes(hex: string): Uint8Array` | Utility operation |
| `fengqudb.Double` | `constructor(readonly value: number)` | Utility operation |
| `fengqudb.Expr` | `constructor( readonly op: Op = Op.TRUE, readonly field: Field = new Field(), readonly values: readonly QueryValue[] = [], readonly children: readonly Expr[] = [], readonly right: Field \| null = null, )` | Utility operation |
| `fengqudb.Expr` | `encode(depth = 0): Uint8Array` | Utility operation |
| `fengqudb.FengquDBError` | `constructor(status: number, message?: string)` | Utility operation |
| `fengqudb.FengquDBError` | `get statusName(): string` | Utility operation |
| `fengqudb.Field` | `any(): Field` | Utility operation |
| `fengqudb.Field` | `static column(index: number): Field` | Utility operation |
| `fengqudb.Field` | `constructor( readonly number = 0, readonly type = 4, readonly source = 0, readonly path: readonly number[] = [], /** For a repeated field, any matching element makes the condition true. Only valid in primary-source WHERE leaf conditions. */ readonly repeated = false, )` | Utility operation |
| `fengqudb.Field` | `encode(): Uint8Array` | Utility operation |
| `fengqudb.Field` | `equals(o: Field): boolean` | Utility operation |
| `fengqudb.Field` | `static key(source = 0): Field` | Utility operation |
| `fengqudb.Field` | `static record(source = 0): Field` | Utility operation |
| `fengqudb.Join` | `constructor( readonly prefix: Uint8Array \| string, readonly left: readonly Field[], readonly right: readonly Field[], readonly kind: JoinKind = JoinKind.INNER, readonly on: Expr = new Expr(), )` | Utility operation |
| `fengqudb.Join` | `encode(): Uint8Array` | Utility operation |
| `fengqudb.Order` | `constructor(readonly field: Field, readonly descending = false, readonly nullsFirst = true)` | Utility operation |
| `fengqudb.Order` | `encode(): Uint8Array` | Utility operation |
| `fengqudb.ResultRow` | `constructor(private readonly r: QueryResult, private readonly start: number, private readonly end: number)` | Utility operation |
| `fengqudb.ResultRow` | `get key(): Uint8Array` | Utility operation |
| `fengqudb.ResultRow` | `keyCopy(): Uint8Array` | Utility operation |
| `fengqudb.ResultRow` | `get length(): number` | Utility operation |
| `fengqudb.ResultRow` | `values(copy = false): ResultValue[]` | Utility operation |
| `fengqudb.Store` | `static attach(token: StoreToken): Store` | Utility operation |
| `fengqudb.Store` | `close(): void` | Release an object or borrowed resource |
| `fengqudb.Store` | `get closed(): boolean` | Release an object or borrowed resource |
| `fengqudb.Store` | `static discard(token: StoreToken): void` | Utility operation |
| `fengqudb.Store` | `static open(path: string, options?: OpenOptions): Store` | Utility operation |
| `fengqudb.Store` | `share(): StoreToken` | Utility operation |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>fengqudb</summary>

| Declaration |
| --- |
| `interface AccessPlan` |
| `class Agg` |
| `export enum Aggregate` |
| `export type BytesLike = Uint8Array \| ArrayBuffer \| string;` |
| `class Col<V = QueryValue>` |
| `class Column` |
| `class Cond` |
| `class Double` |
| `class Expr` |
| `class FengquDBError extends Error` |
| `class Field` |
| `FieldType =` |
| `INDEX_REQUIRED = 2` |
| `INDEX_UNIQUE = 1` |
| `export type IndexField = readonly [field: number, type: number];` |
| `export type IntLike = number \| bigint;` |
| `class Join` |
| `export enum JoinKind` |
| `interface MaintenanceInfo` |
| `export type Millis = number \| bigint;` |
| `OP_EXPLAIN = 54` |
| `OP_SELECT = 53` |
| `export enum Op` |
| `interface OpenOptions` |
| `class Order` |
| `class PreparedBuilder` |
| `class PreparedKey` |
| `class PreparedQuery` |
| `class PreparedReader` |
| `class Query` |
| `class QueryBuilder` |
| `interface QueryOptions` |
| `class QueryResult implements Iterable<ResultRow>` |
| `interface QueryStats` |
| `export type QueryValue = null \| boolean \| bigint \| number \| string \| Uint8Array \| Double;` |
| `class ResultRow` |
| `export type ResultValue = null \| bigint \| number \| boolean \| Uint8Array;` |
| `class Rows implements Iterable<TypedRow>` |
| `interface Selectable` |
| `class SharedValue` |
| `class Snapshot` |
| `class Sort` |
| `Status =` |
| `class Store implements Selectable` |
| `interface StoreInfo` |
| `export type StoreToken = bigint &` |
| `class Transaction implements Selectable` |
| `class TypedRow` |
| `interface WriteOptions` |
| `interface WritePolicy` |
| `interface WritePolicyOptions` |
| `avg = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.AVG))` |
| `boolean = (n: number, source = 0): Col<boolean> => new Col(new Field(n, FieldType.BOOL, source))` |
| `count = (): Agg => new Agg(new Column(Field.key(), Aggregate.COUNT_ALL))` |
| `countDistinct = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.COUNT, "", true))` |
| `countOf = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.COUNT))` |
| `f32 = (n: number, source = 0): Col<number> => new Col(new Field(n, FieldType.FLOAT, source), doubleCast)` |
| `f64 = (n: number, source = 0): Col<number> => new Col(new Field(n, FieldType.DOUBLE, source), doubleCast)` |
| `f64 = new DataView(new ArrayBuffer(8))` |
| `fixed32 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.FIXED32, source), intCast)` |
| `fixed64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.FIXED64, source), intCast)` |
| `i64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.INT64, source), intCast)` |
| `key = (source = 0): Col<string \| Uint8Array> => new Col(Field.key(source))` |
| `max = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.MAX))` |
| `min = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.MIN))` |
| `nested = (path: readonly number[], type: number, source = 0): Col<QueryValue> => new Col(new Field(0, type, source, path))` |
| `s64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.SINT64, source), intCast)` |
| `sfixed32 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.SFIXED32, source), intCast)` |
| `sfixed64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.SFIXED64, source), intCast)` |
| `sum = (c: Col<unknown>): Agg => new Agg(new Column(c.field, Aggregate.SUM))` |
| `text = (n: number, source = 0): Col<string \| Uint8Array> => new Col(new Field(n, FieldType.BYTES, source))` |
| `u64 = (n: number, source = 0): Col<IntLike> => new Col(new Field(n, FieldType.UINT64, source), intCast)` |

</details>

<details>
<summary>fengqudb.AccessPlan</summary>

| Declaration |
| --- |
| `covering?: boolean;` |
| `index?: string;` |
| `needs_scan?: boolean;` |
| `ordered?: boolean;` |
| `reason?: string;` |
| `sampled_candidates?: number;` |
| `source?: number;` |
| `strategy?: string;` |

</details>

<details>
<summary>fengqudb.Aggregate</summary>

| Declaration |
| --- |
| `AVG = 6` |
| `COUNT = 2` |
| `COUNT_ALL = 1` |
| `MAX = 5` |
| `MIN = 4` |
| `NONE = 0` |
| `SUM = 3` |

</details>

<details>
<summary>fengqudb.FengquDBError</summary>

| Declaration |
| --- |
| `readonly status: number;` |

</details>

<details>
<summary>fengqudb.JoinKind</summary>

| Declaration |
| --- |
| `ANTI = 3` |
| `INNER = 0` |
| `LEFT = 1` |
| `SEMI = 2` |

</details>

<details>
<summary>fengqudb.MaintenanceInfo</summary>

| Declaration |
| --- |
| `catchupRounds: bigint;` |
| `copiedKeys: bigint;` |
| `finalVersion: bigint;` |
| `outputFileBytes: bigint;` |
| `snapshotVersion: bigint;` |
| `sourceFileBytes: bigint;` |
| `storedKeys: bigint;` |
| `storedValueBytes: bigint;` |

</details>

<details>
<summary>fengqudb.Op</summary>

| Declaration |
| --- |
| `AND = 2` |
| `CONTAINS = 14` |
| `ENDS_WITH = 16` |
| `EQ = 5` |
| `EXISTS = 12` |
| `FALSE = 1` |
| `FUZZY = 22` |
| `GE = 10` |
| `GT = 9` |
| `ICONTAINS = 19` |
| `ILIKE = 20` |
| `IN = 11` |
| `IREGEX = 21` |
| `IS_NULL = 13` |
| `LE = 8` |
| `LIKE = 17` |
| `LT = 7` |
| `NE = 6` |
| `NOT = 4` |
| `OR = 3` |
| `REGEX = 18` |
| `STARTS_WITH = 15` |
| `TRUE = 0` |

</details>

<details>
<summary>fengqudb.OpenOptions</summary>

| Declaration |
| --- |
| `allTransactionBytes?: Millis;` |
| `cacheBytes?: Millis;` |
| `create?: boolean;` |
| `deltaBytes?: Millis;` |
| `durability?: "sync" \| "buffered";` |
| `maxSnapshots?: Millis;` |
| `pinnedBytes?: Millis;` |
| `segmented?: boolean;` |
| `transactionBytes?: Millis;` |
| `writerBytes?: Millis;` |

</details>

<details>
<summary>fengqudb.PreparedQuery</summary>

| Declaration |
| --- |
| `readonly encoded: Uint8Array;` |

</details>

<details>
<summary>fengqudb.PreparedReader</summary>

| Declaration |
| --- |
| `readonly capacity: number;` |

</details>

<details>
<summary>fengqudb.Query</summary>

| Declaration |
| --- |
| `after: Uint8Array;` |
| `allowScan: boolean;` |
| `columns: Column[];` |
| `countTotal: boolean;` |
| `distinct: boolean;` |
| `group: Field[];` |
| `having: Expr;` |
| `indexHint: Uint8Array \| string;` |
| `joins: Join[];` |
| `limit: number \| bigint;` |
| `maxCandidates: number \| bigint;` |
| `maxWork: number \| bigint;` |
| `memoryBytes: number \| bigint;` |
| `offset: number \| bigint;` |
| `order: Order[];` |
| `prefix: Uint8Array \| string;` |
| `timeoutMs: number \| bigint;` |
| `where: Expr;` |

</details>

<details>
<summary>fengqudb.QueryOptions</summary>

| Declaration |
| --- |
| `after?: Uint8Array;` |
| `allowScan?: boolean;` |
| `columns?: Column[];` |
| `countTotal?: boolean;` |
| `distinct?: boolean;` |
| `group?: Field[];` |
| `having?: Expr;` |
| `indexHint?: Uint8Array \| string;` |
| `joins?: Join[];` |
| `limit?: number \| bigint;` |
| `maxCandidates?: number \| bigint;` |
| `maxWork?: number \| bigint;` |
| `memoryBytes?: number \| bigint;` |
| `offset?: number \| bigint;` |
| `order?: Order[];` |
| `timeoutMs?: number \| bigint;` |
| `where?: Expr;` |

</details>

<details>
<summary>fengqudb.QueryStats</summary>

| Declaration |
| --- |
| `candidates?: number;` |
| `complete?: boolean;` |
| `join_probes?: number;` |
| `matched?: number;` |
| `payload_reads?: number;` |
| `peak_memory?: number;` |
| `result_total?: number;` |
| `work?: number;` |

</details>

<details>
<summary>fengqudb.StoreInfo</summary>

| Declaration |
| --- |
| `cacheBytes: bigint;` |
| `deltaBytes: bigint;` |
| `durableVersion: bigint;` |
| `fileBytes: bigint;` |
| `indexedKeys: bigint;` |
| `pinnedBytes: bigint;` |
| `recoveredTransactions: bigint;` |
| `repairedBytes: bigint;` |
| `storedKeys: bigint;` |
| `storedValueBytes: bigint;` |
| `transactionBytes: bigint;` |
| `version: bigint;` |

</details>

<details>
<summary>fengqudb.WriteOptions</summary>

| Declaration |
| --- |
| `expiresAtMs?: Millis;` |
| `forceReliable?: boolean;` |

</details>

<details>
<summary>fengqudb.WritePolicy</summary>

| Declaration |
| --- |
| `mode: "performance" \| "reliable";` |
| `periodMs: number;` |
| `thresholdBytes: bigint;` |

</details>

<details>
<summary>fengqudb.WritePolicyOptions</summary>

| Declaration |
| --- |
| `mode?: "performance" \| "reliable";` |
| `periodMs?: number;` |
| `thresholdBytes?: number \| bigint;` |

</details>
