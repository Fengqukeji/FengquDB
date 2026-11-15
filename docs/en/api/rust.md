# Rust API and examples

[Getting started](../languages/rust.md) · [简体中文](../../api/rust.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let student = Codec::decode(&bytes)?.value;
}
```

A missing record is None; an empty Vec is valid data. Codec is StudentFqCodec from the getting-started guide.

<details>
<summary>Functions and parameters (3)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>>` | Read data or status |
| `Store` | `pub fn get_buffer(&self, key: &[u8]) -> Result<Option<Buffer>>` | Read data or status |
| `Store` | `pub fn reader(&self, keys: &[&[u8]]) -> Result<Reader>` | Read data or status |

</details>

## 2. Insert, save, and store strings or integers

```rust
db.put(b"name", "Alice".as_bytes())?;
db.put(b"score", &95_i64.to_le_bytes())?;
let score = db.get(b"score")?
    .map(|b| b.as_slice().try_into().map(i64::from_le_bytes)).transpose()?;
db.put(&Codec::key(&student)?, &Codec::encode(&student)?)?;
```

put overwrites the previous value at the same key. The integer example needs a Result that can represent length-conversion errors.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn put(&self, key: &[u8], value: &[u8]) -> Result<u64>` | Save or set data |

</details>

## 3. Update a score

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let mut row = Codec::decode(&bytes)?;
    row.value.score = 96;
    db.put(&row.fq_key()?, &row.fq_encode()?)?;
}
```

The record's fq_encode preserves unknown fields.

## 4. Delete

```rust
db.delete(b"student/7")?;
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn delete(&self, key: &[u8]) -> Result<()>` | Delete a record or member |

</details>

## 5. Transactions and atomic batch writes

```rust
let mut tx = db.begin(0)?;
tx.put(b"a", b"one")?;
tx.put(b"b", b"two")?;
tx.commit()?;
```

commit consumes the transaction; Drop without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute. put_many stages multiple changes in a batch.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn begin(&self, timeout_ms: u64) -> Result<Transaction<'_>>` | Transaction operation |
| `Transaction` | `pub fn commit(self) -> Result<u64>` | Commit a transaction or inspect its result |
| `Transaction` | `pub fn delete(&mut self, key: &[u8]) -> Result<()>` | Delete a record or member |
| `Transaction` | `pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>>` | Read data or status |
| `Transaction` | `pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()>` | Save or set data |
| `Transaction` | `pub fn put_many(&mut self, items: &[(&[u8], &[u8])]) -> Result<()>` | Save or set data |
| `Transaction` | `pub fn put_with_expiry(&mut self, key: &[u8], value: &[u8], expires_at_ms: u64) -> Result<()>` | Save or set data |

</details>

## 6. Filtering, sorting, and aggregation

```rust
use fengqudb::builder;
let score = builder::sint64(Codec::FIELD_SCORE, 0);
let name = builder::text(Codec::FIELD_NAME, 0);
let rows = builder::from_(b"student/".to_vec()).where_(score.ge(90_i64))
    .select(&[&name, &score]).order_by([score.desc()]).limit(10).run(&db)?;
```

Combine conditions, group_by/having, and join for more complex queries. prepare reuses a query, and explain shows the execution plan.

<details>
<summary>Functions and parameters (107)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Agg` | `pub fn asc(&self) -> Sort` | Build a query or read its results |
| `Agg` | `pub fn desc(&self) -> Sort` | Build a query or read its results |
| `Agg` | `pub fn eq(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Agg` | `pub fn ge(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Agg` | `pub fn gt(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Agg` | `pub fn le(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Agg` | `pub fn lt(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Agg` | `pub fn ne(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Builder` | `pub fn after(mut self, cursor: impl Into<Vec<u8>>) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn allow_scan(mut self, on: bool) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn build(&self) -> Result<Query>` | Build a query or read its results |
| `Builder` | `pub fn count_total(mut self, on: bool) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn distinct(mut self, on: bool) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult>` | Inspect the query plan |
| `Builder` | `pub fn group_by(mut self, cols: &[&Col]) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn having(mut self, cond: Cond) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn index(mut self, name: impl Into<Vec<u8>>) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn join(mut self, prefix: impl Into<Vec<u8>>, left: &Col, right: &Col, kind: JoinKind) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn limit(mut self, n: u64) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn memory(mut self, bytes: u64) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn offset(mut self, n: u64) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn order_by(mut self, items: impl IntoIterator<Item = Sort>) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn prepare(&self) -> Result<TypedPrepared>` | Build a query or read its results |
| `Builder` | `pub fn run(&self, db: &impl SelectTarget) -> Result<TypedResult>` | Build a query or read its results |
| `Builder` | `pub fn select(mut self, items: &[&dyn Selectable]) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn timeout_ms(mut self, ms: u64) -> Builder` | Build a query or read its results |
| `Builder` | `pub fn where_(mut self, cond: Cond) -> Builder` | Build a query or read its results |
| `Col` | `pub fn any(&self) -> Col` | Build a query or read its results |
| `Col` | `pub fn asc(&self) -> Sort` | Build a query or read its results |
| `Col` | `pub fn between(&self, low: impl Into<QueryValue>, high: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn contains(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn desc(&self) -> Sort` | Build a query or read its results |
| `Col` | `pub fn ends_with(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn eq(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn eq_col(&self, other: &Col) -> Cond` | Build a query or read its results |
| `Col` | `pub fn exists(&self) -> Cond` | Build a query or read its results |
| `Col` | `pub fn fuzzy(&self, s: impl Into<QueryValue>, distance: u32) -> Cond` | Build a query or read its results |
| `Col` | `pub fn ge(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn ge_col(&self, other: &Col) -> Cond` | Build a query or read its results |
| `Col` | `pub fn gt(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn gt_col(&self, other: &Col) -> Cond` | Build a query or read its results |
| `Col` | `pub fn icontains(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn ilike(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn in_<V: Into<QueryValue>>(&self, values: impl IntoIterator<Item = V>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn is_null(&self) -> Cond` | Build a query or read its results |
| `Col` | `pub fn le(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn le_col(&self, other: &Col) -> Cond` | Build a query or read its results |
| `Col` | `pub fn like(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn lt(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn lt_col(&self, other: &Col) -> Cond` | Build a query or read its results |
| `Col` | `pub fn ne(&self, v: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn ne_col(&self, other: &Col) -> Cond` | Build a query or read its results |
| `Col` | `pub fn regex(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Col` | `pub fn starts_with(&self, s: impl Into<QueryValue>) -> Cond` | Build a query or read its results |
| `Column` | `pub fn new(field: Field) -> Column` | Build a query or read its results |
| `Cond` | `pub fn is_aggregate(&self) -> bool` | Build a query or read its results |
| `PreparedQuery` | `pub fn encoded(&self) -> &[u8]` | Build a query or read its results |
| `PreparedQuery` | `pub fn execute(&self, db: &impl SelectTarget) -> Result<QueryResult>` | Build a query or read its results |
| `PreparedQuery` | `pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult>` | Inspect the query plan |
| `PreparedQuery` | `pub fn new(encoded: Vec<u8>) -> PreparedQuery` | Build a query or read its results |
| `Query` | `pub fn encode(&self) -> Result<Vec<u8>>` | Build a query or read its results |
| `Query` | `pub fn execute(&self, db: &impl SelectTarget) -> Result<QueryResult>` | Build a query or read its results |
| `Query` | `pub fn execute_pages(&self, db: &impl SelectTarget, mut on_page: impl FnMut(&QueryResult) -> Result<()>) -> Result<usize>` | Build a query or read its results |
| `Query` | `pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult>` | Inspect the query plan |
| `Query` | `pub fn new(prefix: impl Into<Vec<u8>>) -> Query` | Build a query or read its results |
| `Query` | `pub fn prepare(&self) -> Result<PreparedQuery>` | Build a query or read its results |
| `Query` | `pub fn request(&self, explain: bool) -> Result<Vec<u8>>` | Build a query or read its results |
| `QueryResult` | `pub fn access(&self) -> AccessIter<'_>` | Build a query or read its results |
| `QueryResult` | `pub fn as_bytes(&self) -> &[u8]` | Build a query or read its results |
| `QueryResult` | `pub fn cursor(&self) -> Result<&[u8]>` | Build a query or read its results |
| `QueryResult` | `pub fn rows(&self) -> ResultRows<'_>` | Build a query or read its results |
| `QueryResult` | `pub fn stats(&self) -> Result<Stats>` | Build a query or read its results |
| `Row` | `pub fn expires_at_ms(&self) -> u64` | Build a query or read its results |
| `Row` | `pub fn found(&self) -> bool` | Build a query or read its results |
| `Row` | `pub fn value(&self) -> &'a [u8]` | Build a query or read its results |
| `Row` | `pub fn version(&self) -> u64` | Build a query or read its results |
| `Rows` | `pub fn get(&self, index: usize) -> Option<Row<'a>>` | Read data or status |
| `Rows` | `pub fn is_empty(&self) -> bool` | Build a query or read its results |
| `Rows` | `pub fn iter(&self) -> impl Iterator<Item = Row<'a>> + 'a` | Build a query or read its results |
| `Rows` | `pub fn len(&self) -> usize` | Build a query or read its results |
| `Sort` | `pub fn nulls_first(mut self, first: bool) -> Sort` | Build a query or read its results |
| `Sort` | `pub fn output(index: u32, descending: bool) -> Sort` | Build a query or read its results |
| `TypedRow` | `pub fn get(&self, item: &dyn Selectable) -> Result<Value<'a>>` | Read data or status |
| `TypedRow` | `pub fn key(&self) -> &'a [u8]` | Build a query or read its results |
| `fengqudb::builder` | `pub fn avg(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn boolean(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn count() -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn count_all_of(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn count_distinct(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn count_of(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn double(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn fixed32(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn fixed64(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn float32(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn from_(prefix: impl Into<Vec<u8>>) -> Builder` | Build a query or read its results |
| `fengqudb::builder` | `pub fn int64(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn key(source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn max(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn min(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn nested(path: &[u32], ty: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn sfixed32(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn sfixed64(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn sint64(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn sum(c: &Col) -> Agg` | Build a query or read its results |
| `fengqudb::builder` | `pub fn text(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::builder` | `pub fn uint64(n: u32, source: u32) -> Col` | Build a query or read its results |
| `fengqudb::query` | `pub fn feature_request(operation: u64, scope: &[u8], key: &[u8], value: &[u8]) -> Vec<u8>` | Build a query or read its results |

</details>

## 7. Indexes

define_compound_index creates field or compound indexes; drop_index removes them. Index field types must match the entity encoding.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn define_compound_index(&self, name: &str, key_prefix: &[u8], fields: &[IndexField], flags: u32) -> Result<()>` | Define or use an index |
| `Store` | `pub fn drop_index(&self, name: &str) -> Result<()>` | Define or use an index |

</details>

## 8. Wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```rust
db.put(b"saved", b"yes")?;
db.sync()?;
```

The safe client has no per-write forceReliable parameter. sync is an explicit durability barrier. Set database-wide reliable mode before writing:

```rust
let mut options = db.write_options()?;
options.mode = fengqudb::WriteMode::Reliable;
db.configure_write(&options)?;
```

write_options reads the settings; reset_write_options restores default performance mode. Settings are saved and survive reopening.

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn configure_write(&self, options: &WriteOptions) -> Result<()>` | Control durability or wait for a commit |
| `Store` | `pub fn reset_write_options(&self) -> Result<()>` | Control durability or wait for a commit |
| `Store` | `pub fn sync(&self) -> Result<()>` | Control durability or wait for a commit |
| `Store` | `pub fn write_options(&self) -> Result<WriteOptions>` | Control durability or wait for a commit |

</details>

## 9. Expiring data

put_with_expiry takes an absolute Unix timestamp in milliseconds; 0 means no expiry. Use it for temporary state and short-lived caches.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn put_with_expiry(&self, key: &[u8], value: &[u8], expires_at_ms: u64) -> Result<u64>` | Save or set data |

</details>

## 10. Hot-path shared reads

```rust
if let Some(view) = db.get_shared(b"student/7")? {
    consume(view.as_bytes());
}
```

Borrowed bytes become invalid when the lease is dropped. Reader's Rows borrow prevents using old rows across the next read. Copy data if it must live longer.

<details>
<summary>Functions and parameters (11)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `PreparedKey` | `pub fn get_shared(&self) -> Result<Option<SharedValue>>` | Read data or status |
| `Reader` | `pub fn key_count(&self) -> usize` | Reuse keys, buffers, or shared bytes |
| `Reader` | `pub fn read(&mut self) -> Result<Rows<'_>>` | Read data or status |
| `Reader` | `pub fn release(&mut self)` | Release an object or borrowed resource |
| `SharedValue` | `pub fn as_bytes(&self) -> &[u8]` | Reuse keys, buffers, or shared bytes |
| `SharedValue` | `pub fn expires_at_ms(&self) -> u64` | Reuse keys, buffers, or shared bytes |
| `SharedValue` | `pub fn is_empty(&self) -> bool` | Reuse keys, buffers, or shared bytes |
| `SharedValue` | `pub fn len(&self) -> usize` | Reuse keys, buffers, or shared bytes |
| `SharedValue` | `pub fn version(&self) -> u64` | Reuse keys, buffers, or shared bytes |
| `Store` | `pub fn get_shared(&self, key: &[u8]) -> Result<Option<SharedValue>>` | Read data or status |
| `Store` | `pub fn prepare_key(&self, key: &[u8]) -> Result<PreparedKey>` | Reuse keys, buffers, or shared bytes |

</details>

## 11. Consistent snapshots

snapshot creates a consistent view and Drop releases it. Do not retain snapshots you no longer use.

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Snapshot` | `pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>>` | Read data or status |
| `Snapshot` | `pub fn get_shared(&self, key: &[u8]) -> Result<Option<SharedValue>>` | Read data or status |
| `Store` | `pub fn snapshot(&self) -> Result<Snapshot<'_>>` | Consistent reads or range iteration |
| `TypedResult` | `pub fn cursor(&self) -> Result<&[u8]>` | Consistent reads or range iteration |

</details>

## maintenance

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Store` | `pub fn info(&self) -> Result<StoreInfo>` | Inspect status or maintain the database |

</details>

## 12. Entity encoding and decoding

Codec::encode_into accepts a reusable Vec; clear it before encoding to retain capacity. Struct fields must be pub and the struct must implement Default. Generation runs in cargo build's build script, not during database operations.

The safe client currently has no high-level backup, collection, maintenance, or subscription methods. fengqudb_sys at the end is the low-level unsafe API; application code should prefer the safe methods above.

## Other utilities

<details>
<summary>Functions and parameters (73)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Aggregate` | `pub fn from_u32(v: u32) -> Result<Aggregate>` | Utility operation |
| `Buffer` | `pub fn as_bytes(&self) -> &[u8]` | Utility operation |
| `Buffer` | `pub fn is_empty(&self) -> bool` | Utility operation |
| `Buffer` | `pub fn len(&self) -> usize` | Utility operation |
| `Buffer` | `pub fn to_vec(&self) -> Vec<u8>` | Utility operation |
| `Error` | `pub fn code(&self) -> Option<i32>` | Utility operation |
| `Error` | `pub fn is_status(&self, code: i32) -> bool` | Utility operation |
| `Expr` | `pub fn compare_field(op: Op, field: Field, other: Field) -> Expr` | Utility operation |
| `Expr` | `pub fn logic(op: Op, children: Vec<Expr>) -> Expr` | Utility operation |
| `Expr` | `pub fn test(op: Op, field: Field, values: Vec<QueryValue>) -> Expr` | Utility operation |
| `Field` | `pub fn any(&self) -> Field` | Utility operation |
| `Field` | `pub fn column(index: u32) -> Field` | Utility operation |
| `Field` | `pub fn key(source: u32) -> Field` | Utility operation |
| `Field` | `pub fn new(number: u32, ty: u32, source: u32) -> Field` | Utility operation |
| `Field` | `pub fn record(source: u32) -> Field` | Utility operation |
| `Join` | `pub fn new(prefix: Vec<u8>, left: Vec<Field>, right: Vec<Field>, kind: JoinKind) -> Join` | Utility operation |
| `JoinKind` | `pub fn from_u32(v: u32) -> Result<JoinKind>` | Utility operation |
| `Op` | `pub fn from_u32(v: u32) -> Result<Op>` | Utility operation |
| `Order` | `pub fn new(field: Field) -> Order` | Utility operation |
| `ResultRow` | `pub fn key(&self) -> &'a [u8]` | Utility operation |
| `ResultRow` | `pub fn value(&self, index: usize) -> Result<Option<Value<'a>>>` | Utility operation |
| `ResultRow` | `pub fn values(&self) -> ResultValues<'a>` | Utility operation |
| `Store` | `pub fn close(&self)` | Release an object or borrowed resource |
| `Store` | `pub fn is_closed(&self) -> bool` | Release an object or borrowed resource |
| `Store` | `pub fn open(path: impl AsRef<Path>, options: &Options) -> Result<Store>` | Utility operation |
| `TypedPrepared` | `pub fn run(&self, db: &impl SelectTarget) -> Result<TypedResult>` | Utility operation |
| `TypedResult` | `pub fn result(&self) -> &QueryResult` | Utility operation |
| `TypedResult` | `pub fn rows(&self) -> impl Iterator<Item = Result<TypedRow<'_>>>` | Utility operation |
| `Value` | `pub fn as_bool(&self) -> Option<bool>` | Utility operation |
| `Value` | `pub fn as_bytes(&self) -> Option<&'a [u8]>` | Utility operation |
| `Value` | `pub fn as_f64(&self) -> Option<f64>` | Utility operation |
| `Value` | `pub fn as_i64(&self) -> Option<i64>` | Utility operation |
| `Value` | `pub fn as_u64(&self) -> Option<u64>` | Utility operation |
| `Value` | `pub fn is_null(&self) -> bool` | Utility operation |
| `fengqudb::error` | `pub fn status_message(code: i32) -> String` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_abi_version() -> u32;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_buffer_free(buffer: *mut fqdb_buffer);` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_status_message(status: fqdb_status) -> *const c_char;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_close(store: *mut fqdb_store);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_delete( store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_store_atomic_info, force_reliable: u32, ) -> fqdb_status;` | Delete a record or member |
| `fengqudb_sys` | `pub fn fqdb_store_get(store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_buffer) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_get_info(store: *mut fqdb_store, out: *mut fqdb_store_info) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_get_shared( store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_store_shared_value, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_index_define_compound( store: *mut fqdb_store, name: fqdb_slice, key_prefix: fqdb_slice, fields: *const fqdb_store_index_field, count: u32, flags: u32, options: *const fqdb_store_maintenance_options, out: *mut fqdb_store_maintenance_info, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_index_drop( store: *mut fqdb_store, name: fqdb_slice, options: *const fqdb_store_maintenance_options, out: *mut fqdb_store_maintenance_info, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_key_close(key: *mut fqdb_store_key);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_key_get_shared( key: *mut fqdb_store_key, out: *mut fqdb_store_shared_value, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_key_prepare( store: *mut fqdb_store, key: fqdb_slice, out: *mut *mut fqdb_store_key, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_open( directory: *const c_char, options: *const fqdb_store_options, out: *mut *mut fqdb_store, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_put( store: *mut fqdb_store, key: fqdb_slice, value: fqdb_slice, expires_at_ms: u64, out: *mut fqdb_store_atomic_info, force_reliable: u32, ) -> fqdb_status;` | Save or set data |
| `fengqudb_sys` | `pub fn fqdb_store_reader_close(reader: *mut fqdb_store_reader);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_reader_create( store: *mut fqdb_store, keys: *const fqdb_slice, count: u32, out: *mut *mut fqdb_store_reader, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_reader_read( reader: *mut fqdb_store_reader, rows: *mut *const fqdb_store_result_row, count: *mut u32, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_reader_release(reader: *mut fqdb_store_reader);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_select( store: *mut fqdb_store, request: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_close(snapshot: *mut fqdb_store_snapshot);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_create( store: *mut fqdb_store, out: *mut *mut fqdb_store_snapshot, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_get( snapshot: *const fqdb_store_snapshot, key: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_snapshot_get_shared( snapshot: *const fqdb_store_snapshot, key: fqdb_slice, out: *mut fqdb_store_shared_value, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_sync(store: *mut fqdb_store) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_txn_begin( store: *mut fqdb_store, timeout_ms: u64, out: *mut *mut fqdb_store_txn, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_txn_close(txn: *mut fqdb_store_txn);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_txn_commit( txn: *mut fqdb_store_txn, version: *mut u64, force_reliable: u32, ) -> fqdb_status;` | Commit a transaction or inspect its result |
| `fengqudb_sys` | `pub fn fqdb_store_txn_delete(txn: *mut fqdb_store_txn, key: fqdb_slice) -> fqdb_status;` | Delete a record or member |
| `fengqudb_sys` | `pub fn fqdb_store_txn_execute( txn: *mut fqdb_store_txn, request: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_txn_get( txn: *mut fqdb_store_txn, key: fqdb_slice, out: *mut fqdb_buffer, ) -> fqdb_status;` | Read data or status |
| `fengqudb_sys` | `pub fn fqdb_store_txn_put( txn: *mut fqdb_store_txn, key: fqdb_slice, value: fqdb_slice, expires_at_ms: u64, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_txn_put_many( txn: *mut fqdb_store_txn, items: *const fqdb_store_put_item, count: u32, ) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_value_close(value: *mut fqdb_store_value);` | Release an object or borrowed resource |
| `fengqudb_sys` | `pub fn fqdb_store_write_configure(store: *mut fqdb_store, options: *const fqdb_store_write_options) -> fqdb_status;` | Utility operation |
| `fengqudb_sys` | `pub fn fqdb_store_write_options_get(store: *mut fqdb_store, out: *mut fqdb_store_write_options) -> fqdb_status;` | Read data or status |
| `fengqudb_sys::fqdb_slice` | `pub fn from_bytes(bytes: &[u8]) -> fqdb_slice` | Utility operation |
| `fengqudb_sys::fqdb_store_shared_value` | `pub const fn zeroed() -> Self` | Utility operation |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>Aggregate</summary>

| Declaration |
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

| Declaration |
| --- |
| `Buffered = 1,` |
| `Sync = 0,` |

</details>

<details>
<summary>Error</summary>

| Declaration |
| --- |
| `Closed,` |
| `Decode(&'static str),` |
| `Invalid(&'static str),` |
| `Native { code: i32, op: &'static str },` |

</details>

<details>
<summary>FieldType</summary>

| Declaration |
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

| Declaration |
| --- |
| `Anti = 3,` |
| `Inner = 0,` |
| `Left = 1,` |
| `Semi = 2,` |

</details>

<details>
<summary>Op</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `Performance = 1,` |
| `Reliable = 0,` |

</details>

<details>
<summary>fengqudb</summary>

| Declaration |
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

| Declaration |
| --- |
| `pub struct Buffer` |

</details>

<details>
<summary>fengqudb::builder</summary>

| Declaration |
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

| Declaration |
| --- |
| `pub enum Error` |
| `pub type Result<T> = std::result::Result<T, Error>;` |

</details>

<details>
<summary>fengqudb::query</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `pub struct PreparedKey` |
| `pub struct Reader` |
| `pub struct Row<'a>` |
| `pub struct Rows<'a>` |
| `pub struct SharedValue` |

</details>

<details>
<summary>fengqudb::store</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `assoc_const EMPTY` |

</details>

<details>
<summary>fengqudb_sys::fqdb_slice</summary>

| Declaration |
| --- |
| `assoc_const EMPTY` |

</details>
