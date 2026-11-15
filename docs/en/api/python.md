# Python API and examples

[Getting started](../languages/python.md) · [简体中文](../../api/python.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read: find a record by key

```python
data = db.get(b"student/7")
student = None if data is None else Codec.decode(data).value
```

A missing record returns None; empty bytes are valid data. Codec is StudentFqCodec generated in the getting-started guide.

<details>
<summary>Functions and parameters (3)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def get(self, key)` | Read data or status |
| `fengqudb.Store` | `def get_entries(self, keys)` | Read data or status |
| `fengqudb.Store` | `def get_entry(self, key)` | Read data or status |

</details>

## 2. Insert or save: entities, numbers, and strings

```python
student = Student(7, "Alice", 95)
db.put(Codec.key(student), Codec.encode(student))
db.put(b"name", "Alice".encode("utf-8"))
db.put(b"score", (95).to_bytes(8, "little", signed=True))
score = int.from_bytes(db.get(b"score"), "little", signed=True)
```

put overwrites the previous value at the same key. Single strings and integers need no entity definition; the integer format above also works with add_i64.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def put(self, key, value, *, expires_at_ms=0, force_reliable=False)` | Save or set data |

</details>

## 3. Update only what the application needs

```python
data = db.get(b"student/7")
if data is not None:
    row = Codec.decode(data)
    row.value.score = 96
    db.put(row.fq_key(), row.fq_encode())
db.add_i64(b"login-count", 1) # Use atomic increments for concurrent counters
```

patch changes a raw binary range, compare_exchange updates by version, and put_if_absent prevents overwriting an existing record.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def add_i64(self, key, delta, minimum=-(1 << 63), maximum=(1 << 63) - 1, timeout_ms=0, *, force_reliable=False)` | Conditional update or partial modification |
| `fengqudb.Store` | `def add_i64_fields(self, items, timeout_ms=0, *, force_reliable=False)` | Conditional update or partial modification |
| `fengqudb.Store` | `def compare_exchange(self, key, expected_version, value, expires_at_ms=0, timeout_ms=0, *, force_reliable=False)` | Conditional update or partial modification |
| `fengqudb.Store` | `def delete_if_version(self, key, expected_version, timeout_ms=0, *, force_reliable=False)` | Delete a record or member |
| `fengqudb.Store` | `def patch(self, key, offset, remove, insert, *, force_reliable=False)` | Conditional update or partial modification |
| `fengqudb.Store` | `def put_if_absent(self, key, value, expires_at_ms=0, timeout_ms=0, *, force_reliable=False)` | Save or set data |
| `fengqudb.Store` | `def put_if_changed(self, key, value, expires_at_ms=0, timeout_ms=0, *, force_reliable=False)` | Save or set data |

</details>

## 4. Delete

```python
db.delete(b"student/7")
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def delete(self, key, *, force_reliable=False)` | Delete a record or member |

</details>

## 5. Batch: read and write multiple records

```python
values = db.get_many([b"student/7", b"student/8"])
db.put_many([(b"a", b"one"), (b"b", b"two")])
db.delete_many([b"a", b"b"])
```

put_many/delete_many commit their changes as one atomic operation.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def delete_many(self, keys, *, force_reliable=False)` | Delete a record or member |
| `fengqudb.Store` | `def get_many(self, keys)` | Read data or status |
| `fengqudb.Store` | `def prepare_batch(self, items)` | Process a batch |
| `fengqudb.Store` | `def put_batch(self, batch, *, force_reliable=False)` | Save or set data |
| `fengqudb.Store` | `def put_many(self, items, *, force_reliable=False)` | Save or set data |

</details>

## 6. Transactions: changes that succeed together

```python
with db.transaction() as tx:
    tx.put(b"bag/item", b"sword")
    tx.delete(b"reward/pending")
```

A normal exit from with commits automatically; an exceptional exit discards changes. Use savepoint/rollback_to for partial rollback. On CONFLICT, create a new transaction, read again, and recompute.

<details>
<summary>Functions and parameters (26)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def transaction(self, timeout_ms=0)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def __enter__(self)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def __exit__(self, *_)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def __exit__(self, kind, *_)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def __init__(self, native, handle)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def cancel(self)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreTransaction` | `def commit(self, force_reliable=False)` | Commit a transaction or inspect its result |
| `fengqudb.StoreTransaction` | `def delete(self, key)` | Delete a record or member |
| `fengqudb.StoreTransaction` | `def delete_many(self, keys)` | Delete a record or member |
| `fengqudb.StoreTransaction` | `def execute(self, operation, scope, key=b'', value=b'', **options)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def expire(self, key, expires_at_ms)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def get(self, key)` | Read data or status |
| `fengqudb.StoreTransaction` | `def get_entries(self, keys)` | Read data or status |
| `fengqudb.StoreTransaction` | `def get_many(self, keys)` | Read data or status |
| `fengqudb.StoreTransaction` | `def patch(self, key, offset, remove, insert)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def project(self, key, fields)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def put(self, key, value, expires_at_ms=0)` | Save or set data |
| `fengqudb.StoreTransaction` | `def put_batch(self, batch)` | Save or set data |
| `fengqudb.StoreTransaction` | `def put_many(self, items)` | Save or set data |
| `fengqudb.StoreTransaction` | `def query_index(self, name, *, cursor=b'', limit=256, **query)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def release(self, savepoint)` | Release an object or borrowed resource |
| `fengqudb.StoreTransaction` | `def rollback_to(self, savepoint)` | Roll back to a savepoint |
| `fengqudb.StoreTransaction` | `def savepoint(self)` | Create or manage a transaction savepoint |
| `fengqudb.StoreTransaction` | `def scan(self, begin=b'', end=b'', *, reverse=False)` | Transaction operation |
| `fengqudb.StoreTransaction` | `def submit(self, force_reliable=False)` | Transaction operation |

</details>

## 7. Filtering, sorting, and aggregation

```python
from fengqudb_query import from_, s64, text
score = s64(Codec.Fields.score)
name = text(Codec.Fields.name)
rows = from_(b"student/").where(score >= 90).select(name, score) \
    .order_by(score.desc()).limit(10).run(db)
for row in rows:
    print(row[name], row[score])
```

Combine conditions with `& / | / ~`, rather than Python's and/or/not. group_by/having aggregates groups, join matches equal field values, and explain shows the query plan.

<details>
<summary>Functions and parameters (107)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def select_raw(self, operation, value)` | Build a query or read its results |
| `fengqudb_query` | `def avg(col)` | Build a query or read its results |
| `fengqudb_query` | `def boolean(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def count()` | Build a query or read its results |
| `fengqudb_query` | `def count_distinct(col)` | Build a query or read its results |
| `fengqudb_query` | `def count_of(col)` | Build a query or read its results |
| `fengqudb_query` | `def f32(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def f64(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def fixed32(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def fixed64(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def from_(prefix)` | Build a query or read its results |
| `fengqudb_query` | `def i64(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def key(source=0)` | Build a query or read its results |
| `fengqudb_query` | `def max_(col)` | Build a query or read its results |
| `fengqudb_query` | `def min_(col)` | Build a query or read its results |
| `fengqudb_query` | `def nested(path, type, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def s64(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def sfixed32(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def sfixed64(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def sum_(col)` | Build a query or read its results |
| `fengqudb_query` | `def text(n, source=0)` | Build a query or read its results |
| `fengqudb_query` | `def u64(n, source=0)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __eq__(self, v)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __ge__(self, v)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __gt__(self, v)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __init__(self, column)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __le__(self, v)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __lt__(self, v)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def __ne__(self, v)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def asc(self, nulls_first=True)` | Build a query or read its results |
| `fengqudb_query.Agg` | `def desc(self, nulls_first=True)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def __init__(self, prefix)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def after(self, cursor)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def allow_scan(self, on)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def build(self)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def count_total(self, on=True)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def distinct(self, on=True)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def explain(self, database)` | Inspect the query plan |
| `fengqudb_query.Builder` | `def group_by(self, *cols)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def having(self, cond)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def index(self, name)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def join(self, prefix, left, right, kind=JoinKind.INNER)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def limit(self, n)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def memory(self, n)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def offset(self, n)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def order_by(self, *items)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def prepare(self)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def run(self, database)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def select(self, *items)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def timeout_ms(self, n)` | Build a query or read its results |
| `fengqudb_query.Builder` | `def where(self, cond)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __eq__(self, v)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __ge__(self, v)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __gt__(self, v)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __init__(self, field, cast=None)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __le__(self, v)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __lt__(self, v)` | Build a query or read its results |
| `fengqudb_query.Col` | `def __ne__(self, v)` | Build a query or read its results |
| `fengqudb_query.Col` | `def any(self)` | Build a query or read its results |
| `fengqudb_query.Col` | `def asc(self, nulls_first=True)` | Build a query or read its results |
| `fengqudb_query.Col` | `def between(self, low, high)` | Build a query or read its results |
| `fengqudb_query.Col` | `def contains(self, s)` | Build a query or read its results |
| `fengqudb_query.Col` | `def desc(self, nulls_first=True)` | Build a query or read its results |
| `fengqudb_query.Col` | `def ends_with(self, s)` | Build a query or read its results |
| `fengqudb_query.Col` | `def exists(self)` | Build a query or read its results |
| `fengqudb_query.Col` | `def fuzzy(self, s, distance)` | Build a query or read its results |
| `fengqudb_query.Col` | `def icontains(self, s)` | Build a query or read its results |
| `fengqudb_query.Col` | `def ilike(self, s)` | Build a query or read its results |
| `fengqudb_query.Col` | `def in_(self, values)` | Build a query or read its results |
| `fengqudb_query.Col` | `def is_null(self)` | Build a query or read its results |
| `fengqudb_query.Col` | `def like(self, s)` | Build a query or read its results |
| `fengqudb_query.Col` | `def regex(self, s)` | Build a query or read its results |
| `fengqudb_query.Col` | `def starts_with(self, s)` | Build a query or read its results |
| `fengqudb_query.Cond` | `def __and__(self, other)` | Build a query or read its results |
| `fengqudb_query.Cond` | `def __bool__(self)` | Build a query or read its results |
| `fengqudb_query.Cond` | `def __init__(self, resolve, aggregate=False)` | Build a query or read its results |
| `fengqudb_query.Cond` | `def __invert__(self)` | Build a query or read its results |
| `fengqudb_query.Cond` | `def __or__(self, other)` | Build a query or read its results |
| `fengqudb_query.Cond` | `def build(self, columns)` | Build a query or read its results |
| `fengqudb_query.Prepared` | `def __init__(self, prepared, columns)` | Build a query or read its results |
| `fengqudb_query.Prepared` | `def run(self, database)` | Build a query or read its results |
| `fengqudb_query.Rows` | `def __getitem__(self, i)` | Read data or status |
| `fengqudb_query.Rows` | `def __init__(self, columns, result)` | Build a query or read its results |
| `fengqudb_query.Rows` | `def __iter__(self)` | Build a query or read its results |
| `fengqudb_query.Rows` | `def __len__(self)` | Build a query or read its results |
| `fengqudb_select.Column` | `def encode(self)` | Build a query or read its results |
| `fengqudb_select.Expr` | `def __and__(self, other)` | Build a query or read its results |
| `fengqudb_select.Expr` | `def __invert__(self)` | Build a query or read its results |
| `fengqudb_select.Expr` | `def __or__(self, other)` | Build a query or read its results |
| `fengqudb_select.Expr` | `def encode(self, depth=0)` | Build a query or read its results |
| `fengqudb_select.Field` | `def any(self)` | Build a query or read its results |
| `fengqudb_select.Field` | `def column(index)` | Build a query or read its results |
| `fengqudb_select.Field` | `def compare_field(self, op, other)` | Build a query or read its results |
| `fengqudb_select.Field` | `def encode(self)` | Build a query or read its results |
| `fengqudb_select.Field` | `def key(source=0)` | Build a query or read its results |
| `fengqudb_select.Field` | `def record(source=0)` | Build a query or read its results |
| `fengqudb_select.Field` | `def test(self, op, *values)` | Build a query or read its results |
| `fengqudb_select.Join` | `def encode(self)` | Build a query or read its results |
| `fengqudb_select.Order` | `def encode(self)` | Build a query or read its results |
| `fengqudb_select.PreparedQuery` | `def __post_init__(self)` | Build a query or read its results |
| `fengqudb_select.PreparedQuery` | `def execute(self, database)` | Build a query or read its results |
| `fengqudb_select.PreparedQuery` | `def explain(self, database)` | Inspect the query plan |
| `fengqudb_select.Query` | `def encode(self)` | Build a query or read its results |
| `fengqudb_select.Query` | `def execute(self, database)` | Build a query or read its results |
| `fengqudb_select.Query` | `def explain(self, database)` | Inspect the query plan |
| `fengqudb_select.Query` | `def prepare(self)` | Build a query or read its results |
| `fengqudb_select.Result` | `def __init__(self, data)` | Build a query or read its results |

</details>

## 8. Indexes for repeated field queries

```python
db.define_index(b"student-score", b"student/", Codec.Fields.score, 5)
```

5 means a signed ZigZag integer. Use define_compound_index options for compound indexes and unique/required constraints.

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def define_compound_index(self, name, key_prefix, fields, flags=0, **options)` | Define or use an index |
| `fengqudb.Store` | `def define_index(self, name, key_prefix, field, field_type, flags=0, **options)` | Define or use an index |
| `fengqudb.Store` | `def drop_index(self, name, **options)` | Define or use an index |
| `fengqudb.Store` | `def query_index(self, name, *, cursor=b'', limit=256, **query)` | Define or use an index |

</details>

## 9. Critical writes: wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```python
db.put(b"payment/receipt", b"paid", force_reliable=True)
db.sync()
db.configure_write(mode="reliable") # Require reliable confirmation for every write
```

Asynchronous submit_put returns a receipt; wait for or poll its result. On COMMIT_UNKNOWN, reconcile by business operation ID instead of charging again.

<details>
<summary>Functions and parameters (16)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def configure_write(self, *, mode=StoreWriteMode.PERFORMANCE, period_ms=50, threshold_bytes=1048576)` | Control durability or wait for a commit |
| `fengqudb.Store` | `async def configure_write_async(self, **options)` | Control durability or wait for a commit |
| `fengqudb.Store` | `def reset_write_options(self)` | Control durability or wait for a commit |
| `fengqudb.Store` | `async def reset_write_options_async(self)` | Control durability or wait for a commit |
| `fengqudb.Store` | `def submit_put(self, key, value, *, expires_at_ms=0, force_reliable=False)` | Control durability or wait for a commit |
| `fengqudb.Store` | `def sync(self)` | Control durability or wait for a commit |
| `fengqudb.Store` | `def write_options(self)` | Control durability or wait for a commit |
| `fengqudb.Store` | `def write_state(self)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `def __enter__(self)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `def __exit__(self, *_)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `def __init__(self, native, handle)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `def cancel(self)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreCommit` | `def poll(self)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `def wait(self, timeout_ms=0)` | Control durability or wait for a commit |
| `fengqudb.StoreCommit` | `async def wait_async(self, poll_interval=0.005)` | Control durability or wait for a commit |

</details>

## 10. Expiring data

```python
import time
expires = int(time.time() * 1000) + 5 * 60_000
db.put(b"verification", b"1234", expires_at_ms=expires)
```

Expiry is an absolute Unix timestamp in milliseconds; 0 means no expiry.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def purge_expired(self, *, max_keys=1024, timeout_ms=0, max_batch_bytes=1048576)` | Manage expiring data |

</details>

## 11. Hot-path and shared reads

```python
with db.prepare_reader([b"student/7", b"student/8"]) as reader:
    rows = reader.read()
```

Ordinary get returns independently owned bytes. Close shared values. A reader's memoryview remains valid only until the next read/release/close. Copy it if it must live longer.

<details>
<summary>Functions and parameters (36)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def get_many_shared(self, keys)` | Read data or status |
| `fengqudb.Store` | `def get_shared(self, key)` | Read data or status |
| `fengqudb.Store` | `def prepare_key(self, key)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Store` | `def prepare_reader(self, keys)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreBufferReader` | `def __del__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreBufferReader` | `def __enter__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreBufferReader` | `def __exit__(self, *_)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreBufferReader` | `def __init__(self, key, destination)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreBufferReader` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreBufferReader` | `def read(self)` | Read data or status |
| `fengqudb.StorePreparedKey` | `def __enter__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StorePreparedKey` | `def __exit__(self, *_)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StorePreparedKey` | `def __init__(self, native, handle)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StorePreparedKey` | `def bind_buffer(self, destination)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StorePreparedKey` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StorePreparedKey` | `def get(self)` | Read data or status |
| `fengqudb.StorePreparedKey` | `def get_entry(self)` | Read data or status |
| `fengqudb.StorePreparedKey` | `def get_into(self, destination)` | Read data or status |
| `fengqudb.StorePreparedKey` | `def get_shared(self)` | Read data or status |
| `fengqudb.StoreReader` | `def __enter__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreReader` | `def __exit__(self, *_)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreReader` | `def __init__(self, native, handle, count)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreReader` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreReader` | `def read_shared(self)` | Read data or status |
| `fengqudb.StoreReader` | `def release(self)` | Release an object or borrowed resource |
| `fengqudb.StoreSharedBatch` | `def __enter__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedBatch` | `def __exit__(self, *_)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedBatch` | `def __getitem__(self, index)` | Read data or status |
| `fengqudb.StoreSharedBatch` | `def __init__(self, lease, count)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedBatch` | `def __len__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedBatch` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreSharedValue` | `def __enter__(self)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedValue` | `def __exit__(self, *_)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedValue` | `def __init__(self, row, lease)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.StoreSharedValue` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreSharedValue` | `def value(self)` | Reuse keys, buffers, or shared bytes |

</details>

## 12. Consistent snapshots

```python
with db.snapshot() as snapshot:
    first = snapshot.get(b"a")
    second = snapshot.get(b"b")
```

<details>
<summary>Functions and parameters (14)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def snapshot(self)` | Consistent reads or range iteration |
| `fengqudb.StoreCursor` | `def __enter__(self)` | Consistent reads or range iteration |
| `fengqudb.StoreCursor` | `def __exit__(self, *_)` | Consistent reads or range iteration |
| `fengqudb.StoreCursor` | `def __iter__(self)` | Consistent reads or range iteration |
| `fengqudb.StoreCursor` | `def __next__(self)` | Consistent reads or range iteration |
| `fengqudb.StoreCursor` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreSnapshot` | `def __enter__(self)` | Consistent reads or range iteration |
| `fengqudb.StoreSnapshot` | `def __exit__(self, *_)` | Consistent reads or range iteration |
| `fengqudb.StoreSnapshot` | `def __init__(self, native, handle)` | Consistent reads or range iteration |
| `fengqudb.StoreSnapshot` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreSnapshot` | `def get(self, key)` | Read data or status |
| `fengqudb.StoreSnapshot` | `def get_entries(self, keys)` | Read data or status |
| `fengqudb.StoreSnapshot` | `def get_many(self, keys)` | Read data or status |
| `fengqudb.StoreSnapshot` | `def scan(self, begin=b'', end=b'', *, reverse=False)` | Consistent reads or range iteration |

</details>

## 13. Backup and restore

```python
db.backup("backups/full-001")
```

Use a new destination directory. Restore with restore/recover_chain after closing the database being replaced. Do not copy a directory while it is being written.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def backup(self, destination, **options)` | Backup, restore, or history retention |
| `fengqudb.Store` | `def backup_incremental(self, destination, from_version, **options)` | Backup, restore, or history retention |
| `fengqudb.Store` | `def history_info(self)` | Backup, restore, or history retention |
| `fengqudb.Store` | `def hold_history(self, name, from_version=None, timeout_ms=0)` | Backup, restore, or history retention |
| `fengqudb.Store` | `def recover_chain(base, archives, destination, *, target_version=0, target_unix_ms=0, library=None, **options)` | Backup, restore, or history retention |
| `fengqudb.Store` | `def release_history(self, name, timeout_ms=0)` | Release an object or borrowed resource |
| `fengqudb.Store` | `def restore(source, destination, *, library=None, **options)` | Backup, restore, or history retention |

</details>

## 14. Native collections

execute runs Hash/Set/List/sorted-set commands; hash_get reads a Hash field. Use them for atomic member changes, queues, and rankings.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def execute(self, operation, scope, key=b'', value=b'', *, force_reliable=False, **options)` | Native collection operation |
| `fengqudb.Store` | `def hash_get(self, scope, key)` | Read data or status |

</details>

## 15. Cleanup, compaction, and verification

```python
db.purge_expired(max_keys=1000)
db.compact()
db.verify()
```

<details>
<summary>Functions and parameters (6)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def checkpoint(self)` | Inspect status or maintain the database |
| `fengqudb.Store` | `def compact(self, **options)` | Inspect status or maintain the database |
| `fengqudb.Store` | `def configure(self, *, background=False, interval_ms=200, expiry_keys=256, group_max=128, group_delay_us=0, garbage_percent=30, compact_min_bytes=33554432, compact_growth_bytes=33554432, job_timeout_ms=1000, job_max_read_bytes=0, job_max_write_bytes=0)` | Inspect status or maintain the database |
| `fengqudb.Store` | `def info(self)` | Inspect status or maintain the database |
| `fengqudb.Store` | `def runtime_info(self)` | Inspect status or maintain the database |
| `fengqudb.Store` | `def verify(self, **options)` | Inspect status or maintain the database |

</details>

## 16. Subscribe to commit changes

watch returns a consumer; ack the processed watermark and close when done. Use it to update in-process caches and trigger background tasks.

<details>
<summary>Functions and parameters (10)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `def watch(self, from_version=None, prefix=b'', *, metadata_only=False, include_system=False, max_scan_commits=256, max_batch_bytes=67108864)` | Consume changes or diagnose issues |
| `fengqudb.StoreChange` | `def __init__(self, key, value, kind, expires_at_ms)` | Consume changes or diagnose issues |
| `fengqudb.StoreChanges` | `def __init__(self, data)` | Consume changes or diagnose issues |
| `fengqudb.StoreWatch` | `def __enter__(self)` | Consume changes or diagnose issues |
| `fengqudb.StoreWatch` | `def __exit__(self, *_)` | Consume changes or diagnose issues |
| `fengqudb.StoreWatch` | `def __init__(self, native, handle)` | Consume changes or diagnose issues |
| `fengqudb.StoreWatch` | `def ack(self, version)` | Consume changes or diagnose issues |
| `fengqudb.StoreWatch` | `def cancel(self)` | Consume changes or diagnose issues |
| `fengqudb.StoreWatch` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreWatch` | `def next_batch(self, timeout_ms=0)` | Consume changes or diagnose issues |

</details>

## 17. Entity encoding and decoding

Codec.encode accepts Student; decode returns a record containing your Student. Use row.fq_encode after modification to preserve unknown fields. encode_into reuses a bytearray; clear it before reuse. Generation runs at development startup or build time, not on each database operation.

## Other utilities

<details>
<summary>Functions and parameters (18)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.BufferTooSmall` | `def __init__(self, required_size)` | Utility operation |
| `fengqudb.Error` | `def __init__(self, code, message)` | Utility operation |
| `fengqudb.Store` | `def __enter__(self)` | Utility operation |
| `fengqudb.Store` | `def __exit__(self, *_)` | Utility operation |
| `fengqudb.Store` | `def __init__(self, path, *, name=None, library=None, buffered=False, create=True, segmented=False, cache_bytes=64 * 1024 * 1024, delta_bytes=4 * 1024 * 1024, writer_bytes=32 * 1024 * 1024, transaction_bytes=64 * 1024 * 1024, all_transaction_bytes=256 * 1024 * 1024, pinned_bytes=128 * 1024 * 1024, max_snapshots=1024)` | Utility operation |
| `fengqudb.Store` | `def cancel_maintenance(self)` | Utility operation |
| `fengqudb.Store` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.Store` | `def open_at(cls, root, name, **options)` | Utility operation |
| `fengqudb.Store` | `def path_at(root, name, *, library=None)` | Utility operation |
| `fengqudb.Store` | `def prepare_project_reader(self, keys, fields)` | Utility operation |
| `fengqudb.Store` | `def project(self, key, fields)` | Utility operation |
| `fengqudb.StoreBatch` | `def __enter__(self)` | Utility operation |
| `fengqudb.StoreBatch` | `def __exit__(self, *_)` | Utility operation |
| `fengqudb.StoreBatch` | `def __init__(self, native, handle)` | Utility operation |
| `fengqudb.StoreBatch` | `def close(self)` | Release an object or borrowed resource |
| `fengqudb.StoreCommandResult` | `def __init__(self)` | Utility operation |
| `fengqudb.StoreEntry` | `def __init__(self, value, version, expires_at_ms)` | Utility operation |
| `fengqudb.StoreWriteOptions` | `def __init__(self, mode=StoreWriteMode.PERFORMANCE, period_ms=50, threshold_bytes=1048576)` | Utility operation |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>fengqudb</summary>

| Declaration |
| --- |
| `class BufferTooSmall` |
| `class Error` |
| `class Slice` |
| `class Store` |
| `class StoreAtomicInfo` |
| `class StoreBatch` |
| `class StoreBufferReader` |
| `class StoreChange` |
| `class StoreChanges` |
| `class StoreCommandResult` |
| `class StoreCommit` |
| `class StoreCursor` |
| `class StoreEntry` |
| `class StoreExpiryInfo` |
| `class StoreExpiryOptions` |
| `class StoreHistoryInfo` |
| `class StoreIndexField` |
| `class StoreInfo` |
| `class StoreMaintenanceIO` |
| `class StoreMaintenanceInfo` |
| `class StoreMaintenanceOptions` |
| `class StoreOperation` |
| `class StoreOptions` |
| `class StorePreparedKey` |
| `class StoreReader` |
| `class StoreRuntimeInfo` |
| `class StoreRuntimeOptions` |
| `class StoreSharedBatch` |
| `class StoreSharedValue` |
| `class StoreSnapshot` |
| `class StoreTransaction` |
| `class StoreWatch` |
| `class StoreWriteMode` |
| `class StoreWriteOptions` |
| `class StoreWriteState` |

</details>

<details>
<summary>fengqudb.BufferTooSmall</summary>

| Declaration |
| --- |
| `required_size` |

</details>

<details>
<summary>fengqudb.Error</summary>

| Declaration |
| --- |
| `code` |

</details>

<details>
<summary>fengqudb.Slice</summary>

| Declaration |
| --- |
| `data: C.c_void_p` |
| `size: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreAtomicInfo</summary>

| Declaration |
| --- |
| `changed: C.c_uint32` |
| `matched: C.c_uint32` |
| `observed_version: C.c_uint64` |
| `value: C.c_int64` |
| `version: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreChanges</summary>

| Declaration |
| --- |
| `rows` |

</details>

<details>
<summary>fengqudb.StoreCommandResult</summary>

| Declaration |
| --- |
| `cursor` |

</details>

<details>
<summary>fengqudb.StoreIndexField</summary>

| Declaration |
| --- |
| `field: C.c_uint32` |
| `type: C.c_uint32` |

</details>

<details>
<summary>fengqudb.StoreMaintenanceIO</summary>

| Declaration |
| --- |
| `read_bytes: C.c_uint64` |
| `sync_calls: C.c_uint64` |
| `write_bytes: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreWriteMode</summary>

| Declaration |
| --- |
| `PERFORMANCE = 1` |
| `RELIABLE = 0` |

</details>

<details>
<summary>fengqudb.StoreWriteOptions</summary>

| Declaration |
| --- |
| `abi_version: C.c_uint32` |
| `mode: C.c_uint32` |
| `period_ms: C.c_uint32` |
| `struct_size: C.c_uint32` |
| `threshold_bytes: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreWriteState</summary>

| Declaration |
| --- |
| `durable_version: C.c_uint64` |
| `last_status: C.c_uint64` |
| `pending_bytes: C.c_uint64` |
| `version: C.c_uint64` |

</details>

<details>
<summary>fengqudb_query</summary>

| Declaration |
| --- |
| `class Agg` |
| `class Builder` |
| `class Col` |
| `class Cond` |
| `class Prepared` |
| `class Rows` |

</details>

<details>
<summary>fengqudb_query.Agg</summary>

| Declaration |
| --- |
| `column` |

</details>

<details>
<summary>fengqudb_select</summary>

| Declaration |
| --- |
| `class Aggregate` |
| `class Column` |
| `class Expr` |
| `class Field` |
| `class Join` |
| `class JoinKind` |
| `class Op` |
| `class Order` |
| `class PreparedQuery` |
| `class Query` |
| `class Result` |

</details>

<details>
<summary>fengqudb_select.Aggregate</summary>

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
<summary>fengqudb_select.Column</summary>

| Declaration |
| --- |
| `aggregate: Aggregate = Aggregate.NONE` |
| `distinct: bool = False` |
| `field: Field = Field()` |
| `name: str = ''` |

</details>

<details>
<summary>fengqudb_select.Expr</summary>

| Declaration |
| --- |
| `children: tuple = ()` |
| `field: Field = Field()` |
| `op: Op = Op.TRUE` |
| `right: object = None` |
| `values: tuple = ()` |

</details>

<details>
<summary>fengqudb_select.Field</summary>

| Declaration |
| --- |
| `number: int = 0` |
| `path: tuple = ()` |
| `repeated: bool = False` |
| `source: int = 0` |
| `type: int = 4` |

</details>

<details>
<summary>fengqudb_select.Join</summary>

| Declaration |
| --- |
| `kind: JoinKind = JoinKind.INNER` |
| `left: tuple` |
| `on: Expr = Expr()` |
| `prefix: bytes` |
| `right: tuple` |

</details>

<details>
<summary>fengqudb_select.JoinKind</summary>

| Declaration |
| --- |
| `ANTI = 3` |
| `INNER = 0` |
| `LEFT = 1` |
| `SEMI = 2` |

</details>

<details>
<summary>fengqudb_select.Op</summary>

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
<summary>fengqudb_select.Order</summary>

| Declaration |
| --- |
| `descending: bool = False` |
| `field: Field` |
| `nulls_first: bool = True` |

</details>

<details>
<summary>fengqudb_select.PreparedQuery</summary>

| Declaration |
| --- |
| `encoded: bytes` |

</details>

<details>
<summary>fengqudb_select.Query</summary>

| Declaration |
| --- |
| `after: bytes = b''` |
| `allow_scan: bool = True` |
| `columns: list = factory(default_factory=list)` |
| `count_total: bool = False` |
| `distinct: bool = False` |
| `group: list = factory(default_factory=list)` |
| `having: Expr = Expr()` |
| `index_hint: bytes = b''` |
| `joins: list = factory(default_factory=list)` |
| `limit: int = 100` |
| `max_candidates: int = 1000000` |
| `max_work: int = 100000000` |
| `memory_bytes: int = 16 << 20` |
| `offset: int = 0` |
| `order: list = factory(default_factory=list)` |
| `prefix: bytes` |
| `timeout_ms: int = 1000` |
| `where: Expr = Expr()` |

</details>

<details>
<summary>fengqudb_select.Result</summary>

| Declaration |
| --- |
| `access` |
| `cursor` |
| `rows` |
| `stats` |

</details>
