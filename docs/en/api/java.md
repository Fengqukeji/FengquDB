# Java API and examples

[Getting started](../languages/java.md) · [简体中文](../../api/java.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read a student

```java
byte[] bytes = db.get("student/7".getBytes(StandardCharsets.UTF_8));
Student student = bytes == null ? null : StudentFqCodec.decode(bytes).value;
```

A missing record returns null; an empty array is valid data. This page uses `java.nio.charset.StandardCharsets`.

<details>
<summary>Functions and parameters (3)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public byte[] get(byte[] key)` | Read data or status |
| `io.fengqudb.Store` | `public Entry[] getEntries(byte[][] keys)` | Read data or status |
| `io.fengqudb.Store` | `public Entry getEntry(byte[] key)` | Read data or status |

</details>

## 2. Insert, save, and store a string or integer

```java
Student student = new Student();
student.id=7; student.name="Alice"; student.score=95;
db.put(StudentFqCodec.key(student), StudentFqCodec.encode(student));
db.put("name".getBytes(StandardCharsets.UTF_8), "Alice".getBytes(StandardCharsets.UTF_8));
```

A single integer needs no entity; use eight-byte little-endian encoding:

```java
byte[] key = "score".getBytes(StandardCharsets.UTF_8);
byte[] value = java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(95).array();
db.put(key, value);
long score = java.nio.ByteBuffer.wrap(db.get(key)).order(java.nio.ByteOrder.LITTLE_ENDIAN).getLong();
```

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public AtomicInfo put(byte[] key, byte[] value, boolean... forceReliable)` | Save or set data |
| `io.fengqudb.Store` | `public AtomicInfo put(byte[] key, byte[] value, long expiresAtMs, boolean... forceReliable)` | Save or set data |
| `io.fengqudb.Store.Put` | `public Put(byte[] key, byte[] value)` | Create an object |
| `io.fengqudb.Store.Put` | `public Put(byte[] key, byte[] value, long expiresAtMs)` | Create an object |

</details>

## 3. Update scores and use atomic counters

```java
byte[] key = "student/7".getBytes(StandardCharsets.UTF_8);
byte[] bytes = db.get(key);
if (bytes != null) {
    StudentFqCodec.Record row = StudentFqCodec.decode(bytes);
    row.value.score = 96;
    db.put(row.fqKey(), row.fqEncode());
}
db.addInt64("login-count".getBytes(StandardCharsets.UTF_8), 1);
```

addInt64 is only for eight-byte integers. compareExchange updates by version; patch changes raw byte ranges managed by your application.

<details>
<summary>Functions and parameters (10)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public AtomicInfo addInt64(byte[] key, long delta)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo addInt64(byte[] key, long delta, long minimum, long maximum, long timeout, boolean... forceReliable)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo addInt64Fields(Int64FieldUpdate[] items)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo addInt64Fields(Int64FieldUpdate[] items, long timeout, boolean... forceReliable)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo compareExchange(byte[] key, long expected, byte[] value)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo compareExchange(byte[] key, long expected, byte[] value, long expires, long timeout, boolean... forceReliable)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo deleteIfVersion(byte[] key, long expected, boolean... forceReliable)` | Delete a record or member |
| `io.fengqudb.Store` | `public AtomicInfo patch(byte[] key, long offset, long remove, byte[] insert, boolean... forceReliable)` | Conditional update or partial modification |
| `io.fengqudb.Store` | `public AtomicInfo putIfAbsent(byte[] key, byte[] value)` | Save or set data |
| `io.fengqudb.Store` | `public AtomicInfo putIfChanged(byte[] key, byte[] value, boolean... forceReliable)` | Save or set data |

</details>

## 4. Delete

```java
db.delete("student/7".getBytes(StandardCharsets.UTF_8));
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public AtomicInfo delete(byte[] key, boolean... forceReliable)` | Delete a record or member |

</details>

## 5. Batch reads and writes

```java
byte[][] keys = { "student/7".getBytes(StandardCharsets.UTF_8), "student/8".getBytes(StandardCharsets.UTF_8) };
byte[][] values = db.getMany(keys);
```

putMany/deleteMany apply multiple changes in one atomic commit.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public long deleteMany(byte[][] keys, boolean... forceReliable)` | Delete a record or member |
| `io.fengqudb.Store` | `public byte[][] getMany(byte[][] keys)` | Read data or status |
| `io.fengqudb.Store` | `public PreparedBatch prepareBatch(Put[] items)` | Process a batch |
| `io.fengqudb.Store` | `public long putBatch(PreparedBatch batch, boolean... forceReliable)` | Save or set data |
| `io.fengqudb.Store` | `public long putMany(Put[] items, boolean... forceReliable)` | Save or set data |

</details>

## 6. Transactions: changes that succeed together

```java
try (Store.Transaction tx = db.begin()) {
    tx.put("a".getBytes(StandardCharsets.UTF_8), new byte[]{1});
    tx.put("b".getBytes(StandardCharsets.UTF_8), new byte[]{2});
    tx.commit();
}
```

Closing without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute.

<details>
<summary>Functions and parameters (26)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Transaction begin()` | Transaction operation |
| `io.fengqudb.Store` | `public Transaction begin(long timeoutMs)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public void cancel()` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.Transaction` | `public long commit(boolean... forceReliable)` | Commit a transaction or inspect its result |
| `io.fengqudb.Store.Transaction` | `public void delete(byte[] key)` | Delete a record or member |
| `io.fengqudb.Store.Transaction` | `public void deleteMany(byte[][] keys)` | Delete a record or member |
| `io.fengqudb.Store.Transaction` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value, long argument, double score, long limit, int flags)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public void expire(byte[] key, long expiresAtMs)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public byte[] get(byte[] key)` | Read data or status |
| `io.fengqudb.Store.Transaction` | `public Entry[] getEntries(byte[][] keys)` | Read data or status |
| `io.fengqudb.Store.Transaction` | `public byte[][] getMany(byte[][] keys)` | Read data or status |
| `io.fengqudb.Store.Transaction` | `public void patch(byte[] key, long offset, long remove, byte[] insert)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public byte[] project(byte[] key, int[] fields)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public void put(byte[] key, byte[] value)` | Save or set data |
| `io.fengqudb.Store.Transaction` | `public void put(byte[] key, byte[] value, long expiresAtMs)` | Save or set data |
| `io.fengqudb.Store.Transaction` | `public void putBatch(PreparedBatch batch)` | Save or set data |
| `io.fengqudb.Store.Transaction` | `public void putMany(Put[] items)` | Save or set data |
| `io.fengqudb.Store.Transaction` | `public CommandResult queryIndex(byte[] name, IndexQuery query, byte[] cursor, long limit)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public void release(long id)` | Release an object or borrowed resource |
| `io.fengqudb.Store.Transaction` | `public void rollbackTo(long id)` | Roll back to a savepoint |
| `io.fengqudb.Store.Transaction` | `public long savepoint()` | Create or manage a transaction savepoint |
| `io.fengqudb.Store.Transaction` | `public Cursor scan()` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public Cursor scan(byte[] begin, byte[] end, boolean reverse)` | Transaction operation |
| `io.fengqudb.Store.Transaction` | `public CommitReceipt submit(boolean... forceReliable)` | Transaction operation |

</details>

## 7. Conditional queries and sorting

```java
var score = io.fengqudb.Q.s64(StudentFqCodec.Field_score);
var name = io.fengqudb.Q.text(StudentFqCodec.Field_name);
var rows = io.fengqudb.Q.from("student/").where(score.ge(90L))
    .select(name, score).orderBy(score.desc()).limit(10).run(db);
```

Combine conditions with and/or/not; groupBy/having aggregates groups, join performs equality joins, and explain shows index selection.

<details>
<summary>Functions and parameters (116)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Q` | `public static Agg<Double> avg(Col<?> c)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Boolean> bool(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Agg<Long> count()` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Agg<Long> count(Col<?> c)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Double> f32(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Double> f64(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> fixed32(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> fixed64(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Builder from(String prefix)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Builder from(byte[] prefix)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> i64(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<String> key(int source)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static <T> Agg<T> max(Col<T> c)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static <T> Agg<T> min(Col<T> c)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> s64(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> s64(int n, int source)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> sfixed32(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> sfixed64(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static <T> Agg<T> sum(Col<T> c)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<String> text(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<String> text(int n, int source)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> u64(int n)` | Build a query or read its results |
| `io.fengqudb.Q` | `public static Col<Long> u64(int n, int source)` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Sort asc()` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Select.Column column()` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Sort desc()` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Cond eq(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Cond ge(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Cond gt(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Cond le(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Cond lt(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Agg` | `public Cond ne(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder after(byte[] cursor)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder allowScan(boolean on)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Select build()` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder countTotal(boolean on)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder distinct(boolean on)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder groupBy(Col<?>... cols)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder having(Cond c)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public <T> Builder join(String prefix, Col<T> left, Col<T> right)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public <T> Builder join(String prefix, Col<T> left, Col<T> right, Select.JoinKind kind)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder limit(long n)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder offset(long n)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder orderBy(Object... items)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Prepared prepare()` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Rows run(Store db)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Rows run(Store.Transaction tx)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder select(Object... items)` | Build a query or read its results |
| `io.fengqudb.Q.Builder` | `public Builder where(Cond c)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Col<T> any()` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Sort asc()` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond between(T low, T high)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Select.Column column()` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond contains(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Sort desc()` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond endsWith(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond eq(Col<T> o)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond eq(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond exists()` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond fuzzy(String s, int distance)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond ge(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond gt(Col<T> o)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond gt(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond icontains(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond ilike(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `@SafeVarargs() public final Cond in(T... values)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond isNull()` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond le(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond like(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond lt(Col<T> o)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond lt(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond ne(T v)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond regex(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Col` | `public Cond startsWith(String s)` | Build a query or read its results |
| `io.fengqudb.Q.Cond` | `public Cond and(Cond other)` | Build a query or read its results |
| `io.fengqudb.Q.Cond` | `public Cond not()` | Build a query or read its results |
| `io.fengqudb.Q.Cond` | `public Cond or(Cond other)` | Build a query or read its results |
| `io.fengqudb.Q.Prepared` | `public Rows run(Store db)` | Build a query or read its results |
| `io.fengqudb.Q.Prepared` | `public Rows run(Store.Transaction tx)` | Build a query or read its results |
| `io.fengqudb.Q.Row` | `public <T> T get(Agg<T> a)` | Read data or status |
| `io.fengqudb.Q.Row` | `public <T> T get(Col<T> c)` | Read data or status |
| `io.fengqudb.Q.Row` | `public byte[] key()` | Build a query or read its results |
| `io.fengqudb.Q.Rows` | `public byte[] cursor()` | Build a query or read its results |
| `io.fengqudb.Q.Rows` | `public Row get(int i)` | Read data or status |
| `io.fengqudb.Q.Rows` | `public Iterator<Row> iterator()` | Build a query or read its results |
| `io.fengqudb.Q.Rows` | `public int size()` | Build a query or read its results |
| `io.fengqudb.Select` | `public byte[] encode()` | Build a query or read its results |
| `io.fengqudb.Select` | `public Result execute(Store db)` | Build a query or read its results |
| `io.fengqudb.Select` | `public Result execute(Store.Transaction tx)` | Build a query or read its results |
| `io.fengqudb.Select` | `public Result explain(Store db)` | Inspect the query plan |
| `io.fengqudb.Select` | `public Result explain(Store.Transaction tx)` | Inspect the query plan |
| `io.fengqudb.Select` | `public Prepared prepare()` | Build a query or read its results |
| `io.fengqudb.Select.Column` | `public Column(Field f)` | Create an object |
| `io.fengqudb.Select.Column` | `public Column(Field f, Aggregate a)` | Create an object |
| `io.fengqudb.Select.Expr` | `public static Expr all(Expr... children)` | Build a query or read its results |
| `io.fengqudb.Select.Expr` | `public static Expr any(Expr... children)` | Build a query or read its results |
| `io.fengqudb.Select.Expr` | `public static Expr not(Expr child)` | Build a query or read its results |
| `io.fengqudb.Select.Field` | `public Field()` | Create an object |
| `io.fengqudb.Select.Field` | `public Field(int number, int type)` | Create an object |
| `io.fengqudb.Select.Field` | `public Field(int number, int type, int source)` | Create an object |
| `io.fengqudb.Select.Field` | `public Field any()` | Build a query or read its results |
| `io.fengqudb.Select.Field` | `public static Field column(int index)` | Build a query or read its results |
| `io.fengqudb.Select.Field` | `public Expr compare(Op op, Field right)` | Build a query or read its results |
| `io.fengqudb.Select.Field` | `public static Field key(int source)` | Build a query or read its results |
| `io.fengqudb.Select.Field` | `public static Field record(int source)` | Build a query or read its results |
| `io.fengqudb.Select.Field` | `public Expr test(Op op, Object... values)` | Build a query or read its results |
| `io.fengqudb.Select.Order` | `public Order(Field f)` | Create an object |
| `io.fengqudb.Select.Order` | `public Order(Field f, boolean desc)` | Create an object |
| `io.fengqudb.Select.Prepared` | `public Result execute(Store db, boolean explain)` | Build a query or read its results |
| `io.fengqudb.Select.Prepared` | `public Result execute(Store.Transaction tx, boolean explain)` | Build a query or read its results |
| `io.fengqudb.Select.Value` | `public static Value of(Object x)` | Build a query or read its results |
| `io.fengqudb.Select.Value` | `public String text()` | Build a query or read its results |
| `io.fengqudb.Select.Value` | `public static Value uintBits(long bits)` | Build a query or read its results |
| `io.fengqudb.Select.Value` | `public String unsignedText()` | Build a query or read its results |
| `io.fengqudb.Store` | `public CommandResult selectRaw(int operation, byte[] value)` | Build a query or read its results |
| `io.fengqudb.Store.Row` | `public Row(byte[] key, byte[] value, long version, long expiresAtMs)` | Create an object |

</details>

## 8. Indexes

```java
db.defineIndex("student-score".getBytes(StandardCharsets.UTF_8),
    "student/".getBytes(StandardCharsets.UTF_8), StudentFqCodec.Field_score, 5, 0);
```

5 means a signed ZigZag integer.

<details>
<summary>Functions and parameters (8)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public MaintenanceInfo defineCompoundIndex(byte[] name, byte[] prefix, IndexField[] fields, int flags)` | Define or use an index |
| `io.fengqudb.Store` | `public MaintenanceInfo defineCompoundIndex(byte[] name, byte[] prefix, IndexField[] fields, int flags, MaintenanceOptions options)` | Define or use an index |
| `io.fengqudb.Store` | `public MaintenanceInfo defineIndex(byte[] name, byte[] prefix, int field, int type, int flags)` | Define or use an index |
| `io.fengqudb.Store` | `public MaintenanceInfo defineIndex(byte[] name, byte[] prefix, int field, int type, int flags, MaintenanceOptions options)` | Define or use an index |
| `io.fengqudb.Store` | `public MaintenanceInfo dropIndex(byte[] name)` | Define or use an index |
| `io.fengqudb.Store` | `public MaintenanceInfo dropIndex(byte[] name, MaintenanceOptions options)` | Define or use an index |
| `io.fengqudb.Store` | `public CommandResult queryIndex(byte[] name, IndexQuery query, byte[] cursor, long limit)` | Define or use an index |
| `io.fengqudb.Store.IndexField` | `public IndexField(int field, int type)` | Create an object |

</details>

## 9. Critical writes such as payments: reliable confirmation

The default performance mode uses 50 ms / 1 MiB. Ordinary writes acknowledge in-memory visibility. Pass true for critical writes:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```java
db.put("payment/receipt".getBytes(StandardCharsets.UTF_8), "paid".getBytes(StandardCharsets.UTF_8), true);
db.sync();
```

Use commit(true) for transactions. submitPut returns a receipt that you must wait for or poll. On COMMIT_UNKNOWN, reconcile by business operation ID instead of charging again.

<details>
<summary>Functions and parameters (14)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public void configureWrite(WriteOptions p)` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public java.util.concurrent.CompletableFuture<Void> configureWriteAsync(WriteOptions p, java.util.concurrent.Executor executor)` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public void resetWriteOptions()` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public java.util.concurrent.CompletableFuture<Void> resetWriteOptionsAsync(java.util.concurrent.Executor executor)` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public CommitReceipt submitPut(byte[] key, byte[] value)` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public CommitReceipt submitPut(byte[] key, byte[] value, long expiresAtMs, boolean... forceReliable)` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public void sync()` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public WriteOptions writeOptions()` | Control durability or wait for a commit |
| `io.fengqudb.Store` | `public WriteState writeState()` | Control durability or wait for a commit |
| `io.fengqudb.Store.CommitReceipt` | `public long await()` | Control durability or wait for a commit |
| `io.fengqudb.Store.CommitReceipt` | `public long await(long timeoutMs)` | Control durability or wait for a commit |
| `io.fengqudb.Store.CommitReceipt` | `public void cancel()` | Control durability or wait for a commit |
| `io.fengqudb.Store.CommitReceipt` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.CommitReceipt` | `public Long poll()` | Control durability or wait for a commit |

</details>

## 10. Temporary data and expiry

Use the overload with expiresAtMs. `System.currentTimeMillis() + 300_000` expires after five minutes; 0 means no expiry.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public ExpiryInfo purgeExpired()` | Manage expiring data |
| `io.fengqudb.Store` | `public ExpiryInfo purgeExpired(ExpiryOptions options)` | Manage expiring data |

</details>

## 11. Frequent reads

```java
try (var key = db.prepareKey("student/7".getBytes(StandardCharsets.UTF_8))) {
    byte[] bytes = key.get();
}
```

Ordinary get returns an independent array. Close shared ByteBuffer leases and readers promptly; do not use derived views after closing them.

<details>
<summary>Functions and parameters (19)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public SharedValue getShared(byte[] key)` | Read data or status |
| `io.fengqudb.Store` | `public PreparedKey prepareKey(byte[] key)` | Reuse keys, buffers, or shared bytes |
| `io.fengqudb.Store` | `public PreparedReader prepareReader(byte[] key)` | Reuse keys, buffers, or shared bytes |
| `io.fengqudb.Store` | `public PreparedReader prepareReader(byte[][] keys)` | Reuse keys, buffers, or shared bytes |
| `io.fengqudb.Store.PreparedKey` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.PreparedKey` | `public byte[] get()` | Read data or status |
| `io.fengqudb.Store.PreparedKey` | `public Entry getEntry()` | Read data or status |
| `io.fengqudb.Store.PreparedKey` | `public long getInto(byte[] destination)` | Read data or status |
| `io.fengqudb.Store.PreparedKey` | `public long getIntoDirect(ByteBuffer destination)` | Read data or status |
| `io.fengqudb.Store.PreparedKey` | `public SharedValue getShared()` | Read data or status |
| `io.fengqudb.Store.PreparedReader` | `public synchronized void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.PreparedReader` | `public synchronized SharedBatch readShared()` | Read data or status |
| `io.fengqudb.Store.PreparedReader` | `public synchronized void release()` | Release an object or borrowed resource |
| `io.fengqudb.Store.SharedBatch` | `public synchronized void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.SharedBatch` | `public synchronized SharedValue get(int index)` | Read data or status |
| `io.fengqudb.Store.SharedBatch` | `public synchronized int size()` | Reuse keys, buffers, or shared bytes |
| `io.fengqudb.Store.SharedValue` | `public synchronized ByteBuffer buffer()` | Reuse keys, buffers, or shared bytes |
| `io.fengqudb.Store.SharedValue` | `public synchronized void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.SharedValue` | `public boolean isZeroCopy()` | Reuse keys, buffers, or shared bytes |

</details>

## 12. Consistent snapshots

```java
try (var snapshot = db.snapshot()) {
    byte[] value = snapshot.get("student/7".getBytes(StandardCharsets.UTF_8));
}
```

<details>
<summary>Functions and parameters (11)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Snapshot snapshot()` | Consistent reads or range iteration |
| `io.fengqudb.Store.Cursor` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.Cursor` | `public synchronized boolean hasNext()` | Consistent reads or range iteration |
| `io.fengqudb.Store.Cursor` | `public Iterator<Row> iterator()` | Consistent reads or range iteration |
| `io.fengqudb.Store.Cursor` | `public synchronized Row next()` | Consistent reads or range iteration |
| `io.fengqudb.Store.Snapshot` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.Snapshot` | `public byte[] get(byte[] key)` | Read data or status |
| `io.fengqudb.Store.Snapshot` | `public Entry[] getEntries(byte[][] keys)` | Read data or status |
| `io.fengqudb.Store.Snapshot` | `public byte[][] getMany(byte[][] keys)` | Read data or status |
| `io.fengqudb.Store.Snapshot` | `public Cursor scan()` | Consistent reads or range iteration |
| `io.fengqudb.Store.Snapshot` | `public Cursor scan(byte[] begin, byte[] end, boolean reverse)` | Consistent reads or range iteration |

</details>

## 13. Backup and restore

```java
db.backup("backups/full-001");
```

Use a new backup directory. Restore with restore/recoverChain after closing the database being replaced.

<details>
<summary>Functions and parameters (12)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public MaintenanceInfo backup(String destination)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public MaintenanceInfo backup(String destination, MaintenanceOptions options)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public MaintenanceInfo backupIncremental(String destination, long fromVersion)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public MaintenanceInfo backupIncremental(String destination, long fromVersion, MaintenanceOptions options)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public HistoryInfo historyInfo()` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public AtomicInfo holdHistory(byte[] name)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public AtomicInfo holdHistory(byte[] name, long fromVersion)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public static MaintenanceInfo recoverChain(String base, String[] archives, String destination, long targetVersion, long targetUnixMs)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public static MaintenanceInfo recoverChain(String base, String[] archives, String destination, long targetVersion, long targetUnixMs, MaintenanceOptions options)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public AtomicInfo releaseHistory(byte[] name)` | Release an object or borrowed resource |
| `io.fengqudb.Store` | `public static MaintenanceInfo restore(String source, String destination)` | Backup, restore, or history retention |
| `io.fengqudb.Store` | `public static MaintenanceInfo restore(String source, String destination, MaintenanceOptions options)` | Backup, restore, or history retention |

</details>

## 14. Hash, Set, List, and sorted sets

execute runs collection commands; hashGet reads a Hash field. Use them for atomic member operations, queues, and rankings.

<details>
<summary>Functions and parameters (3)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value, boolean... forceReliable)` | Native collection operation |
| `io.fengqudb.Store` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value, long argument, double score, long limit, int flags, boolean... forceReliable)` | Native collection operation |
| `io.fengqudb.Store` | `public byte[] hashGet(byte[] scope, byte[] key)` | Read data or status |

</details>

## 15. Cleanup and compaction

```java
db.purgeExpired();
db.compact();
db.verify();
```

<details>
<summary>Functions and parameters (9)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public void checkpoint()` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public MaintenanceInfo compact()` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public MaintenanceInfo compact(MaintenanceOptions options)` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public void configure()` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public void configure(RuntimeOptions options)` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public long[] info()` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public RuntimeInfo runtimeInfo()` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public MaintenanceInfo verify()` | Inspect status or maintain the database |
| `io.fengqudb.Store` | `public MaintenanceInfo verify(MaintenanceOptions options)` | Inspect status or maintain the database |

</details>

## 16. Commit changes

watch creates a consumer; ack after processing and close when done. Use it to update in-process application caches.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Watch watch()` | Consume changes or diagnose issues |
| `io.fengqudb.Store` | `public Watch watch(long fromVersion, byte[] prefix, WatchOptions options)` | Consume changes or diagnose issues |
| `io.fengqudb.Store.Watch` | `public void ack(long version)` | Consume changes or diagnose issues |
| `io.fengqudb.Store.Watch` | `public void cancel()` | Consume changes or diagnose issues |
| `io.fengqudb.Store.Watch` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store.Watch` | `public Changes next()` | Consume changes or diagnose issues |
| `io.fengqudb.Store.Watch` | `public Changes next(long timeoutMs)` | Consume changes or diagnose issues |

</details>

## 17. Entity encoding and decoding

StudentFqCodec.encode/decode accesses Student directly. The record's fqEncode preserves unknown fields; encodeInto(value, buffer, offset) reuses an array. Generation runs before Gradle compilation, using the native tool included in the SDK.

## Other utilities

<details>
<summary>Functions and parameters (13)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Store(String directory)` | Create an object |
| `io.fengqudb.Store` | `public Store(String directory, Options options)` | Create an object |
| `io.fengqudb.Store` | `public void cancelMaintenance()` | Utility operation |
| `io.fengqudb.Store` | `public void close()` | Release an object or borrowed resource |
| `io.fengqudb.Store` | `public static Store openAt(String root, String name)` | Utility operation |
| `io.fengqudb.Store` | `public static Store openAt(String root, String name, Options options)` | Utility operation |
| `io.fengqudb.Store` | `public static String pathAt(String root, String name)` | Utility operation |
| `io.fengqudb.Store` | `public PreparedReader prepareProjectReader(byte[][] keys, int[] fields)` | Utility operation |
| `io.fengqudb.Store` | `public byte[] project(byte[] key, int[] fields)` | Utility operation |
| `io.fengqudb.Store.DbException` | `public DbException(int code)` | Create an object |
| `io.fengqudb.Store.Int64FieldUpdate` | `public Int64FieldUpdate(byte[] key, long offset, long delta)` | Create an object |
| `io.fengqudb.Store.Int64FieldUpdate` | `public Int64FieldUpdate(byte[] key, long offset, long delta, long minimum, long maximum)` | Create an object |
| `io.fengqudb.Store.PreparedBatch` | `public void close()` | Release an object or borrowed resource |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>io.fengqudb</summary>

| Declaration |
| --- |
| `public class Q` |
| `public class Select` |
| `public class Store implements AutoCloseable` |

</details>

<details>
<summary>io.fengqudb.Q</summary>

| Declaration |
| --- |
| `public class Agg<T> implements Item` |
| `public class Builder` |
| `public class Col<T> implements Item` |
| `public class Cond` |
| `public class Prepared` |
| `public class Row` |
| `public class Rows implements Iterable<Row>` |
| `public class Sort` |

</details>

<details>
<summary>io.fengqudb.Q.Col</summary>

| Declaration |
| --- |
| `public final Select.Field field` |

</details>

<details>
<summary>io.fengqudb.Q.Rows</summary>

| Declaration |
| --- |
| `public final Select.Result raw` |

</details>

<details>
<summary>io.fengqudb.Select</summary>

| Declaration |
| --- |
| `public class Access` |
| `public enum Aggregate` |
| `public class Column` |
| `public class Expr` |
| `public class Field` |
| `public class Join` |
| `public enum JoinKind` |
| `public enum Op` |
| `public class Order` |
| `public class Prepared` |
| `public class Result` |
| `public class Row` |
| `public class Stats` |
| `public class Value` |
| `public byte[] after` |
| `public boolean allowScan` |
| `public List<Column> columns` |
| `public boolean countTotal` |
| `public boolean distinct` |
| `public List<Field> group` |
| `public Expr having` |
| `public byte[] indexHint` |
| `public List<Join> joins` |
| `public long limit` |
| `public long maxCandidates` |
| `public long maxWork` |
| `public long memoryBytes` |
| `public long offset` |
| `public List<Order> order` |
| `public byte[] prefix` |
| `public long timeoutMs` |
| `public Expr where` |

</details>

<details>
<summary>io.fengqudb.Select.Access</summary>

| Declaration |
| --- |
| `public boolean covering` |
| `public String index` |
| `public boolean needsScan` |
| `public boolean ordered` |
| `public String reason` |
| `public long sampledCandidates` |
| `public long source` |
| `public String strategy` |

</details>

<details>
<summary>io.fengqudb.Select.Aggregate</summary>

| Declaration |
| --- |
| `public static final Aggregate AVG` |
| `public static final Aggregate COUNT` |
| `public static final Aggregate COUNT_ALL` |
| `public static final Aggregate MAX` |
| `public static final Aggregate MIN` |
| `public static final Aggregate NONE` |
| `public static final Aggregate SUM` |

</details>

<details>
<summary>io.fengqudb.Select.Column</summary>

| Declaration |
| --- |
| `public Aggregate aggregate` |
| `public boolean distinct` |
| `public Field field` |
| `public String name` |

</details>

<details>
<summary>io.fengqudb.Select.Expr</summary>

| Declaration |
| --- |
| `public List<Expr> children` |
| `public Field field` |
| `public Op op` |
| `public Field right` |
| `public List<Value> values` |

</details>

<details>
<summary>io.fengqudb.Select.Field</summary>

| Declaration |
| --- |
| `public int[] path` |
| `public boolean repeated` |
| `public int source` |
| `public int type` |

</details>

<details>
<summary>io.fengqudb.Select.Join</summary>

| Declaration |
| --- |
| `public JoinKind kind` |
| `public List<Field> left` |
| `public Expr on` |
| `public byte[] prefix` |
| `public List<Field> right` |

</details>

<details>
<summary>io.fengqudb.Select.JoinKind</summary>

| Declaration |
| --- |
| `public static final JoinKind ANTI` |
| `public static final JoinKind INNER` |
| `public static final JoinKind LEFT` |
| `public static final JoinKind SEMI` |

</details>

<details>
<summary>io.fengqudb.Select.Op</summary>

| Declaration |
| --- |
| `public static final Op AND` |
| `public static final Op CONTAINS` |
| `public static final Op ENDS_WITH` |
| `public static final Op EQ` |
| `public static final Op EXISTS` |
| `public static final Op FALSE` |
| `public static final Op FUZZY` |
| `public static final Op GE` |
| `public static final Op GT` |
| `public static final Op ICONTAINS` |
| `public static final Op ILIKE` |
| `public static final Op IN` |
| `public static final Op IREGEX` |
| `public static final Op IS_NULL` |
| `public static final Op LE` |
| `public static final Op LIKE` |
| `public static final Op LT` |
| `public static final Op NE` |
| `public static final Op NOT` |
| `public static final Op OR` |
| `public static final Op REGEX` |
| `public static final Op STARTS_WITH` |
| `public static final Op TRUE` |

</details>

<details>
<summary>io.fengqudb.Select.Order</summary>

| Declaration |
| --- |
| `public boolean descending` |
| `public Field field` |
| `public boolean nullsFirst` |

</details>

<details>
<summary>io.fengqudb.Select.Result</summary>

| Declaration |
| --- |
| `public List<Access> access` |
| `public byte[] cursor` |
| `public List<Row> rows` |
| `public Stats stats` |

</details>

<details>
<summary>io.fengqudb.Select.Row</summary>

| Declaration |
| --- |
| `public byte[] key` |
| `public List<Value> values` |

</details>

<details>
<summary>io.fengqudb.Select.Stats</summary>

| Declaration |
| --- |
| `public long candidates` |
| `public boolean complete` |
| `public long joinProbes` |
| `public long matched` |
| `public long payloadReads` |
| `public long peakMemory` |
| `public long resultTotal` |
| `public long work` |

</details>

<details>
<summary>io.fengqudb.Select.Value</summary>

| Declaration |
| --- |
| `public long bits` |
| `public byte[] bytes` |
| `public int kind` |
| `public double real` |

</details>

<details>
<summary>io.fengqudb.Store</summary>

| Declaration |
| --- |
| `public class AtomicInfo` |
| `public class Change` |
| `public class Changes` |
| `public class CommandResult` |
| `public class CommitReceipt implements AutoCloseable` |
| `public class Cursor implements AutoCloseable, Iterable<Row>, Iterator<Row>` |
| `public class DbException extends RuntimeException` |
| `public class Entry` |
| `public class ExpiryInfo` |
| `public class ExpiryOptions` |
| `public class FeatureRow` |
| `public class HistoryInfo` |
| `public class IndexField` |
| `public class IndexQuery` |
| `public class Int64FieldUpdate` |
| `public class MaintenanceIO` |
| `public class MaintenanceInfo` |
| `public class MaintenanceOptions` |
| `public class Operation` |
| `public class Options` |
| `public class PreparedBatch implements AutoCloseable` |
| `public class PreparedKey implements AutoCloseable` |
| `public class PreparedReader implements AutoCloseable` |
| `public class Put` |
| `public class Row` |
| `public class RuntimeInfo` |
| `public class RuntimeOptions` |
| `public class SharedBatch implements AutoCloseable` |
| `public class SharedValue implements AutoCloseable` |
| `public class Snapshot implements AutoCloseable` |
| `public class Transaction implements AutoCloseable` |
| `public class Watch implements AutoCloseable` |
| `public class WatchOptions` |
| `public enum WriteMode` |
| `public class WriteOptions` |
| `public class WriteState` |

</details>

<details>
<summary>io.fengqudb.Store.AtomicInfo</summary>

| Declaration |
| --- |
| `public boolean changed` |
| `public boolean matched` |
| `public long observedVersion` |
| `public long value` |
| `public long version` |

</details>

<details>
<summary>io.fengqudb.Store.Change</summary>

| Declaration |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] key` |
| `public final int kind` |
| `public final byte[] value` |

</details>

<details>
<summary>io.fengqudb.Store.Changes</summary>

| Declaration |
| --- |
| `public final long durableVersion` |
| `public final int flags` |
| `public final Change[] rows` |
| `public final long unixMs` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.CommandResult</summary>

| Declaration |
| --- |
| `public boolean changed` |
| `public long count` |
| `public byte[] cursor` |
| `public final List<FeatureRow> rows` |
| `public byte[] value` |
| `public long version` |

</details>

<details>
<summary>io.fengqudb.Store.DbException</summary>

| Declaration |
| --- |
| `public final int code` |

</details>

<details>
<summary>io.fengqudb.Store.Entry</summary>

| Declaration |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] value` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.ExpiryInfo</summary>

| Declaration |
| --- |
| `public long cutoffMs` |
| `public long more` |
| `public long removedKeys` |
| `public long removedValueBytes` |
| `public long version` |
| `public long visitedPages` |

</details>

<details>
<summary>io.fengqudb.Store.ExpiryOptions</summary>

| Declaration |
| --- |
| `public long maxBatchBytes` |
| `public int maxKeys` |
| `public long timeoutMs` |

</details>

<details>
<summary>io.fengqudb.Store.FeatureRow</summary>

| Declaration |
| --- |
| `public byte[] key` |
| `public double score` |
| `public byte[] value` |

</details>

<details>
<summary>io.fengqudb.Store.HistoryInfo</summary>

| Declaration |
| --- |
| `public final long activePins` |
| `public final long floor` |
| `public final long oldestPin` |
| `public final long retainedFileBytes` |
| `public final long retainedGenerations` |
| `public final long totalPins` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.IndexField</summary>

| Declaration |
| --- |
| `public final int field` |
| `public final int type` |

</details>

<details>
<summary>io.fengqudb.Store.IndexQuery</summary>

| Declaration |
| --- |
| `public byte[] equal` |
| `public int[] fields` |
| `public boolean includeValues` |
| `public byte[] lower` |
| `public boolean lowerExclusive` |
| `public boolean reverse` |
| `public byte[] upper` |
| `public boolean upperInclusive` |

</details>

<details>
<summary>io.fengqudb.Store.Int64FieldUpdate</summary>

| Declaration |
| --- |
| `public final long delta` |
| `public final byte[] key` |
| `public final long maximum` |
| `public final long minimum` |
| `public final long offset` |

</details>

<details>
<summary>io.fengqudb.Store.MaintenanceIO</summary>

| Declaration |
| --- |
| `public long readBytes` |
| `public long syncCalls` |
| `public long writeBytes` |

</details>

<details>
<summary>io.fengqudb.Store.MaintenanceInfo</summary>

| Declaration |
| --- |
| `public long catchupRounds` |
| `public long copiedKeys` |
| `public long finalVersion` |
| `public long outputFileBytes` |
| `public long snapshotVersion` |
| `public long sourceFileBytes` |
| `public long storedKeys` |
| `public long storedValueBytes` |

</details>

<details>
<summary>io.fengqudb.Store.MaintenanceOptions</summary>

| Declaration |
| --- |
| `public MaintenanceIO ioStats` |
| `public long maxBarrierBytes` |
| `public int maxCatchupRounds` |
| `public long maxReadBytes` |
| `public long maxWriteBytes` |
| `public long timeoutMs` |

</details>

<details>
<summary>io.fengqudb.Store.Operation</summary>

| Declaration |
| --- |
| `public static final int COLLECTION_CLEAR` |
| `public static final int COLLECTION_SIZE` |
| `public static final int HASH_DELETE` |
| `public static final int HASH_GET` |
| `public static final int HASH_SCAN` |
| `public static final int HASH_SET` |
| `public static final int INDEX_LOOKUP` |
| `public static final int INDEX_QUERY` |
| `public static final int LIST_GET` |
| `public static final int LIST_POP_LEFT` |
| `public static final int LIST_POP_RIGHT` |
| `public static final int LIST_PUSH_LEFT` |
| `public static final int LIST_PUSH_RIGHT` |
| `public static final int LIST_RANGE` |
| `public static final int LIST_SET` |
| `public static final int RECORD_PROJECT` |
| `public static final int SET_ADD` |
| `public static final int SET_CONTAINS` |
| `public static final int SET_REMOVE` |
| `public static final int SET_SCAN` |
| `public static final int SORTED_ADD` |
| `public static final int SORTED_RANGE` |
| `public static final int SORTED_REMOVE` |
| `public static final int SORTED_SCORE` |

</details>

<details>
<summary>io.fengqudb.Store.Options</summary>

| Declaration |
| --- |
| `public long allTransactionBytes` |
| `public boolean buffered` |
| `public long cacheBytes` |
| `public boolean create` |
| `public long deltaBytes` |
| `public long maxSnapshots` |
| `public long pinnedBytes` |
| `public boolean segmented` |
| `public long transactionBytes` |
| `public long writerBytes` |

</details>

<details>
<summary>io.fengqudb.Store.Put</summary>

| Declaration |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] key` |
| `public final byte[] value` |

</details>

<details>
<summary>io.fengqudb.Store.Row</summary>

| Declaration |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] key` |
| `public final byte[] value` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.RuntimeInfo</summary>

| Declaration |
| --- |
| `public long compactions` |
| `public long expiryBatches` |
| `public long groupFlushes` |
| `public long groupTransactions` |
| `public long lastStatus` |
| `public long running` |

</details>

<details>
<summary>io.fengqudb.Store.RuntimeOptions</summary>

| Declaration |
| --- |
| `public boolean background` |
| `public long compactGrowthBytes` |
| `public long compactMinBytes` |
| `public int expiryKeys` |
| `public int garbagePercent` |
| `public int groupDelayUs` |
| `public int groupMax` |
| `public int intervalMs` |
| `public long jobMaxReadBytes` |
| `public long jobMaxWriteBytes` |
| `public long jobTimeoutMs` |

</details>

<details>
<summary>io.fengqudb.Store.SharedValue</summary>

| Declaration |
| --- |
| `public final long expiresAtMs` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.WatchOptions</summary>

| Declaration |
| --- |
| `public boolean includeSystem` |
| `public long maxBatchBytes` |
| `public int maxScanCommits` |
| `public boolean metadataOnly` |

</details>

<details>
<summary>io.fengqudb.Store.WriteMode</summary>

| Declaration |
| --- |
| `public static final WriteMode PERFORMANCE` |
| `public static final WriteMode RELIABLE` |

</details>

<details>
<summary>io.fengqudb.Store.WriteOptions</summary>

| Declaration |
| --- |
| `public WriteMode mode` |
| `public long periodMs` |
| `public long thresholdBytes` |

</details>

<details>
<summary>io.fengqudb.Store.WriteState</summary>

| Declaration |
| --- |
| `public final long durableVersion` |
| `public final long lastStatus` |
| `public final long pendingBytes` |
| `public final long version` |

</details>
