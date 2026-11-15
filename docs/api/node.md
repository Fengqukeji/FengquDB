# Node.js / TypeScript API 与调用例子

[接入文档](../languages/node.md) · [English](../en/api/node.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取

```js
const data = db.get('student/7');
const student = data === null ? null : Codec.decode(data).value;
```

缺失返回 null，空 Uint8Array 是有效数据。Codec 是接入文档生成的 StudentFqCodec。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.ResultRow` | `get(index: number, copy = false): ResultValue` | 读取对应的数据或状态 |
| `fengqudb.Store` | `get(key: BytesLike): Uint8Array \| null` | 读取对应的数据或状态 |

</details>

## 2. 新增、保存、单个字符串和整数

```js
db.put(Codec.key(student), Codec.encode(student));
db.put('name', '张三');
const number = Buffer.alloc(8); number.writeBigInt64LE(95n);
db.put('score', number);
const score = Buffer.from(db.get('score')).readBigInt64LE();
```

同键 put 覆盖原值。64位整数使用 bigint。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `put(key: BytesLike, value: BytesLike, options?: WriteOptions): bigint` | 保存或设置数据 |

</details>

## 3. 修改

```js
const bytes = db.get('student/7');
if (bytes !== null) {
  const row = Codec.decode(bytes);
  row.value.score = 96;
  db.put(row.fqKey(), row.fqEncode());
}
```

多键修改放在事务中；当前 Node 包没有原子计数和二进制 patch 的便捷方法。

## 4. 删除

```js
const removed = db.delete('student/7');
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `delete(key: BytesLike, options?: { forceReliable?: boolean }): boolean` | 删除记录或成员 |

</details>

## 5. 批量修改

```js
const tx = db.transaction();
try { tx.putMany([['a','one'],['b','two']]); tx.commit(); }
finally { tx.close(); }
```

整批修改作为同一笔事务提交。

## 6. 事务与局部回退

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

commit 会关闭事务；未提交即 close 会丢弃。CONFLICT 时重新创建事务并重新读取计算。

<details>
<summary>函数与参数（15项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `transaction(timeoutMs?: Millis): Transaction` | 事务操作 |
| `fengqudb.Transaction` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.Transaction` | `get closed(): boolean` | 释放对象或借用的资源 |
| `fengqudb.Transaction` | `commit(forceReliable = false): bigint` | 提交事务或查看提交结果 |
| `fengqudb.Transaction` | `constructor(h: Handle)` | 事务操作 |
| `fengqudb.Transaction` | `delete(key: BytesLike): void` | 删除记录或成员 |
| `fengqudb.Transaction` | `deleteMany(keys: BytesLike[]): void` | 删除记录或成员 |
| `fengqudb.Transaction` | `get(key: BytesLike): Uint8Array \| null` | 读取对应的数据或状态 |
| `fengqudb.Transaction` | `put(key: BytesLike, value: BytesLike, expiresAtMs?: Millis): void` | 保存或设置数据 |
| `fengqudb.Transaction` | `putMany(items: ReadonlyArray<readonly [BytesLike, BytesLike, Millis?]>): void` | 保存或设置数据 |
| `fengqudb.Transaction` | `releaseSavepoint(id: bigint): void` | 释放对象或借用的资源 |
| `fengqudb.Transaction` | `rollbackTo(id: bigint): void` | 回退到保存点 |
| `fengqudb.Transaction` | `savepoint(): bigint` | 创建或操作事务保存点 |
| `fengqudb.Transaction` | `select(q: Query \| PreparedQuery): QueryResult` | 事务操作 |
| `fengqudb.Transaction` | `selectRaw(request: Uint8Array): Uint8Array` | 事务操作 |

</details>

## 7. 条件筛选、排序、统计

```js
const {from, s64, text} = require('./FengquDB/node');
const score = s64(Codec.fields.score), name = text(Codec.fields.name);
const rows = from('student/').where(score.ge(90n))
  .select(name,score).orderBy(score.desc()).limit(10).run(db);
try { for(const row of rows) console.log(row.get(name),row.get(score)); }
finally { rows.close(); }
```

and/or/not 组合条件，groupBy/having 分组统计，join 做等值关联；prepare 复用查询，explain 查看是否走索引。查询列类型应匹配编码，普通 number 字段默认用 f64。

<details>
<summary>函数与参数（111项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `export function and(...cs: Cond[]): Cond` | 查询构建或结果读取 |
| `fengqudb` | `export function from(prefix: Uint8Array \| string): QueryBuilder` | 查询构建或结果读取 |
| `fengqudb` | `export function not(c: Cond): Cond` | 查询构建或结果读取 |
| `fengqudb` | `export function or(...cs: Cond[]): Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `asc(nullsFirst = true): Sort` | 查询构建或结果读取 |
| `fengqudb.Agg` | `column(): Column` | 查询构建或结果读取 |
| `fengqudb.Agg` | `constructor(readonly col: Column)` | 查询构建或结果读取 |
| `fengqudb.Agg` | `desc(nullsFirst = true): Sort` | 查询构建或结果读取 |
| `fengqudb.Agg` | `eq(v: IntLike \| number): Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `ge(v: IntLike \| number): Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `gt(v: IntLike \| number): Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `le(v: IntLike \| number): Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `lt(v: IntLike \| number): Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `ne(v: IntLike \| number): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `any(): Col<V>` | 查询构建或结果读取 |
| `fengqudb.Col` | `asc(nullsFirst = true): Sort` | 查询构建或结果读取 |
| `fengqudb.Col` | `between(low: V, high: V): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `column(): Column` | 查询构建或结果读取 |
| `fengqudb.Col` | `constructor(readonly field: Field, private readonly cast: Cast = undefined)` | 查询构建或结果读取 |
| `fengqudb.Col` | `contains(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `desc(nullsFirst = true): Sort` | 查询构建或结果读取 |
| `fengqudb.Col` | `endsWith(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `eq(v: V \| Col<unknown>): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `exists(): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `fuzzy(s: string \| Uint8Array, distance: number): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `ge(v: V \| Col<unknown>): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `gt(v: V \| Col<unknown>): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `icontains(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `ilike(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `in(values: readonly V[]): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `in_(values: readonly V[]): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `isNull(): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `le(v: V \| Col<unknown>): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `like(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `lt(v: V \| Col<unknown>): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `ne(v: V \| Col<unknown>): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `regex(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `startsWith(s: string \| Uint8Array): Cond` | 查询构建或结果读取 |
| `fengqudb.Column` | `constructor( readonly field: Field = new Field(), readonly aggregate: Aggregate = Aggregate.NONE, readonly name = "", readonly distinct = false, )` | 查询构建或结果读取 |
| `fengqudb.Column` | `encode(): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.Cond` | `and(o: Cond): Cond` | 查询构建或结果读取 |
| `fengqudb.Cond` | `build(columns: readonly Column[]): Expr` | 查询构建或结果读取 |
| `fengqudb.Cond` | `constructor(private readonly resolve: (columns: readonly Column[]) => Expr, readonly aggregate = false)` | 查询构建或结果读取 |
| `fengqudb.Cond` | `not(): Cond` | 查询构建或结果读取 |
| `fengqudb.Cond` | `or(o: Cond): Cond` | 查询构建或结果读取 |
| `fengqudb.Expr` | `and(o: Expr): Expr` | 查询构建或结果读取 |
| `fengqudb.Expr` | `not(): Expr` | 查询构建或结果读取 |
| `fengqudb.Expr` | `or(o: Expr): Expr` | 查询构建或结果读取 |
| `fengqudb.PreparedBuilder` | `constructor(readonly prepared: PreparedQuery, private readonly columns: readonly Column[])` | 查询构建或结果读取 |
| `fengqudb.PreparedBuilder` | `run(db: Selectable): Rows` | 查询构建或结果读取 |
| `fengqudb.PreparedQuery` | `constructor(encoded: Uint8Array)` | 查询构建或结果读取 |
| `fengqudb.PreparedQuery` | `execute(db: Selectable): QueryResult` | 查询构建或结果读取 |
| `fengqudb.PreparedQuery` | `explain(db: Selectable): QueryResult` | 查看查询计划 |
| `fengqudb.PreparedQuery` | `request(explain = false): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.Query` | `constructor(prefix: Uint8Array \| string, o: QueryOptions = {})` | 查询构建或结果读取 |
| `fengqudb.Query` | `encode(): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.Query` | `execute(db: Selectable): QueryResult` | 查询构建或结果读取 |
| `fengqudb.Query` | `explain(db: Selectable): QueryResult` | 查看查询计划 |
| `fengqudb.Query` | `*pages(db: Selectable): Generator<QueryResult, void, undefined>` | 查询构建或结果读取 |
| `fengqudb.Query` | `prepare(): PreparedQuery` | 查询构建或结果读取 |
| `fengqudb.Query` | `request(explain = false): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.Query` | `withAfter(cursor: Uint8Array): Query` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `after(cursor: Uint8Array): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `allowScan(on: boolean): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `build(): Query` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `constructor(prefix: Uint8Array \| string)` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `countTotal(on = true): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `distinct(on = true): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `explain(db: Selectable): QueryResult` | 查看查询计划 |
| `fengqudb.QueryBuilder` | `groupBy(...cols: Col<any>[]): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `having(c: Cond): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `index(name: string \| Uint8Array): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `join(prefix: Uint8Array \| string, left: Col<any>, right: Col<any>, kind: JoinKind = JoinKind.INNER, on?: Cond): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `limit(n: IntLike): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `maxCandidates(n: IntLike): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `maxWork(n: IntLike): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `memory(n: IntLike): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `offset(n: IntLike): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `orderBy(...items: Array<Sort \| Col<any> \| Agg>): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `*pages(db: Selectable): Generator<Rows, void, undefined>` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `prepare(): PreparedBuilder` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `run(db: Selectable): Rows` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `select(...items: Selectation[]): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `timeoutMs(n: IntLike): this` | 查询构建或结果读取 |
| `fengqudb.QueryBuilder` | `where(c: Cond): this` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `*[Symbol.iterator](): Iterator<ResultRow>` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `get access(): AccessPlan[]` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `get byteLength(): number` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.QueryResult` | `constructor(response: Uint8Array)` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `get cursor(): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `data(): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `get length(): number` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `row(i: number): ResultRow` | 查询构建或结果读取 |
| `fengqudb.QueryResult` | `get stats(): QueryStats` | 查询构建或结果读取 |
| `fengqudb.Rows` | `*[Symbol.iterator](): Iterator<TypedRow>` | 查询构建或结果读取 |
| `fengqudb.Rows` | `get access(): AccessPlan[]` | 查询构建或结果读取 |
| `fengqudb.Rows` | `at(i: number): TypedRow` | 查询构建或结果读取 |
| `fengqudb.Rows` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.Rows` | `constructor(private readonly columns: readonly Column[], readonly result: QueryResult)` | 查询构建或结果读取 |
| `fengqudb.Rows` | `get cursor(): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.Rows` | `get length(): number` | 查询构建或结果读取 |
| `fengqudb.Rows` | `get stats(): QueryStats` | 查询构建或结果读取 |
| `fengqudb.Selectable` | `selectRaw(request: Uint8Array): Uint8Array;` | 查询构建或结果读取 |
| `fengqudb.Sort` | `constructor(readonly build: (columns: readonly Column[]) => Order)` | 查询构建或结果读取 |
| `fengqudb.Store` | `explain(q: Query \| PreparedQuery): QueryResult` | 查看查询计划 |
| `fengqudb.Store` | `select(q: Query \| PreparedQuery): QueryResult` | 查询构建或结果读取 |
| `fengqudb.Store` | `selectRaw(request: Uint8Array): Uint8Array` | 查询构建或结果读取 |
| `fengqudb.TypedRow` | `constructor(private readonly columns: readonly Column[], readonly raw: ResultRow)` | 查询构建或结果读取 |
| `fengqudb.TypedRow` | `get(item: Selectation, copy = false): ResultValue` | 读取对应的数据或状态 |
| `fengqudb.TypedRow` | `get key(): Uint8Array` | 查询构建或结果读取 |

</details>

## 8. 索引

```js
db.defineCompoundIndex('student-score', 'student/', [[Codec.fields.score,5]], 0);
```

5 表示有符号 ZigZag 整数。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `defineCompoundIndex(name: BytesLike, keyPrefix: BytesLike, fields: readonly IndexField[], flags = 0): MaintenanceInfo` | 定义或使用索引 |
| `fengqudb.Store` | `dropIndex(name: BytesLike): MaintenanceInfo` | 定义或使用索引 |

</details>

## 9. 关键写入：等待落盘

默认性能模式为 50 ms / 1 MiB，普通成功表示内存已生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```js
db.put('payment/receipt', 'paid', {forceReliable:true});
db.sync();
```

事务使用 commit(true)。整库切换用 `db.configureWrite({mode:'reliable'})`，性能模式用 `db.configureWrite({mode:'performance', periodMs:50, thresholdBytes:1024*1024})`；省略的参数保留当前值。`db.writeOptions()` 读取配置，`db.resetWriteOptions()` 恢复默认性能模式。配置会保存，重开后保留。

接口是同步的，可靠写与大查询应放到 worker，避免阻塞事件循环。COMMIT_UNKNOWN 时先按业务编号核对，不要重复扣款。

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `configureWrite(options: WritePolicyOptions): void` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `resetWriteOptions(): void` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `sync(): void` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `writeOptions(): WritePolicy` | 控制可靠确认或提交等待 |

</details>

## 10. 到期数据

```js
db.put('verification', '1234', {expiresAtMs:Date.now()+300_000});
```

到期时间为绝对 Unix 毫秒，0 表示不过期。

## 11. 热点读取与共享字节

```js
db.withShared('student/7', bytes => { if(bytes) consume(bytes); });
const reader = db.prepareReader(['student/7','student/8']);
try { reader.read(); if(reader.found(0)) consume(reader.value(0)); }
finally { reader.close(); }
```

共享字节只读；回调结束或释放后不能继续用。读取器视图只保留到下一次 read/release/close，需要跨次保存就复制。

<details>
<summary>函数与参数（35项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `export function releaseShared(view: Uint8Array): boolean` | 释放对象或借用的资源 |
| `fengqudb.PreparedKey` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.PreparedKey` | `get closed(): boolean` | 释放对象或借用的资源 |
| `fengqudb.PreparedKey` | `constructor(h: Handle)` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedKey` | `get(): Uint8Array \| null` | 读取对应的数据或状态 |
| `fengqudb.PreparedKey` | `getInto(target: Uint8Array): number` | 读取对应的数据或状态 |
| `fengqudb.PreparedKey` | `getShared(): SharedValue \| null` | 读取对应的数据或状态 |
| `fengqudb.PreparedKey` | `getSharedBytes(): Uint8Array \| null` | 读取对应的数据或状态 |
| `fengqudb.PreparedKey` | `peekByte(index: number): number` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedKey` | `withShared<T>(fn: (bytes: Uint8Array \| null) => T): T` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.PreparedReader` | `get closed(): boolean` | 释放对象或借用的资源 |
| `fengqudb.PreparedReader` | `constructor(store: Handle, keys: BytesLike[])` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `copyValue(i: number, target: Uint8Array): number` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `expiresAtMs(i: number): bigint` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `forEach(fn: (index: number, value: Uint8Array \| null) => void): void` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `found(i: number): boolean` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `length(i: number): number` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `read(): number` | 读取对应的数据或状态 |
| `fengqudb.PreparedReader` | `release(): void` | 释放对象或借用的资源 |
| `fengqudb.PreparedReader` | `value(i: number): Uint8Array \| null` | 复用键、缓冲或共享字节 |
| `fengqudb.PreparedReader` | `version(i: number): bigint` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `get bytes(): Uint8Array` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `constructor(view: Uint8Array)` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `copy(): Uint8Array` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `get expiresAtMs(): bigint` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `get length(): number` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `release(): void` | 释放对象或借用的资源 |
| `fengqudb.SharedValue` | `get released(): boolean` | 释放对象或借用的资源 |
| `fengqudb.SharedValue` | `get version(): bigint` | 复用键、缓冲或共享字节 |
| `fengqudb.Store` | `getShared(key: BytesLike): SharedValue \| null` | 读取对应的数据或状态 |
| `fengqudb.Store` | `getSharedBytes(key: BytesLike): Uint8Array \| null` | 读取对应的数据或状态 |
| `fengqudb.Store` | `prepareKey(key: BytesLike): PreparedKey` | 复用键、缓冲或共享字节 |
| `fengqudb.Store` | `prepareReader(keys: BytesLike[]): PreparedReader` | 复用键、缓冲或共享字节 |
| `fengqudb.Store` | `withShared<T>(key: BytesLike, fn: (bytes: Uint8Array \| null) => T): T` | 复用键、缓冲或共享字节 |

</details>

## 12. 一致快照

```js
const snapshot = db.snapshot();
try { const value = snapshot.get('student/7'); }
finally { snapshot.close(); }
```

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Snapshot` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.Snapshot` | `constructor(h: Handle)` | 一致读取或范围遍历 |
| `fengqudb.Snapshot` | `get(key: BytesLike): Uint8Array \| null` | 读取对应的数据或状态 |
| `fengqudb.Snapshot` | `getShared(key: BytesLike): SharedValue \| null` | 读取对应的数据或状态 |
| `fengqudb.Store` | `snapshot(): Snapshot` | 一致读取或范围遍历 |

</details>

## maintenance

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `info(): StoreInfo` | 查看状态或维护数据库 |

</details>

## 13. 实体编解码

Codec.encode/decode 操作原 Student。读改写时用记录的 fqEncode 保留未知字段；encode(value, destination) 可复用 Uint8Array，返回实际编码区间。自动生成由 npm prebuild 执行，不需要额外安装生成器。

## 其他辅助接口

<details>
<summary>函数与参数（30项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `export function buildRequest(operation: number, value: Uint8Array): Uint8Array` | 辅助操作 |
| `fengqudb` | `export function bytesToHex(b: Uint8Array): string` | 辅助操作 |
| `fengqudb` | `export function hexToBytes(hex: string): Uint8Array` | 辅助操作 |
| `fengqudb.Double` | `constructor(readonly value: number)` | 辅助操作 |
| `fengqudb.Expr` | `constructor( readonly op: Op = Op.TRUE, readonly field: Field = new Field(), readonly values: readonly QueryValue[] = [], readonly children: readonly Expr[] = [], readonly right: Field \| null = null, )` | 辅助操作 |
| `fengqudb.Expr` | `encode(depth = 0): Uint8Array` | 辅助操作 |
| `fengqudb.FengquDBError` | `constructor(status: number, message?: string)` | 辅助操作 |
| `fengqudb.FengquDBError` | `get statusName(): string` | 辅助操作 |
| `fengqudb.Field` | `any(): Field` | 辅助操作 |
| `fengqudb.Field` | `static column(index: number): Field` | 辅助操作 |
| `fengqudb.Field` | `constructor( readonly number = 0, readonly type = 4, readonly source = 0, readonly path: readonly number[] = [], /** repeated 字段：任一元素满足即为真，只能用于主源 WHERE 叶子条件。 */ readonly repeated = false, )` | 辅助操作 |
| `fengqudb.Field` | `encode(): Uint8Array` | 辅助操作 |
| `fengqudb.Field` | `equals(o: Field): boolean` | 辅助操作 |
| `fengqudb.Field` | `static key(source = 0): Field` | 辅助操作 |
| `fengqudb.Field` | `static record(source = 0): Field` | 辅助操作 |
| `fengqudb.Join` | `constructor( readonly prefix: Uint8Array \| string, readonly left: readonly Field[], readonly right: readonly Field[], readonly kind: JoinKind = JoinKind.INNER, readonly on: Expr = new Expr(), )` | 辅助操作 |
| `fengqudb.Join` | `encode(): Uint8Array` | 辅助操作 |
| `fengqudb.Order` | `constructor(readonly field: Field, readonly descending = false, readonly nullsFirst = true)` | 辅助操作 |
| `fengqudb.Order` | `encode(): Uint8Array` | 辅助操作 |
| `fengqudb.ResultRow` | `constructor(private readonly r: QueryResult, private readonly start: number, private readonly end: number)` | 辅助操作 |
| `fengqudb.ResultRow` | `get key(): Uint8Array` | 辅助操作 |
| `fengqudb.ResultRow` | `keyCopy(): Uint8Array` | 辅助操作 |
| `fengqudb.ResultRow` | `get length(): number` | 辅助操作 |
| `fengqudb.ResultRow` | `values(copy = false): ResultValue[]` | 辅助操作 |
| `fengqudb.Store` | `static attach(token: StoreToken): Store` | 辅助操作 |
| `fengqudb.Store` | `close(): void` | 释放对象或借用的资源 |
| `fengqudb.Store` | `get closed(): boolean` | 释放对象或借用的资源 |
| `fengqudb.Store` | `static discard(token: StoreToken): void` | 辅助操作 |
| `fengqudb.Store` | `static open(path: string, options?: OpenOptions): Store` | 辅助操作 |
| `fengqudb.Store` | `share(): StoreToken` | 辅助操作 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>fengqudb</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `readonly status: number;` |

</details>

<details>
<summary>fengqudb.JoinKind</summary>

| 声明 |
| --- |
| `ANTI = 3` |
| `INNER = 0` |
| `LEFT = 1` |
| `SEMI = 2` |

</details>

<details>
<summary>fengqudb.MaintenanceInfo</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `readonly encoded: Uint8Array;` |

</details>

<details>
<summary>fengqudb.PreparedReader</summary>

| 声明 |
| --- |
| `readonly capacity: number;` |

</details>

<details>
<summary>fengqudb.Query</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `expiresAtMs?: Millis;` |
| `forceReliable?: boolean;` |

</details>

<details>
<summary>fengqudb.WritePolicy</summary>

| 声明 |
| --- |
| `mode: "performance" \| "reliable";` |
| `periodMs: number;` |
| `thresholdBytes: bigint;` |

</details>

<details>
<summary>fengqudb.WritePolicyOptions</summary>

| 声明 |
| --- |
| `mode?: "performance" \| "reliable";` |
| `periodMs?: number;` |
| `thresholdBytes?: number \| bigint;` |

</details>
