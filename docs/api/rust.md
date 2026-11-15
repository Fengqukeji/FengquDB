# Rust API 与调用例子

[接入文档](../languages/rust.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let student = Codec::decode(&bytes)?.value;
}
```

缺失是 None，空 Vec 是有效值。Codec 是接入文档中的 StudentFqCodec。

<details>
<summary>函数与参数（3项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>>` | 读取对应的数据或状态 |
| `Store` | `pub fn get_buffer(&self, key: &[u8]) -> Result<Option<Buffer>>` | 读取对应的数据或状态 |
| `Store` | `pub fn reader(&self, keys: &[&[u8]]) -> Result<Reader>` | 读取对应的数据或状态 |

</details>

## 2. 新增、保存、单个字符串和整数

```rust
db.put(b"name", "Alice".as_bytes())?;
db.put(b"score", &95_i64.to_le_bytes())?;
let score = db.get(b"score")?
    .map(|b| b.as_slice().try_into().map(i64::from_le_bytes)).transpose()?;
db.put(&Codec::key(&student)?, &Codec::encode(&student)?)?;
```

同键 put 覆盖旧值。整数例子需要使用可容纳长度转换错误的 Result。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn put(&self, key: &[u8], value: &[u8]) -> Result<u64>` | 保存或设置数据 |

</details>

## 3. 修改成绩

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let mut row = Codec::decode(&bytes)?;
    row.value.score = 96;
    db.put(&row.fq_key()?, &row.fq_encode()?)?;
}
```

记录的 fq_encode 会保留未知字段。

## 4. 删除

```rust
db.delete(b"student/7")?;
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn delete(&self, key: &[u8]) -> Result<()>` | 删除记录或成员 |

</details>

## 5. 事务和原子批写

```rust
let mut tx = db.begin(0)?;
tx.put(b"a", b"one")?;
tx.put(b"b", b"two")?;
tx.commit()?;
```

commit 消耗事务；未提交即 Drop 会丢弃。CONFLICT 时重新创建事务、读取和计算。put_many 批量暂存多条修改。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn begin(&self, timeout_ms: u64) -> Result<Transaction<'_>>` | 事务操作 |
| `Transaction` | `pub fn commit(self) -> Result<u64>` | 提交事务或查看提交结果 |
| `Transaction` | `pub fn delete(&mut self, key: &[u8]) -> Result<()>` | 删除记录或成员 |
| `Transaction` | `pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>>` | 读取对应的数据或状态 |
| `Transaction` | `pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()>` | 保存或设置数据 |
| `Transaction` | `pub fn put_many(&mut self, items: &[(&[u8], &[u8])]) -> Result<()>` | 保存或设置数据 |
| `Transaction` | `pub fn put_with_expiry(&mut self, key: &[u8], value: &[u8], expires_at_ms: u64) -> Result<()>` | 保存或设置数据 |

</details>

## 6. 筛选、排序、聚合

```rust
use fengqudb::builder;
let score = builder::sint64(Codec::FIELD_SCORE, 0);
let name = builder::text(Codec::FIELD_NAME, 0);
let rows = builder::from_(b"student/".to_vec()).where_(score.ge(90_i64))
    .select(&[&name, &score]).order_by([score.desc()]).limit(10).run(&db)?;
```

组合条件、group_by/having 和 join 用于更复杂筛选；prepare 复用查询，explain 查看执行计划。

<details>
<summary>函数与参数（107项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Agg` | `pub fn asc(&self) -> Sort` | 查询构建或结果读取 |
| `Agg` | `pub fn desc(&self) -> Sort` | 查询构建或结果读取 |
| `Agg` | `pub fn eq(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Agg` | `pub fn ge(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Agg` | `pub fn gt(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Agg` | `pub fn le(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Agg` | `pub fn lt(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Agg` | `pub fn ne(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Builder` | `pub fn after(mut self, cursor: impl Into<Vec<u8>>) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn allow_scan(mut self, on: bool) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn build(&self) -> Result<Query>` | 查询构建或结果读取 |
| `Builder` | `pub fn count_total(mut self, on: bool) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn distinct(mut self, on: bool) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult>` | 查看查询计划 |
| `Builder` | `pub fn group_by(mut self, cols: &[&Col]) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn having(mut self, cond: Cond) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn index(mut self, name: impl Into<Vec<u8>>) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn join(mut self, prefix: impl Into<Vec<u8>>, left: &Col, right: &Col, kind: JoinKind) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn limit(mut self, n: u64) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn memory(mut self, bytes: u64) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn offset(mut self, n: u64) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn order_by(mut self, items: impl IntoIterator<Item = Sort>) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn prepare(&self) -> Result<TypedPrepared>` | 查询构建或结果读取 |
| `Builder` | `pub fn run(&self, db: &impl SelectTarget) -> Result<TypedResult>` | 查询构建或结果读取 |
| `Builder` | `pub fn select(mut self, items: &[&dyn Selectable]) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn timeout_ms(mut self, ms: u64) -> Builder` | 查询构建或结果读取 |
| `Builder` | `pub fn where_(mut self, cond: Cond) -> Builder` | 查询构建或结果读取 |
| `Col` | `pub fn any(&self) -> Col` | 查询构建或结果读取 |
| `Col` | `pub fn asc(&self) -> Sort` | 查询构建或结果读取 |
| `Col` | `pub fn between(&self, low: impl Into<QueryValue>, high: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn contains(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn desc(&self) -> Sort` | 查询构建或结果读取 |
| `Col` | `pub fn ends_with(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn eq(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn eq_col(&self, other: &Col) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn exists(&self) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn fuzzy(&self, s: impl Into<QueryValue>, distance: u32) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn ge(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn ge_col(&self, other: &Col) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn gt(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn gt_col(&self, other: &Col) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn icontains(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn ilike(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn in_<V: Into<QueryValue>>(&self, values: impl IntoIterator<Item = V>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn is_null(&self) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn le(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn le_col(&self, other: &Col) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn like(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn lt(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn lt_col(&self, other: &Col) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn ne(&self, v: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn ne_col(&self, other: &Col) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn regex(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Col` | `pub fn starts_with(&self, s: impl Into<QueryValue>) -> Cond` | 查询构建或结果读取 |
| `Column` | `pub fn new(field: Field) -> Column` | 查询构建或结果读取 |
| `Cond` | `pub fn is_aggregate(&self) -> bool` | 查询构建或结果读取 |
| `PreparedQuery` | `pub fn encoded(&self) -> &[u8]` | 查询构建或结果读取 |
| `PreparedQuery` | `pub fn execute(&self, db: &impl SelectTarget) -> Result<QueryResult>` | 查询构建或结果读取 |
| `PreparedQuery` | `pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult>` | 查看查询计划 |
| `PreparedQuery` | `pub fn new(encoded: Vec<u8>) -> PreparedQuery` | 查询构建或结果读取 |
| `Query` | `pub fn encode(&self) -> Result<Vec<u8>>` | 查询构建或结果读取 |
| `Query` | `pub fn execute(&self, db: &impl SelectTarget) -> Result<QueryResult>` | 查询构建或结果读取 |
| `Query` | `pub fn execute_pages(&self, db: &impl SelectTarget, mut on_page: impl FnMut(&QueryResult) -> Result<()>) -> Result<usize>` | 查询构建或结果读取 |
| `Query` | `pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult>` | 查看查询计划 |
| `Query` | `pub fn new(prefix: impl Into<Vec<u8>>) -> Query` | 查询构建或结果读取 |
| `Query` | `pub fn prepare(&self) -> Result<PreparedQuery>` | 查询构建或结果读取 |
| `Query` | `pub fn request(&self, explain: bool) -> Result<Vec<u8>>` | 查询构建或结果读取 |
| `QueryResult` | `pub fn access(&self) -> AccessIter<'_>` | 查询构建或结果读取 |
| `QueryResult` | `pub fn as_bytes(&self) -> &[u8]` | 查询构建或结果读取 |
| `QueryResult` | `pub fn cursor(&self) -> Result<&[u8]>` | 查询构建或结果读取 |
| `QueryResult` | `pub fn rows(&self) -> ResultRows<'_>` | 查询构建或结果读取 |
| `QueryResult` | `pub fn stats(&self) -> Result<Stats>` | 查询构建或结果读取 |
| `Row` | `pub fn expires_at_ms(&self) -> u64` | 查询构建或结果读取 |
| `Row` | `pub fn found(&self) -> bool` | 查询构建或结果读取 |
| `Row` | `pub fn value(&self) -> &'a [u8]` | 查询构建或结果读取 |
| `Row` | `pub fn version(&self) -> u64` | 查询构建或结果读取 |
| `Rows` | `pub fn get(&self, index: usize) -> Option<Row<'a>>` | 读取对应的数据或状态 |
| `Rows` | `pub fn is_empty(&self) -> bool` | 查询构建或结果读取 |
| `Rows` | `pub fn iter(&self) -> impl Iterator<Item = Row<'a>> + 'a` | 查询构建或结果读取 |
| `Rows` | `pub fn len(&self) -> usize` | 查询构建或结果读取 |
| `Sort` | `pub fn nulls_first(mut self, first: bool) -> Sort` | 查询构建或结果读取 |
| `Sort` | `pub fn output(index: u32, descending: bool) -> Sort` | 查询构建或结果读取 |
| `TypedRow` | `pub fn get(&self, item: &dyn Selectable) -> Result<Value<'a>>` | 读取对应的数据或状态 |
| `TypedRow` | `pub fn key(&self) -> &'a [u8]` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn avg(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn boolean(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn count() -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn count_all_of(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn count_distinct(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn count_of(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn double(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn fixed32(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn fixed64(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn float32(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn from_(prefix: impl Into<Vec<u8>>) -> Builder` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn int64(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn key(source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn max(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn min(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn nested(path: &[u32], ty: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn sfixed32(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn sfixed64(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn sint64(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn sum(c: &Col) -> Agg` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn text(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::builder` | `pub fn uint64(n: u32, source: u32) -> Col` | 查询构建或结果读取 |
| `fengqudb::query` | `pub fn feature_request(operation: u64, scope: &[u8], key: &[u8], value: &[u8]) -> Vec<u8>` | 查询构建或结果读取 |

</details>

## 7. 索引

define_compound_index 建立字段/复合索引，drop_index 删除。索引字段类型要和实体编码一致。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn define_compound_index(&self, name: &str, key_prefix: &[u8], fields: &[IndexField], flags: u32) -> Result<()>` | 定义或使用索引 |
| `Store` | `pub fn drop_index(&self, name: &str) -> Result<()>` | 定义或使用索引 |

</details>

## 8. 等待可靠落盘

默认性能模式为 50 ms / 1 MiB，普通成功表示内存生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```rust
db.put(b"saved", b"yes")?;
db.sync()?;
```

当前安全客户端没有单笔 forceReliable 参数；sync 提供显式落盘屏障。整库可靠模式在写入前设置：

```rust
let mut options = db.write_options()?;
options.mode = fengqudb::WriteMode::Reliable;
db.configure_write(&options)?;
```

write_options 读取当前配置，reset_write_options 恢复默认性能模式。配置会保存，重开后保留。

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn configure_write(&self, options: &WriteOptions) -> Result<()>` | 控制可靠确认或提交等待 |
| `Store` | `pub fn reset_write_options(&self) -> Result<()>` | 控制可靠确认或提交等待 |
| `Store` | `pub fn sync(&self) -> Result<()>` | 控制可靠确认或提交等待 |
| `Store` | `pub fn write_options(&self) -> Result<WriteOptions>` | 控制可靠确认或提交等待 |

</details>

## 9. 到期数据

put_with_expiry 传绝对 Unix 毫秒；0 不过期。适合短期状态和临时缓存。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn put_with_expiry(&self, key: &[u8], value: &[u8], expires_at_ms: u64) -> Result<u64>` | 保存或设置数据 |

</details>

## 10. 热点共享读取

```rust
if let Some(view) = db.get_shared(b"student/7")? {
    consume(view.as_bytes());
}
```

租约 Drop 后借用失效；Reader 的 Rows 借用阻止在下一次 read 后继续使用旧行。需要长时间保存则复制。

<details>
<summary>函数与参数（11项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `PreparedKey` | `pub fn get_shared(&self) -> Result<Option<SharedValue>>` | 读取对应的数据或状态 |
| `Reader` | `pub fn key_count(&self) -> usize` | 复用键、缓冲或共享字节 |
| `Reader` | `pub fn read(&mut self) -> Result<Rows<'_>>` | 读取对应的数据或状态 |
| `Reader` | `pub fn release(&mut self)` | 释放对象或借用的资源 |
| `SharedValue` | `pub fn as_bytes(&self) -> &[u8]` | 复用键、缓冲或共享字节 |
| `SharedValue` | `pub fn expires_at_ms(&self) -> u64` | 复用键、缓冲或共享字节 |
| `SharedValue` | `pub fn is_empty(&self) -> bool` | 复用键、缓冲或共享字节 |
| `SharedValue` | `pub fn len(&self) -> usize` | 复用键、缓冲或共享字节 |
| `SharedValue` | `pub fn version(&self) -> u64` | 复用键、缓冲或共享字节 |
| `Store` | `pub fn get_shared(&self, key: &[u8]) -> Result<Option<SharedValue>>` | 读取对应的数据或状态 |
| `Store` | `pub fn prepare_key(&self, key: &[u8]) -> Result<PreparedKey>` | 复用键、缓冲或共享字节 |

</details>

## 11. 一致快照

snapshot 创建一致视图，Drop 自动释放。不要保留不再使用的快照。

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Snapshot` | `pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>>` | 读取对应的数据或状态 |
| `Snapshot` | `pub fn get_shared(&self, key: &[u8]) -> Result<Option<SharedValue>>` | 读取对应的数据或状态 |
| `Store` | `pub fn snapshot(&self) -> Result<Snapshot<'_>>` | 一致读取或范围遍历 |
| `TypedResult` | `pub fn cursor(&self) -> Result<&[u8]>` | 一致读取或范围遍历 |

</details>

## maintenance

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Store` | `pub fn info(&self) -> Result<StoreInfo>` | 查看状态或维护数据库 |

</details>

## 12. 实体编解码

Codec::encode_into 接受可复用 Vec，编码前 clear 可以保留容量。源码结构字段需要 pub 和 Default。生成发生在 cargo build 的 build script 中，不在数据库读写时运行。

当前安全客户端没有备份、集合、维护和订阅的高层方法。末尾 fengqudb_sys 是底层 unsafe 接口；普通业务优先使用本页前面的安全方法。

## 其他辅助接口

<details>
<summary>函数与参数（73项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `Aggregate` | `pub fn from_u32(v: u32) -> Result<Aggregate>` | 辅助操作 |
| `Buffer` | `pub fn as_bytes(&self) -> &[u8]` | 辅助操作 |
| `Buffer` | `pub fn is_empty(&self) -> bool` | 辅助操作 |
| `Buffer` | `pub fn len(&self) -> usize` | 辅助操作 |
| `Buffer` | `pub fn to_vec(&self) -> Vec<u8>` | 辅助操作 |
| `Error` | `pub fn code(&self) -> Option<i32>` | 辅助操作 |
| `Error` | `pub fn is_status(&self, code: i32) -> bool` | 辅助操作 |
| `Expr` | `pub fn compare_field(op: Op, field: Field, other: Field) -> Expr` | 辅助操作 |
| `Expr` | `pub fn logic(op: Op, children: Vec<Expr>) -> Expr` | 辅助操作 |
| `Expr` | `pub fn test(op: Op, field: Field, values: Vec<QueryValue>) -> Expr` | 辅助操作 |
| `Field` | `pub fn any(&self) -> Field` | 辅助操作 |
| `Field` | `pub fn column(index: u32) -> Field` | 辅助操作 |
| `Field` | `pub fn key(source: u32) -> Field` | 辅助操作 |
| `Field` | `pub fn new(number: u32, ty: u32, source: u32) -> Field` | 辅助操作 |
| `Field` | `pub fn record(source: u32) -> Field` | 辅助操作 |
| `Join` | `pub fn new(prefix: Vec<u8>, left: Vec<Field>, right: Vec<Field>, kind: JoinKind) -> Join` | 辅助操作 |
| `JoinKind` | `pub fn from_u32(v: u32) -> Result<JoinKind>` | 辅助操作 |
| `Op` | `pub fn from_u32(v: u32) -> Result<Op>` | 辅助操作 |
| `Order` | `pub fn new(field: Field) -> Order` | 辅助操作 |
| `ResultRow` | `pub fn key(&self) -> &'a [u8]` | 辅助操作 |
| `ResultRow` | `pub fn value(&self, index: usize) -> Result<Option<Value<'a>>>` | 辅助操作 |
| `ResultRow` | `pub fn values(&self) -> ResultValues<'a>` | 辅助操作 |
| `Store` | `pub fn close(&self)` | 释放对象或借用的资源 |
| `Store` | `pub fn is_closed(&self) -> bool` | 释放对象或借用的资源 |
| `Store` | `pub fn open(path: impl AsRef<Path>, options: &Options) -> Result<Store>` | 辅助操作 |
| `TypedPrepared` | `pub fn run(&self, db: &impl SelectTarget) -> Result<TypedResult>` | 辅助操作 |
| `TypedResult` | `pub fn result(&self) -> &QueryResult` | 辅助操作 |
| `TypedResult` | `pub fn rows(&self) -> impl Iterator<Item = Result<TypedRow<'_>>>` | 辅助操作 |
| `Value` | `pub fn as_bool(&self) -> Option<bool>` | 辅助操作 |
| `Value` | `pub fn as_bytes(&self) -> Option<&'a [u8]>` | 辅助操作 |
| `Value` | `pub fn as_f64(&self) -> Option<f64>` | 辅助操作 |
| `Value` | `pub fn as_i64(&self) -> Option<i64>` | 辅助操作 |
| `Value` | `pub fn as_u64(&self) -> Option<u64>` | 辅助操作 |
| `Value` | `pub fn is_null(&self) -> bool` | 辅助操作 |
| `fengqudb::error` | `pub fn status_message(code: i32) -> String` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_abi_version() -> u32;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_buffer_free(buffer: *mut fqdb_buffer);` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_status_message(status: fqdb_status) -> *const c_char;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_close(store: *mut fqdb_store);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_delete( store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_store_atomic_info, force_reliable: u32, ) -> fqdb_status;` | 删除记录或成员 |
| `fengqudb_sys` | `pub fn fqdb_store_get(store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_buffer) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_get_info(store: *mut fqdb_store, out: *mut fqdb_store_info) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_get_shared( store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_store_shared_value, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_index_define_compound( store: *mut fqdb_store, name: fqdb_slice, key_prefix: fqdb_slice, fields: *const fqdb_store_index_field, count: u32, flags: u32, options: *const fqdb_store_maintenance_options, out: *mut fqdb_store_maintenance_info, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_index_drop( store: *mut fqdb_store, name: fqdb_slice, options: *const fqdb_store_maintenance_options, out: *mut fqdb_store_maintenance_info, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_key_close(key: *mut fqdb_store_key);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_key_get_shared( key: *mut fqdb_store_key, out: *mut fqdb_store_shared_value, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_key_prepare( store: *mut fqdb_store, key: fqdb_slice, out: *mut *mut fqdb_store_key, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_open( directory: *const c_char, options: *const fqdb_store_options, out: *mut *mut fqdb_store, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_put( store: *mut fqdb_store, key: fqdb_slice, value: fqdb_slice, expires_at_ms: u64, out: *mut fqdb_store_atomic_info, force_reliable: u32, ) -> fqdb_status;` | 保存或设置数据 |
| `fengqudb_sys` | `pub fn fqdb_store_reader_close(reader: *mut fqdb_store_reader);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_reader_create( store: *mut fqdb_store, keys: *const fqdb_slice, count: u32, out: *mut *mut fqdb_store_reader, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_reader_read( reader: *mut fqdb_store_reader, rows: *mut *const fqdb_store_result_row, count: *mut u32, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_reader_release(reader: *mut fqdb_store_reader);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_select( store: *mut fqdb_store, request: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_close(snapshot: *mut fqdb_store_snapshot);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_create( store: *mut fqdb_store, out: *mut *mut fqdb_store_snapshot, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_get( snapshot: *const fqdb_store_snapshot, key: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_get_shared( snapshot: *const fqdb_store_snapshot, key: fqdb_slice, out: *mut fqdb_store_shared_value, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_sync(store: *mut fqdb_store) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_begin( store: *mut fqdb_store, timeout_ms: u64, out: *mut *mut fqdb_store_txn, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_close(txn: *mut fqdb_store_txn);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_commit( txn: *mut fqdb_store_txn, version: *mut u64, force_reliable: u32, ) -> fqdb_status;` | 提交事务或查看提交结果 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_delete(txn: *mut fqdb_store_txn, key: fqdb_slice) -> fqdb_status;` | 删除记录或成员 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_execute( txn: *mut fqdb_store_txn, request: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_get( txn: *mut fqdb_store_txn, key: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_put( txn: *mut fqdb_store_txn, key: fqdb_slice, value: fqdb_slice, expires_at_ms: u64, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_txn_put_many( txn: *mut fqdb_store_txn, items: *const fqdb_store_put_item, count: u32, ) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_value_close(value: *mut fqdb_store_value);` | 释放对象或借用的资源 |
| `fengqudb_sys` | `pub fn fqdb_store_write_configure(store: *mut fqdb_store, options: *const fqdb_store_write_options) -> fqdb_status;` | 辅助操作 |
| `fengqudb_sys` | `pub fn fqdb_store_write_options_get(store: *mut fqdb_store, out: *mut fqdb_store_write_options) -> fqdb_status;` | 读取对应的数据或状态 |
| `fengqudb_sys::fqdb_slice` | `pub fn from_bytes(bytes: &[u8]) -> fqdb_slice` | 辅助操作 |
| `fengqudb_sys::fqdb_store_shared_value` | `pub const fn zeroed() -> Self` | 辅助操作 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>Aggregate</summary>

| 声明 |
| --- |
| `Avg = 6,` |
| `Count = 2,` |
| `CountAll = 1,` |
| `Max = 5,` |
| `Min = 4,` |
| `None = 0,` |
| `Sum = 3,` |

</details>

<details>
<summary>Durability</summary>

| 声明 |
| --- |
| `Buffered = 1,` |
| `Sync = 0,` |

</details>

<details>
<summary>Error</summary>

| 声明 |
| --- |
| `Closed,` |
| `Decode(&'static str),` |
| `Invalid(&'static str),` |
| `Native { code: i32, op: &'static str },` |

</details>

<details>
<summary>FieldType</summary>

| 声明 |
| --- |
| `Bool = 11,` |
| `Bytes = 4,` |
| `Double = 3,` |
| `Fixed32 = 9,` |
| `Fixed64 = 7,` |
| `Float = 6,` |
| `Int64 = 2,` |
| `Sfixed32 = 10,` |
| `Sfixed64 = 8,` |
| `Sint64 = 5,` |
| `Uint64 = 1,` |

</details>

<details>
<summary>JoinKind</summary>

| 声明 |
| --- |
| `Anti = 3,` |
| `Inner = 0,` |
| `Left = 1,` |
| `Semi = 2,` |

</details>

<details>
<summary>Op</summary>

| 声明 |
| --- |
| `And = 2,` |
| `Contains = 14,` |
| `EndsWith = 16,` |
| `Eq = 5,` |
| `Exists = 12,` |
| `False = 1,` |
| `Fuzzy = 22,` |
| `Ge = 10,` |
| `Gt = 9,` |
| `IContains = 19,` |
| `ILike = 20,` |
| `IRegex = 21,` |
| `In = 11,` |
| `IsNull = 13,` |
| `Le = 8,` |
| `Like = 17,` |
| `Lt = 7,` |
| `Ne = 6,` |
| `Not = 4,` |
| `Or = 3,` |
| `Regex = 18,` |
| `StartsWith = 15,` |
| `True = 0,` |

</details>

<details>
<summary>QueryValue</summary>

| 声明 |
| --- |
| `Bool(bool),` |
| `Bytes(Vec<u8>),` |
| `Double(f64),` |
| `Int(i64),` |
| `Null,` |
| `Uint(u64),` |

</details>

<details>
<summary>Value</summary>

| 声明 |
| --- |
| `Bool(bool),` |
| `Bytes(&'a [u8]),` |
| `Double(f64),` |
| `Int(i64),` |
| `Null,` |
| `Uint(u64),` |

</details>

<details>
<summary>WriteMode</summary>

| 声明 |
| --- |
| `Performance = 1,` |
| `Reliable = 0,` |

</details>

<details>
<summary>fengqudb</summary>

| 声明 |
| --- |
| `pub after: Vec<u8>,` |
| `pub aggregate: Aggregate,` |
| `pub all_transaction_bytes: u64,` |
| `pub allow_scan: bool,` |
| `pub cache_bytes: u64,` |
| `pub candidates: u64,` |
| `pub children: Vec<Expr>,` |
| `pub columns: Vec<Column>,` |
| `pub complete: bool,` |
| `pub count_total: bool,` |
| `pub covering: bool,` |
| `pub create: bool,` |
| `pub delta_bytes: u64,` |
| `pub descending: bool,` |
| `pub distinct: bool,` |
| `pub durability: Durability,` |
| `pub durable_version: u64,` |
| `pub field: Field,` |
| `pub field: u32,` |
| `pub file_bytes: u64,` |
| `pub group: Vec<Field>,` |
| `pub having: Expr,` |
| `pub index: &'a str,` |
| `pub index_hint: Vec<u8>,` |
| `pub join_probes: u64,` |
| `pub joins: Vec<Join>,` |
| `pub kind: JoinKind,` |
| `pub left: Vec<Field>,` |
| `pub limit: u64,` |
| `pub matched: u64,` |
| `pub max_candidates: u64,` |
| `pub max_snapshots: u64,` |
| `pub max_work: u64,` |
| `pub memory_bytes: u64,` |
| `pub mode: WriteMode,` |
| `pub name: String,` |
| `pub needs_scan: bool,` |
| `pub nulls_first: bool,` |
| `pub number: u32,` |
| `pub offset: u64,` |
| `pub on: Expr,` |
| `pub op: Op,` |
| `pub order: Vec<Order>,` |
| `pub ordered: bool,` |
| `pub path: Vec<u32>,` |
| `pub payload_reads: u64,` |
| `pub peak_memory: u64,` |
| `pub period_ms: u32,` |
| `pub pinned_bytes: u64,` |
| `pub prefix: Vec<u8>,` |
| `pub reason: &'a str,` |
| `pub repeated: bool,` |
| `pub result_total: u64,` |
| `pub right: Option<Field>,` |
| `pub right: Vec<Field>,` |
| `pub sampled_candidates: u64,` |
| `pub segmented: bool,` |
| `pub source: u32,` |
| `pub source: u64,` |
| `pub stored_keys: u64,` |
| `pub stored_value_bytes: u64,` |
| `pub strategy: &'a str,` |
| `pub threshold_bytes: u64,` |
| `pub timeout_ms: u64,` |
| `pub transaction_bytes: u64,` |
| `pub ty: FieldType,` |
| `pub ty: u32,` |
| `pub values: Vec<QueryValue>,` |
| `pub version: u64,` |
| `pub where_: Expr,` |
| `pub work: u64,` |
| `pub writer_bytes: u64,` |

</details>

<details>
<summary>fengqudb::buffer</summary>

| 声明 |
| --- |
| `pub struct Buffer` |

</details>

<details>
<summary>fengqudb::builder</summary>

| 声明 |
| --- |
| `pub struct Agg` |
| `pub struct Builder` |
| `pub struct Col` |
| `pub struct Cond` |
| `pub trait Selectable` |
| `pub struct Sort` |
| `pub struct TypedPrepared` |
| `pub struct TypedResult` |
| `pub struct TypedRow<'a>` |

</details>

<details>
<summary>fengqudb::error</summary>

| 声明 |
| --- |
| `pub enum Error` |
| `pub type Result<T> = std::result::Result<T, Error>;` |

</details>

<details>
<summary>fengqudb::query</summary>

| 声明 |
| --- |
| `pub enum Aggregate` |
| `pub struct Column` |
| `pub struct Expr` |
| `pub struct Field` |
| `pub struct Join` |
| `pub enum JoinKind` |
| `pub enum Op` |
| `pub struct Order` |
| `pub struct PreparedQuery` |
| `pub struct Query` |
| `pub enum QueryValue` |
| `pub trait SelectTarget` |

</details>

<details>
<summary>fengqudb::result</summary>

| 声明 |
| --- |
| `pub struct Access<'a>` |
| `pub struct AccessIter<'a>` |
| `pub struct QueryResult` |
| `pub struct ResultRow<'a>` |
| `pub struct ResultRows<'a>` |
| `pub struct ResultValues<'a>` |
| `pub struct Stats` |
| `pub enum Value<'a>` |

</details>

<details>
<summary>fengqudb::shared</summary>

| 声明 |
| --- |
| `pub struct PreparedKey` |
| `pub struct Reader` |
| `pub struct Row<'a>` |
| `pub struct Rows<'a>` |
| `pub struct SharedValue` |

</details>

<details>
<summary>fengqudb::store</summary>

| 声明 |
| --- |
| `pub enum Durability` |
| `pub enum FieldType` |
| `constant INDEX_REQUIRED` |
| `constant INDEX_UNIQUE` |
| `pub struct IndexField` |
| `pub struct Options` |
| `pub struct Snapshot<'s>` |
| `pub struct Store` |
| `pub struct StoreInfo` |
| `pub struct Transaction<'s>` |
| `pub enum WriteMode` |
| `pub struct WriteOptions` |

</details>

<details>
<summary>fengqudb_sys</summary>

| 声明 |
| --- |
| `constant FQDB_ALREADY_EXISTS` |
| `constant FQDB_ASYNC_REQUIRED` |
| `constant FQDB_BUFFER_TOO_SMALL` |
| `constant FQDB_CANCELLED` |
| `constant FQDB_COMMIT_UNKNOWN` |
| `constant FQDB_CONFLICT` |
| `constant FQDB_CONSTRAINT` |
| `constant FQDB_CORRUPT` |
| `constant FQDB_DURABILITY_BUFFERED` |
| `constant FQDB_DURABILITY_SYNC` |
| `constant FQDB_END` |
| `constant FQDB_HISTORY_LOST` |
| `constant FQDB_INTERNAL_ERROR` |
| `constant FQDB_INVALID_ARGUMENT` |
| `constant FQDB_IO_ERROR` |
| `constant FQDB_LIMIT` |
| `constant FQDB_LOCKED` |
| `constant FQDB_NOT_FOUND` |
| `constant FQDB_NOT_MODIFIED` |
| `constant FQDB_NO_MEMORY` |
| `constant FQDB_OK` |
| `constant FQDB_RECOVERY_REQUIRED` |
| `constant FQDB_SELECT` |
| `constant FQDB_SELECT_EXPLAIN` |
| `constant FQDB_STORE_CREATE` |
| `constant FQDB_STORE_SEGMENTED` |
| `constant FQDB_TIMEOUT` |
| `constant FQDB_UNSUPPORTED` |
| `constant FQDB_WRITE_PERFORMANCE` |
| `constant FQDB_WRITE_RELIABLE` |
| `pub struct fqdb_buffer` |
| `pub struct fqdb_slice` |
| `pub type fqdb_status = i32;` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `pub struct fqdb_store_atomic_info` |
| `pub struct fqdb_store_index_field` |
| `pub struct fqdb_store_info` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `pub struct fqdb_store_maintenance_info` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `pub struct fqdb_store_options` |
| `pub struct fqdb_store_put_item` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `pub struct fqdb_store_result_row` |
| `pub struct fqdb_store_shared_value` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `opaque!( fqdb_store, fqdb_store_txn, fqdb_store_snapshot, fqdb_store_value, fqdb_store_key, fqdb_store_reader, fqdb_store_maintenance_options, );` |
| `pub struct fqdb_store_write_options` |

</details>

<details>
<summary>fengqudb_sys::fengqudb</summary>

| 声明 |
| --- |
| `pub abi_version: u32,` |
| `pub all_transaction_bytes: u64,` |
| `pub cache_bytes: u64,` |
| `pub catchup_rounds: u64,` |
| `pub changed: u32,` |
| `pub copied_keys: u64,` |
| `pub data: *const c_void,` |
| `pub data: *mut c_void,` |
| `pub delta_bytes: u64,` |
| `pub durability: u32,` |
| `pub durable_version: u64,` |
| `pub expires_at_ms: u64,` |
| `pub field: u32,` |
| `pub file_bytes: u64,` |
| `pub final_version: u64,` |
| `pub flags: u32,` |
| `pub found: u32,` |
| `pub indexed_keys: u64,` |
| `pub key: fqdb_slice,` |
| `pub matched: u32,` |
| `pub max_snapshots: u64,` |
| `pub mode: u32,` |
| `pub observed_version: u64,` |
| `pub output_file_bytes: u64,` |
| `pub owner: *mut fqdb_store_value,` |
| `pub period_ms: u32,` |
| `pub pinned_bytes: u64,` |
| `pub recovered_transactions: u64,` |
| `pub repaired_bytes: u64,` |
| `pub reserved: u32,` |
| `pub row: fqdb_store_result_row,` |
| `pub size: u64,` |
| `pub snapshot_version: u64,` |
| `pub source_file_bytes: u64,` |
| `pub stored_keys: u64,` |
| `pub stored_value_bytes: u64,` |
| `pub struct_size: u32,` |
| `pub threshold_bytes: u64,` |
| `pub transaction_bytes: u64,` |
| `pub type_: u32,` |
| `pub value: fqdb_slice,` |
| `pub value: i64,` |
| `pub version: u64,` |
| `pub writer_bytes: u64,` |

</details>

<details>
<summary>fengqudb_sys::fqdb_buffer</summary>

| 声明 |
| --- |
| `assoc_const EMPTY` |

</details>

<details>
<summary>fengqudb_sys::fqdb_slice</summary>

| 声明 |
| --- |
| `assoc_const EMPTY` |

</details>
