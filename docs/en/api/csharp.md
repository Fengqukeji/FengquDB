# C# API and examples

[Getting started](../languages/csharp.md) · [简体中文](../../api/csharp.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read: find a student or a score

The examples use the shared entry point: `var db = AppDb.Main;`.

```csharp
Student? student = db.GetEntity<Student>(7); // Returns null if the record is missing
int score = db.Get<int>("score");             // Throws if the key is missing
bool found = db.TryGet<int>("score", out int value);
```

Read one field without constructing the entire student object:

```csharp
using var reader = db.PrepareReader(Student.MakeKey(7));
int grade = reader.Read(rows => Student.ReadScore(rows[0].Value));
```

<details>
<summary>Functions and parameters (8)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public T Get<T>(string key)` | Read data or status |
| `FengquDB.Store` | `public byte[]? Get(byte[] key)` | Read data or status |
| `FengquDB.Store` | `public T? GetEntity<T>(long id) where T : class, IFqEntity<T>, new()` | Read data or status |
| `FengquDB.Store` | `public T? GetEntity<T>(string id) where T : class, IFqEntity<T>, new()` | Read data or status |
| `FengquDB.Store` | `public T? GetEntity<T>(ulong id) where T : class, IFqEntity<T>, new()` | Read data or status |
| `FengquDB.Store` | `public StoreEntry?[] GetEntries(byte[][] keys)` | Read data or status |
| `FengquDB.Store` | `public StoreEntry? GetEntry(byte[] key)` | Read data or status |
| `FengquDB.Store` | `public bool TryGet<T>(string key, out T value)` | Read data or status |

</details>

## 2. Insert or save: entities, numbers, and strings

Put overwrites the previous value at the same key. Student is defined in the getting-started guide.

```csharp
db.Put(new Student { Id=7, Name="Alice", Score=95 });
db.Put("score", 95);
db.Put("name", "Alice");
db.Put("enabled", true);
```

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public ulong Put<T>(T entity, ulong expiresAtMs = 0, bool forceReliable = false) where T : class, IFqEntity<T>` | Save or set data |
| `FengquDB.Store` | `public ulong Put<T>(string key, T value, ulong expiresAtMs = 0, bool forceReliable = false)` | Save or set data |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Put(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value, ulong expiresAtMs = 0, bool forceReliable = false)` | Save or set data |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Put(byte[] key, byte[] value, ulong expiresAtMs = 0, bool forceReliable = false)` | Save or set data |

</details>

## 3. Update: scores, conditional writes, and atomic counters

```csharp
var student = db.GetEntity<Student>(7);
if (student != null) { student.Score = 96; db.Put(student); }
```

Use an atomic operation when multiple threads increment a counter, rather than reading and overwriting the same old value:

```csharp
byte[] key = System.Text.Encoding.UTF8.GetBytes("login-count");
var changed = db.AddInt64(key, 1);
```

AddInt64 uses a dedicated eight-byte integer format; do not call it on a key saved with `Put("score", 95)`. `CompareExchange` updates by version, `PutIfAbsent` writes only when missing, and `Patch` changes raw binary ranges managed by your application.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo AddInt64(byte[] key, long delta, long minimum = long.MinValue, long maximum = long.MaxValue, ulong timeoutMs = 0, bool forceReliable = false)` | Conditional update or partial modification |
| `FengquDB.Store` | `public StoreAtomicInfo AddInt64Fields(StoreI64FieldUpdate[] items, ulong timeoutMs = 0, bool forceReliable = false)` | Conditional update or partial modification |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo CompareExchange(byte[] key, ulong expectedVersion, byte[] value, ulong expiresAtMs = 0, ulong timeoutMs = 0, bool forceReliable = false)` | Conditional update or partial modification |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo DeleteIfVersion(byte[] key, ulong expectedVersion, ulong timeoutMs = 0, bool forceReliable = false)` | Delete a record or member |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Patch(byte[] key, ulong offset, ulong remove, byte[] insert, bool forceReliable = false)` | Conditional update or partial modification |
| `FengquDB.Store` | `public StoreAtomicInfo PutIfAbsent(byte[] key, byte[] value, ulong expiresAtMs = 0, ulong timeoutMs = 0, bool forceReliable = false)` | Save or set data |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo PutIfChanged(byte[] key, byte[] value, ulong expiresAtMs = 0, ulong timeoutMs = 0, bool forceReliable = false)` | Save or set data |

</details>

## 4. Delete records

```csharp
db.Delete(Student.MakeKey(7));
db.Delete(System.Text.Encoding.UTF8.GetBytes("score"));
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Delete(byte[] key, bool forceReliable = false)` | Delete a record or member |

</details>

## 5. Batch: read and write multiple records

```csharp
byte[][] keys = { Student.MakeKey(7), Student.MakeKey(8) };
var values = db.GetMany(keys);
```

Save multiple entities in one transaction to avoid committing them individually.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public ulong DeleteMany(byte[][] keys, bool forceReliable = false)` | Delete a record or member |
| `FengquDB.Store` | `public byte[]?[] GetMany(byte[][] keys)` | Read data or status |
| `FengquDB.Store` | `public StorePreparedBatch PrepareBatch(StorePut[] items)` | Process a batch |
| `FengquDB.Store` | `public ulong PutBatch(StorePreparedBatch batch, bool forceReliable = false)` | Save or set data |
| `FengquDB.Store` | `public ulong PutMany(StorePut[] items, bool forceReliable = false)` | Save or set data |

</details>

## 6. Transactions: changes that must succeed together

```csharp
using var tx = db.Begin();
tx.Put("score", 100);
tx.Put("level", 2);
tx.Commit();
```

Disposing without Commit discards the changes. On CONFLICT, create a new transaction, read again, and recompute; do not simply replay stale values.

<details>
<summary>Functions and parameters (32)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreTransaction Begin(ulong timeoutMs = 0)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public void Cancel()` | Transaction operation |
| `FengquDB.StoreTransaction` | `public ulong Commit(bool forceReliable = false)` | Commit a transaction or inspect its result |
| `FengquDB.StoreTransaction` | `public unsafe void Delete(byte[] key)` | Delete a record or member |
| `FengquDB.StoreTransaction` | `public void DeleteMany(byte[][] keys)` | Delete a record or member |
| `FengquDB.StoreTransaction` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StoreTransaction` | `public unsafe StoreCommandResult Execute(StoreOperation operation, byte[] scope, byte[]? key = null, byte[]? value = null, long argument = 0, double score = 0, ulong limit = 256, uint flags = 0)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public unsafe void Expire(byte[] key, ulong expiresAtMs)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public StoreQueryResult Explain(StoreQuery query)` | Inspect the query plan |
| `FengquDB.StoreTransaction` | `public T Get<T>(string key)` | Read data or status |
| `FengquDB.StoreTransaction` | `public byte[]? Get(byte[] key)` | Read data or status |
| `FengquDB.StoreTransaction` | `public T? GetEntity<T>(long id) where T : class, IFqEntity<T>, new()` | Read data or status |
| `FengquDB.StoreTransaction` | `public T? GetEntity<T>(string id) where T : class, IFqEntity<T>, new()` | Read data or status |
| `FengquDB.StoreTransaction` | `public T? GetEntity<T>(ulong id) where T : class, IFqEntity<T>, new()` | Read data or status |
| `FengquDB.StoreTransaction` | `public StoreEntry?[] GetEntries(byte[][] keys)` | Read data or status |
| `FengquDB.StoreTransaction` | `public byte[]?[] GetMany(byte[][] keys)` | Read data or status |
| `FengquDB.StoreTransaction` | `public unsafe void Patch(byte[] key, ulong offset, ulong remove, byte[] insert)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public byte[] Project(byte[] key, uint[] fields)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public unsafe void Put(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value, ulong expiresAtMs = 0)` | Save or set data |
| `FengquDB.StoreTransaction` | `public unsafe void Put(byte[] key, byte[] value, ulong expiresAtMs = 0)` | Save or set data |
| `FengquDB.StoreTransaction` | `public void Put<T>(T entity, ulong expiresAtMs = 0) where T : class, IFqEntity<T>` | Save or set data |
| `FengquDB.StoreTransaction` | `public void Put<T>(string key, T value, ulong expiresAtMs = 0)` | Save or set data |
| `FengquDB.StoreTransaction` | `public void PutBatch(StorePreparedBatch batch)` | Save or set data |
| `FengquDB.StoreTransaction` | `public void PutMany(StorePut[] items)` | Save or set data |
| `FengquDB.StoreTransaction` | `public StoreCommandResult QueryIndex(byte[] name, StoreIndexQuery? query = null, byte[]? cursor = null, ulong limit = 256)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public void Release(ulong id)` | Release an object or borrowed resource |
| `FengquDB.StoreTransaction` | `public void RollbackTo(ulong id)` | Roll back to a savepoint |
| `FengquDB.StoreTransaction` | `public ulong Savepoint()` | Create or manage a transaction savepoint |
| `FengquDB.StoreTransaction` | `public IEnumerable<StoreRow> Scan(byte[]? begin = null, byte[]? end = null, bool reverse = false)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public StoreQueryResult Select(StoreQuery query)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public StoreCommit Submit(bool forceReliable = false)` | Transaction operation |
| `FengquDB.StoreTransaction` | `public bool TryGet<T>(string key, out T value)` | Read data or status |

</details>

## 7. Filtering, sorting, rankings, and aggregation

```csharp
var score = Student.Cols.Score;
var name = Student.Cols.Name;
var rows = Q.From("student/").Where(score >= 90L)
    .Select(name, score).OrderBy(score.Desc()).Limit(10).Run(db);
foreach (var row in rows) Console.WriteLine($"{row.Get(name)} {row.Get(score)}");
```

Combine `QCond` with `&`, `|`, and `!`. `GroupBy/Having` aggregates groups, and `Join` matches records by field. `Explain` shows index use; `Memory/TimeoutMs` limits resources for large queries. All query methods and overloads are listed below.

<details>
<summary>Functions and parameters (110)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.PreparedQuery` | `public StoreQueryResult Execute(Store db,bool explain=false)` | Build a query or read its results |
| `FengquDB.PreparedQuery` | `public StoreQueryResult Execute(StoreTransaction tx,bool explain=false)` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<double> Avg<T>(QCol<T> c)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<bool> Bool(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<ulong> Count()` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<ulong> Count<T>(QCol<T> c)` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<ulong> CountDistinct<T>(QCol<T> c)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<double> F32(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<double> F64(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<ulong> Fixed32(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<ulong> Fixed64(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QueryBuilder From(byte[] prefix)` | Build a query or read its results |
| `FengquDB.Q` | `public static QueryBuilder From(string prefix)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<long> I64(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<string> Key(uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<T> Max<T>(QCol<T> c)` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<T> Min<T>(QCol<T> c)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<T> Nested<T>(uint type, uint source, params uint[] path)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<long> S64(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<long> SFixed32(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<long> SFixed64(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QAgg<T> Sum<T>(QCol<T> c)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<string> Text(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.Q` | `public static QCol<ulong> U64(uint n, uint source = 0)` | Build a query or read its results |
| `FengquDB.QAgg` | `public QSort Asc(bool nullsFirst = true)` | Build a query or read its results |
| `FengquDB.QAgg` | `public QSort Desc(bool nullsFirst = true)` | Build a query or read its results |
| `FengquDB.QAgg` | `public override bool Equals(object? o)` | Build a query or read its results |
| `FengquDB.QAgg` | `public override int GetHashCode()` | Read data or status |
| `FengquDB.QAgg` | `public static QCond operator !=(QAgg<T> a, T v)` | Build a query or read its results |
| `FengquDB.QAgg` | `public static QCond operator <(QAgg<T> a, T v)` | Build a query or read its results |
| `FengquDB.QAgg` | `public static QCond operator <=(QAgg<T> a, T v)` | Build a query or read its results |
| `FengquDB.QAgg` | `public static QCond operator ==(QAgg<T> a, T v)` | Build a query or read its results |
| `FengquDB.QAgg` | `public static QCond operator >(QAgg<T> a, T v)` | Build a query or read its results |
| `FengquDB.QAgg` | `public static QCond operator >=(QAgg<T> a, T v)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCol<T> Any()` | Build a query or read its results |
| `FengquDB.QCol` | `public QSort Asc(bool nullsFirst = true)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond Between(T low, T high)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond Contains(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public QSort Desc(bool nullsFirst = true)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond EndsWith(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public override bool Equals(object? o)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond Exists()` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond Fuzzy(string s, ulong distance)` | Build a query or read its results |
| `FengquDB.QCol` | `public override int GetHashCode()` | Read data or status |
| `FengquDB.QCol` | `public QCond IContains(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond ILike(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond In(params T[] values)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond IsNull()` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond Like(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCol(QueryField field)` | Create an object |
| `FengquDB.QCol` | `public QCond Regex(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public QCond StartsWith(string s)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator !=(QCol<T> a, QCol<T> b)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator !=(QCol<T> c, T v)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator <(QCol<T> a, QCol<T> b)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator <(QCol<T> c, T v)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator <=(QCol<T> a, QCol<T> b)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator <=(QCol<T> c, T v)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator ==(QCol<T> a, QCol<T> b)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator ==(QCol<T> c, T v)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator >(QCol<T> a, QCol<T> b)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator >(QCol<T> c, T v)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator >=(QCol<T> a, QCol<T> b)` | Build a query or read its results |
| `FengquDB.QCol` | `public static QCond operator >=(QCol<T> c, T v)` | Build a query or read its results |
| `FengquDB.QCond` | `public static QCond operator !(QCond a)` | Build a query or read its results |
| `FengquDB.QCond` | `public static QCond operator &(QCond a, QCond b)` | Build a query or read its results |
| `FengquDB.QCond` | `public static bool operator false(QCond _)` | Build a query or read its results |
| `FengquDB.QCond` | `public static bool operator true(QCond _)` | Build a query or read its results |
| `FengquDB.QCond` | `public static QCond operator \|(QCond a, QCond b)` | Build a query or read its results |
| `FengquDB.QRow` | `public T Get<T>(QAgg<T> a)` | Read data or status |
| `FengquDB.QRow` | `public T Get<T>(QCol<T> c)` | Read data or status |
| `FengquDB.QRow` | `public bool TryGet<T>(QCol<T> c, out T value)` | Read data or status |
| `FengquDB.QRows` | `public IEnumerator<QRow> GetEnumerator()` | Read data or status |
| `FengquDB.QueryBuilder` | `public QueryBuilder After(byte[] cursor)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder AllowScan(bool on)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public StoreQuery Build()` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder CountTotal(bool on = true)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Distinct(bool on = true)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder GroupBy(params IQColumn[] items)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Having(QCond c)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Index(string name)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Join<T>(string prefix, QCol<T> left, QCol<T> right, QueryJoinKind kind = QueryJoinKind.Inner)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Limit(ulong n)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder MaxCandidates(ulong n)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Memory(ulong bytes)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Offset(ulong n)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder OrderBy(params IQOrder[] items)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QPrepared Prepare()` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QRows Run(Store db)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QRows Run(StoreTransaction tx)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Select(params IQColumn[] items)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder TimeoutMs(ulong ms)` | Build a query or read its results |
| `FengquDB.QueryBuilder` | `public QueryBuilder Where(QCond c)` | Build a query or read its results |
| `FengquDB.QueryColumn` | `public QueryColumn(QueryField field,QueryAggregate aggregate=QueryAggregate.None)` | Create an object |
| `FengquDB.QueryExpr` | `public static QueryExpr All(params QueryExpr[] children)` | Build a query or read its results |
| `FengquDB.QueryExpr` | `public static QueryExpr Any(params QueryExpr[] children)` | Build a query or read its results |
| `FengquDB.QueryExpr` | `public static QueryExpr Not(QueryExpr child)` | Build a query or read its results |
| `FengquDB.QueryField` | `public QueryField Any()` | Build a query or read its results |
| `FengquDB.QueryField` | `public static QueryField Column(uint index)` | Build a query or read its results |
| `FengquDB.QueryField` | `public QueryExpr Compare(QueryOp op,QueryField right)` | Build a query or read its results |
| `FengquDB.QueryField` | `public static QueryField Key(uint source=0)` | Build a query or read its results |
| `FengquDB.QueryField` | `public QueryField()` | Create an object |
| `FengquDB.QueryField` | `public QueryField(uint number,uint type,uint source=0)` | Create an object |
| `FengquDB.QueryField` | `public static QueryField Record(uint source=0)` | Build a query or read its results |
| `FengquDB.QueryField` | `public QueryExpr Test(QueryOp op,params object?[] values)` | Build a query or read its results |
| `FengquDB.QueryOrder` | `public QueryOrder(QueryField field,bool descending=false)` | Create an object |
| `FengquDB.Store` | `public StoreQueryResult Explain(StoreQuery query)` | Inspect the query plan |
| `FengquDB.Store` | `public StoreQueryResult Select(StoreQuery query)` | Build a query or read its results |
| `FengquDB.StoreQuery` | `public byte[] Encode()` | Build a query or read its results |
| `FengquDB.StoreQuery` | `public PreparedQuery Prepare()` | Build a query or read its results |

</details>

## 8. Indexes: find records by field

```csharp
byte[] index = System.Text.Encoding.UTF8.GetBytes("student-score");
byte[] prefix = System.Text.Encoding.UTF8.GetBytes("student/");
db.DefineIndex(index, prefix, 3, 5); // Field 3: signed ZigZag integer
```

Indexes narrow the candidates before the remaining filters run. Configure uniqueness, required fields, and compound-field constraints through index options.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreMaintenanceInfo DefineCompoundIndex(byte[] name, byte[] prefix, StoreIndexField[] fields, uint flags = 0, StoreMaintenanceOptions? options = null)` | Define or use an index |
| `FengquDB.Store` | `public unsafe StoreMaintenanceInfo DefineIndex(byte[] name, byte[] prefix, uint field, uint type, uint flags = 0, StoreMaintenanceOptions? options = null)` | Define or use an index |
| `FengquDB.Store` | `public unsafe StoreMaintenanceInfo DropIndex(byte[] name, StoreMaintenanceOptions? options = null)` | Define or use an index |
| `FengquDB.Store` | `public StoreCommandResult QueryIndex(byte[] name, StoreIndexQuery? query = null, byte[]? cursor = null, ulong limit = 256)` | Define or use an index |
| `FengquDB.StoreIndexField` | `public StoreIndexField(uint field, uint type)` | Create an object |

</details>

## 9. Reliable writes: persist payment records before proceeding

The default performance mode uses 50 ms / 1 MiB and returns after applying writes in memory. For critical writes:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```csharp
db.Put("payment/receipt", "paid", forceReliable: true);
using (var tx = db.Begin()) { tx.Put("paid", true); tx.Commit(forceReliable: true); }
db.Sync(); // Wait for previous writes to reach durable storage
```

To require reliable confirmation for the whole database:

```csharp
db.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
```

To avoid blocking a UI thread, obtain a receipt from SubmitPut, then await WaitAsync. COMMIT_UNKNOWN means the outcome is uncertain: reconcile by order ID instead of charging again.

<details>
<summary>Functions and parameters (13)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public void ConfigureWrite(StoreWriteOptions options)` | Control durability or wait for a commit |
| `FengquDB.Store` | `public Task ConfigureWriteAsync(StoreWriteOptions options)` | Control durability or wait for a commit |
| `FengquDB.Store` | `public void ResetWriteOptions()` | Control durability or wait for a commit |
| `FengquDB.Store` | `public Task ResetWriteOptionsAsync()` | Control durability or wait for a commit |
| `FengquDB.Store` | `public unsafe StoreCommit SubmitPut(byte[] key, byte[] value, ulong expiresAtMs = 0, bool forceReliable = false)` | Control durability or wait for a commit |
| `FengquDB.Store` | `public void Sync()` | Control durability or wait for a commit |
| `FengquDB.Store` | `public StoreWriteOptions WriteOptions()` | Control durability or wait for a commit |
| `FengquDB.Store` | `public StoreWriteState WriteState()` | Control durability or wait for a commit |
| `FengquDB.StoreCommit` | `public void Cancel()` | Control durability or wait for a commit |
| `FengquDB.StoreCommit` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StoreCommit` | `public ulong? Poll()` | Control durability or wait for a commit |
| `FengquDB.StoreCommit` | `public ulong Wait(ulong timeoutMs = 0)` | Control durability or wait for a commit |
| `FengquDB.StoreCommit` | `public async Task<ulong> WaitAsync(int pollIntervalMs = 5)` | Control durability or wait for a commit |

</details>

## 10. Expiring data: verification codes and temporary state

```csharp
ulong expires = (ulong)DateTimeOffset.UtcNow.AddMinutes(5).ToUnixTimeMilliseconds();
db.Put("verification", "1234", expiresAtMs: expires);
```

Time is an absolute Unix timestamp in milliseconds; 0 means no expiry. Expired records are hidden from reads, and cleanup or compaction later reclaims their space.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreExpiryInfo PurgeExpired(StoreExpiryOptions? options = null)` | Manage expiring data |

</details>

## 11. Hot-path reads: reuse keys and read buffers

```csharp
using var scoreReader = db.PrepareValue<int>("score");
int score = scoreReader.Read();
using var entityReader = db.PrepareEntity<Student>(7);
Student student = entityReader.Read();
```

Ordinary Get returns an independently owned value. Dispose shared reads and readers promptly. Do not retain a reader's byte view across the next Read.

<details>
<summary>Functions and parameters (19)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareEntity<T>(long id) where T : class, IFqEntity<T>, new()` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareEntity<T>(string id) where T : class, IFqEntity<T>, new()` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareEntity<T>(ulong id) where T : class, IFqEntity<T>, new()` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public unsafe StorePreparedKey PrepareKey(byte[] key)` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public StoreReader PrepareReader(byte[] key)` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public StoreReader PrepareReader(byte[][] keys)` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareValue<T>(string key)` | Reuse keys, buffers, or shared bytes |
| `FengquDB.Store` | `public unsafe TResult ReadShared<TResult>(byte[] key, StoreValueAction<TResult> action)` | Read data or status |
| `FengquDB.StorePreparedKey` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StorePreparedKey` | `public byte[]? Get()` | Read data or status |
| `FengquDB.StorePreparedKey` | `public StoreEntry? GetEntry()` | Read data or status |
| `FengquDB.StorePreparedKey` | `public unsafe ulong? GetInto(byte[] destination)` | Read data or status |
| `FengquDB.StoreReader` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StoreReader` | `public TResult Read<TResult>(StoreReadAction<TResult> action)` | Read data or status |
| `FengquDB.StoreReader` | `public void Release()` | Release an object or borrowed resource |
| `FengquDB.StoreTypedReader` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StoreTypedReader` | `public T Read()` | Read data or status |
| `FengquDB.StoreTypedReader` | `public void Release()` | Release an object or borrowed resource |
| `FengquDB.StoreTypedReader` | `public bool TryRead(out T value)` | Reuse keys, buffers, or shared bytes |

</details>

## 12. Consistent reads: use the same point in time

```csharp
using var snapshot = db.Snapshot();
var student = snapshot.GetEntity<Student>(7);
int score = snapshot.Get<int>("score");
```

<details>
<summary>Functions and parameters (6)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreSnapshot Snapshot()` | Consistent reads or range iteration |
| `FengquDB.StoreSnapshot` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StoreSnapshot` | `public byte[]? Get(byte[] key)` | Read data or status |
| `FengquDB.StoreSnapshot` | `public StoreEntry?[] GetEntries(byte[][] keys)` | Read data or status |
| `FengquDB.StoreSnapshot` | `public byte[]?[] GetMany(byte[][] keys)` | Read data or status |
| `FengquDB.StoreSnapshot` | `public IEnumerable<StoreRow> Scan(byte[]? begin = null, byte[]? end = null, bool reverse = false)` | Consistent reads or range iteration |

</details>

## 13. Backup and restore: save state before an update

```csharp
db.Backup("backups/full-001"); // Use a new destination directory
```

Close the database being replaced and call Restore/RecoverChain. Do not copy a database directory while it is being written.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreMaintenanceInfo Backup(string destination, StoreMaintenanceOptions? options = null)` | Backup, restore, or history retention |
| `FengquDB.Store` | `public StoreMaintenanceInfo BackupIncremental(string destination, ulong fromVersion, StoreMaintenanceOptions? options = null)` | Backup, restore, or history retention |
| `FengquDB.Store` | `public StoreHistoryInfo HistoryInfo()` | Backup, restore, or history retention |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo HoldHistory(byte[] name, ulong fromVersion = ulong.MaxValue, ulong timeoutMs = 0)` | Backup, restore, or history retention |
| `FengquDB.Store` | `public static StoreMaintenanceInfo RecoverChain(string source, string[] archives, string destination, ulong targetVersion = 0, ulong targetUnixMs = 0, StoreMaintenanceOptions? options = null)` | Backup, restore, or history retention |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo ReleaseHistory(byte[] name, ulong timeoutMs = 0)` | Release an object or borrowed resource |
| `FengquDB.Store` | `public static StoreMaintenanceInfo Restore(string source, string destination, StoreMaintenanceOptions? options = null)` | Backup, restore, or history retention |

</details>

## 14. Collections: Hash, Set, List, and sorted sets

Execute accepts collection commands; HashGet reads a Hash field. Use them for inventory members, deduplication, and rankings without repeatedly loading and rewriting an entire collection. See the types and overloads in this section for command parameters.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreCommandResult Execute(StoreOperation operation, byte[] scope, byte[]? key = null, byte[]? value = null, long argument = 0, double score = 0, ulong limit = 256, uint flags = 0, bool forceReliable = false)` | Native collection operation |
| `FengquDB.Store` | `public unsafe byte[]? HashGet(byte[] scope, byte[] key)` | Read data or status |

</details>

## 15. Compaction, cleanup, and verification

```csharp
db.PurgeExpired();
db.Compact();
db.Verify();
```

Run these during maintenance periods. Use options to limit work per call on large databases.

<details>
<summary>Functions and parameters (6)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public void Checkpoint()` | Inspect status or maintain the database |
| `FengquDB.Store` | `public StoreMaintenanceInfo Compact(StoreMaintenanceOptions? options = null)` | Inspect status or maintain the database |
| `FengquDB.Store` | `public void Configure(StoreRuntimeOptions? options = null)` | Inspect status or maintain the database |
| `FengquDB.Store` | `public StoreInfo Info()` | Inspect status or maintain the database |
| `FengquDB.Store` | `public StoreRuntimeInfo RuntimeInfo()` | Inspect status or maintain the database |
| `FengquDB.Store` | `public StoreMaintenanceInfo Verify(StoreMaintenanceOptions? options = null)` | Inspect status or maintain the database |

</details>

## 16. Change notifications: consume in-process commit changes

Watch creates a consumer; Ack acknowledges the processed watermark, and Dispose closes it. Use it to update application caches or trigger background tasks.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreWatch Watch(ulong fromVersion = ulong.MaxValue, byte[]? prefix = null, StoreWatchOptions? options = null)` | Consume changes or diagnose issues |
| `FengquDB.StoreWatch` | `public void Ack(ulong version)` | Consume changes or diagnose issues |
| `FengquDB.StoreWatch` | `public void Cancel()` | Consume changes or diagnose issues |
| `FengquDB.StoreWatch` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StoreWatch` | `public unsafe StoreChanges? Next(ulong timeoutMs = 0)` | Consume changes or diagnose issues |

</details>

## 17. Encoding, decoding, and manual generation

Normal compilation generates the entity's Decode, EncodeTo, MakeKey, field readers, and Cols without runtime reflection. Keep existing field numbers unchanged.

See this language's getting-started guide for tool placement and double-click instructions. The native tool reads `// fqdb:entity prefix=student/ key=Id` and generates a separate codec, whose calls differ from compiler-generated entity methods. Use the manual fallback like this, without runtime reflection:

```csharp
byte[] key = StudentFqCodec.Key(student);
db.Put(key, StudentFqCodec.Encode(student));
var row = StudentFqCodec.Decode(db.Get(key)!);
row.Value.Score++;
db.Put(row.Key(), row.Encode());
```

Use the decoded record's Encode when rewriting it to preserve unknown fields. A reused encoding buffer must not overlap the entity's byte fields.

<details>
<summary>Functions and parameters (32)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.Entities.FqEntityAttribute` | `public FqEntityAttribute(string keyPrefix)` | Create an object |
| `FengquDB.Entities.FqEntityStorage` | `public static void ValidateSize(int keyBufferBytes, int valueBytes)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqGeneratedEntityAttribute` | `public FqGeneratedEntityAttribute(string schema)` | Create an object |
| `FengquDB.Entities.FqWire` | `public static int BytesSize(int length)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static byte[] CopyUnknown(ReadOnlySpan<byte> source, int length, Func<uint, bool> known)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int KeySize(ReadOnlySpan<byte> prefix, long id)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int KeySize(ReadOnlySpan<byte> prefix, string id)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int KeySize(ReadOnlySpan<byte> prefix, ulong id)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static bool ReadBool(ReadOnlySpan<byte> input, ref int position)` | Read data or status |
| `FengquDB.Entities.FqWire` | `public static ReadOnlySpan<byte> ReadBytes(ReadOnlySpan<byte> input, ref int position)` | Read data or status |
| `FengquDB.Entities.FqWire` | `public static double ReadDouble(ReadOnlySpan<byte> input, ref int position)` | Read data or status |
| `FengquDB.Entities.FqWire` | `public static float ReadSingle(ReadOnlySpan<byte> input, ref int position)` | Read data or status |
| `FengquDB.Entities.FqWire` | `public static uint ReadTag(ReadOnlySpan<byte> input, ref int position)` | Read data or status |
| `FengquDB.Entities.FqWire` | `public static ulong ReadVarint(ReadOnlySpan<byte> input, ref int position)` | Read data or status |
| `FengquDB.Entities.FqWire` | `public static void Skip(ReadOnlySpan<byte> input, ref int position, uint tag)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int StringSize(string value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static long UnZigZag(ulong value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int VarintSize(ulong value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteBytes(Span<byte> output, ReadOnlySpan<byte> value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteDouble(Span<byte> output, double value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, long id)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, string id)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, ulong id)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteSingle(Span<byte> output, float value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteString(Span<byte> output, string value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static int WriteVarint(Span<byte> output, ulong value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.FqWire` | `public static ulong ZigZag(long value)` | Encode or decode entities or bytes |
| `FengquDB.Entities.IFqEntity` | `int EncodeTo(Span<byte> destination)` | Encode or decode entities or bytes |
| `FengquDB.Entities.IFqEntity` | `int EncodedSize()` | Encode or decode entities or bytes |
| `FengquDB.Entities.IFqEntity` | `T ReadEntity(ReadOnlySpan<byte> source)` | Read data or status |
| `FengquDB.Entities.IFqEntity` | `ulong WriteTo(global::FengquDB.Store store, ulong expiresAtMs, bool forceReliable)` | Encode or decode entities or bytes |
| `FengquDB.Entities.IFqEntity` | `void WriteTo(global::FengquDB.StoreTransaction transaction, ulong expiresAtMs)` | Encode or decode entities or bytes |

</details>

## Other utilities

<details>
<summary>Functions and parameters (16)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `FengquDB.DatabaseException` | `public DatabaseException(int code) : base($"FengquDB {code}: {Marshal.PtrToStringAnsi(Native.fqdb_status_message(code))}")` | Create an object |
| `FengquDB.Entities.FqFieldAttribute` | `public FqFieldAttribute(int number)` | Create an object |
| `FengquDB.IQOrder` | `QueryOrder Resolve(IReadOnlyList<QueryColumn> columns)` | Utility operation |
| `FengquDB.QPrepared` | `public QRows Run(Store db)` | Utility operation |
| `FengquDB.QPrepared` | `public QRows Run(StoreTransaction tx)` | Utility operation |
| `FengquDB.QSort` | `public QueryOrder Resolve(IReadOnlyList<QueryColumn> columns)` | Utility operation |
| `FengquDB.Store` | `public void CancelMaintenance()` | Utility operation |
| `FengquDB.Store` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.Store` | `public static Store OpenAt(string root, string name, StoreOptions? options = null)` | Utility operation |
| `FengquDB.Store` | `public static string PathAt(string root, string name)` | Utility operation |
| `FengquDB.Store` | `public StoreReader PrepareProjectReader(byte[][] keys, uint[] fields)` | Utility operation |
| `FengquDB.Store` | `public unsafe byte[] Project(byte[] key, uint[] fields)` | Utility operation |
| `FengquDB.Store` | `public Store(string directory, StoreOptions? options = null)` | Create an object |
| `FengquDB.StoreI64FieldUpdate` | `public StoreI64FieldUpdate(byte[] key, ulong offset, long delta, long minimum = long.MinValue, long maximum = long.MaxValue)` | Create an object |
| `FengquDB.StorePreparedBatch` | `public void Dispose()` | Release an object or borrowed resource |
| `FengquDB.StorePut` | `public StorePut(byte[] key, byte[] value, ulong expiresAtMs = 0)` | Create an object |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>FengquDB</summary>

| Declaration |
| --- |
| `public class BufferTooSmallException` |
| `public class DatabaseException` |
| `public interface IQColumn` |
| `public interface IQOrder` |
| `public class PreparedQuery` |
| `public class Q` |
| `public class QAgg<T>` |
| `public class QCol<T>` |
| `public class QCond` |
| `public class QPrepared` |
| `public struct QRow` |
| `public class QRows` |
| `public class QSort` |
| `public class QueryAccess` |
| `public enum QueryAggregate` |
| `public class QueryBuilder` |
| `public class QueryColumn` |
| `public class QueryExpr` |
| `public class QueryField` |
| `public class QueryJoin` |
| `public enum QueryJoinKind` |
| `public enum QueryOp` |
| `public class QueryOrder` |
| `public class QueryRow` |
| `public class QueryStats` |
| `public class Store` |
| `public struct StoreAtomicInfo` |
| `public class StoreChange` |
| `public class StoreChanges` |
| `public class StoreCommandResult` |
| `public class StoreCommit` |
| `public class StoreEntry` |
| `public struct StoreExpiryInfo` |
| `public class StoreExpiryOptions` |
| `public class StoreFeatureRow` |
| `public struct StoreHistoryInfo` |
| `public class StoreI64FieldUpdate` |
| `public struct StoreIndexField` |
| `public class StoreIndexQuery` |
| `public struct StoreInfo` |
| `public class StoreMaintenanceIO` |
| `public struct StoreMaintenanceInfo` |
| `public class StoreMaintenanceOptions` |
| `public enum StoreOperation` |
| `public class StoreOptions` |
| `public class StorePreparedBatch` |
| `public class StorePreparedKey` |
| `public class StorePut` |
| `public class StoreQuery` |
| `public class StoreQueryResult` |
| `public delegate TResult StoreReadAction<TResult>(StoreReadBatch batch);` |
| `public struct StoreReadBatch` |
| `public class StoreReader` |
| `public class StoreRow` |
| `public struct StoreRuntimeInfo` |
| `public class StoreRuntimeOptions` |
| `public struct StoreSharedEntry` |
| `public class StoreSnapshot` |
| `public class StoreTransaction` |
| `public class StoreTypedReader<T>` |
| `public delegate TResult StoreValueAction<TResult>(StoreSharedEntry entry);` |
| `public class StoreWatch` |
| `public class StoreWatchOptions` |
| `public enum StoreWriteMode` |
| `public class StoreWriteOptions` |
| `public struct StoreWriteState` |

</details>

<details>
<summary>FengquDB.BufferTooSmallException</summary>

| Declaration |
| --- |
| `ulong RequiredSize { get; }` |

</details>

<details>
<summary>FengquDB.DatabaseException</summary>

| Declaration |
| --- |
| `int Code { get; }` |

</details>

<details>
<summary>FengquDB.Entities</summary>

| Declaration |
| --- |
| `public class FqEntityAttribute` |
| `public enum FqEntityKeyKind` |
| `public class FqEntityStorage` |
| `public class FqFieldAttribute` |
| `public class FqGeneratedEntityAttribute` |
| `public class FqKeyAttribute` |
| `public class FqWire` |
| `public interface IFqEntity<T>` |

</details>

<details>
<summary>FengquDB.Entities.FqEntityAttribute</summary>

| Declaration |
| --- |
| `string KeyPrefix { get; }` |

</details>

<details>
<summary>FengquDB.Entities.FqEntityKeyKind</summary>

| Declaration |
| --- |
| `Signed` |
| `Text` |
| `Unsigned` |

</details>

<details>
<summary>FengquDB.Entities.FqFieldAttribute</summary>

| Declaration |
| --- |
| `int Number { get; }` |

</details>

<details>
<summary>FengquDB.Entities.FqGeneratedEntityAttribute</summary>

| Declaration |
| --- |
| `string Schema { get; }` |

</details>

<details>
<summary>FengquDB.Entities.FqWire</summary>

| Declaration |
| --- |
| `Encoding Utf8 = new UTF8Encoding(false, true)` |

</details>

<details>
<summary>FengquDB.Entities.IFqEntity</summary>

| Declaration |
| --- |
| `FqEntityKeyKind KeyKind { get; }` |
| `ReadOnlySpan<byte> KeyPrefix { get; }` |

</details>

<details>
<summary>FengquDB.IQColumn</summary>

| Declaration |
| --- |
| `QueryColumn Column { get; }` |

</details>

<details>
<summary>FengquDB.QAgg</summary>

| Declaration |
| --- |
| `QueryColumn Column { get; }` |

</details>

<details>
<summary>FengquDB.QCol</summary>

| Declaration |
| --- |
| `QueryColumn Column { get; }` |
| `QueryField Field` |

</details>

<details>
<summary>FengquDB.QRow</summary>

| Declaration |
| --- |
| `byte[] Key { get; }` |

</details>

<details>
<summary>FengquDB.QRows</summary>

| Declaration |
| --- |
| `int Count { get; }` |
| `byte[] Cursor { get; }` |
| `StoreQueryResult Raw { get; }` |
| `QueryStats Stats { get; }` |
| `QRow this[int i]` |

</details>

<details>
<summary>FengquDB.QueryAccess</summary>

| Declaration |
| --- |
| `bool Covering,Ordered,NeedsScan` |
| `ulong SampledCandidates` |
| `uint Source` |
| `string Strategy="",Index="",Reason=""` |

</details>

<details>
<summary>FengquDB.QueryAggregate</summary>

| Declaration |
| --- |
| `Avg` |
| `Count` |
| `CountAll` |
| `Max` |
| `Min` |
| `None` |
| `Sum` |

</details>

<details>
<summary>FengquDB.QueryColumn</summary>

| Declaration |
| --- |
| `QueryAggregate Aggregate` |
| `bool Distinct` |
| `QueryField Field` |
| `string Name=""` |

</details>

<details>
<summary>FengquDB.QueryExpr</summary>

| Declaration |
| --- |
| `QueryExpr[] Children=Array.Empty<QueryExpr>()` |
| `QueryField Field=new QueryField()` |
| `QueryOp Op` |
| `QueryField? Right` |
| `object?[] Values=Array.Empty<object?>()` |

</details>

<details>
<summary>FengquDB.QueryField</summary>

| Declaration |
| --- |
| `uint[] Path=Array.Empty<uint>()` |
| `bool Repeated` |
| `uint Source, Type=4` |

</details>

<details>
<summary>FengquDB.QueryJoin</summary>

| Declaration |
| --- |
| `QueryJoinKind Kind` |
| `QueryField[] Left=Array.Empty<QueryField>(),Right=Array.Empty<QueryField>()` |
| `QueryExpr On=new QueryExpr()` |
| `byte[] Prefix=Array.Empty<byte>()` |

</details>

<details>
<summary>FengquDB.QueryJoinKind</summary>

| Declaration |
| --- |
| `Anti` |
| `Inner` |
| `Left` |
| `Semi` |

</details>

<details>
<summary>FengquDB.QueryOp</summary>

| Declaration |
| --- |
| `And` |
| `Contains` |
| `EndsWith` |
| `Eq` |
| `Exists` |
| `False` |
| `Fuzzy` |
| `Ge` |
| `Gt` |
| `IContains` |
| `ILike` |
| `IRegex` |
| `In` |
| `IsNull` |
| `Le` |
| `Like` |
| `Lt` |
| `Ne` |
| `Not` |
| `Or` |
| `Regex` |
| `StartsWith` |
| `True` |

</details>

<details>
<summary>FengquDB.QueryOrder</summary>

| Declaration |
| --- |
| `bool Descending,NullsFirst=true` |
| `QueryField Field` |

</details>

<details>
<summary>FengquDB.QueryRow</summary>

| Declaration |
| --- |
| `byte[] Key=Array.Empty<byte>()` |
| `List<object?> Values=new List<object?>()` |

</details>

<details>
<summary>FengquDB.QueryStats</summary>

| Declaration |
| --- |
| `ulong Candidates,Matched,ResultTotal,Work,PeakMemory,PayloadReads,JoinProbes` |
| `bool Complete` |

</details>

<details>
<summary>FengquDB.StoreAtomicInfo</summary>

| Declaration |
| --- |
| `bool Changed { get; }` |
| `bool Matched { get; }` |
| `long Value` |
| `ulong Version, ObservedVersion` |

</details>

<details>
<summary>FengquDB.StoreChange</summary>

| Declaration |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Key { get; }` |
| `uint Kind { get; }` |
| `byte[]? Value { get; }` |

</details>

<details>
<summary>FengquDB.StoreChanges</summary>

| Declaration |
| --- |
| `ulong DurableVersion { get; }` |
| `uint Flags { get; }` |
| `StoreChange[] Rows { get; }` |
| `ulong UnixMs { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreCommandResult</summary>

| Declaration |
| --- |
| `bool Changed` |
| `byte[] Cursor = Array.Empty<byte>()` |
| `List<StoreFeatureRow> Rows { get; }` |
| `byte[]? Value` |
| `ulong Version, Count` |

</details>

<details>
<summary>FengquDB.StoreEntry</summary>

| Declaration |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Value { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreExpiryInfo</summary>

| Declaration |
| --- |
| `ulong Version, CutoffMs, RemovedKeys, RemovedValueBytes, VisitedPages, More` |

</details>

<details>
<summary>FengquDB.StoreExpiryOptions</summary>

| Declaration |
| --- |
| `ulong MaxBatchBytes { get; set; }` |
| `uint MaxKeys { get; set; }` |
| `ulong TimeoutMs { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreFeatureRow</summary>

| Declaration |
| --- |
| `byte[] Key = Array.Empty<byte>(), Value = Array.Empty<byte>()` |
| `double Score` |

</details>

<details>
<summary>FengquDB.StoreHistoryInfo</summary>

| Declaration |
| --- |
| `ulong Version, Floor, OldestPin, ActivePins, TotalPins, RetainedGenerations, RetainedFileBytes` |

</details>

<details>
<summary>FengquDB.StoreI64FieldUpdate</summary>

| Declaration |
| --- |
| `long Delta { get; }` |
| `byte[] Key { get; }` |
| `long Maximum { get; }` |
| `long Minimum { get; }` |
| `ulong Offset { get; }` |

</details>

<details>
<summary>FengquDB.StoreIndexField</summary>

| Declaration |
| --- |
| `uint Field, Type` |

</details>

<details>
<summary>FengquDB.StoreIndexQuery</summary>

| Declaration |
| --- |
| `byte[] Equal = Array.Empty<byte>()` |
| `uint[] Fields = Array.Empty<uint>()` |
| `byte[]? Lower, Upper` |
| `bool LowerExclusive, UpperInclusive, Reverse, IncludeValues` |

</details>

<details>
<summary>FengquDB.StoreInfo</summary>

| Declaration |
| --- |
| `ulong DeltaBytes, CacheBytes, PinnedBytes, TransactionBytes, RecoveredTransactions, RepairedBytes` |
| `ulong Version, DurableVersion, FileBytes, IndexedKeys, StoredKeys, StoredValueBytes` |

</details>

<details>
<summary>FengquDB.StoreMaintenanceIO</summary>

| Declaration |
| --- |
| `ulong ReadBytes, WriteBytes, SyncCalls` |

</details>

<details>
<summary>FengquDB.StoreMaintenanceInfo</summary>

| Declaration |
| --- |
| `ulong SnapshotVersion, FinalVersion, SourceFileBytes, OutputFileBytes` |
| `ulong StoredKeys, StoredValueBytes, CopiedKeys, CatchupRounds` |

</details>

<details>
<summary>FengquDB.StoreMaintenanceOptions</summary>

| Declaration |
| --- |
| `StoreMaintenanceIO? IOStats { get; set; }` |
| `ulong MaxBarrierBytes { get; set; }` |
| `uint MaxCatchupRounds { get; set; }` |
| `ulong MaxReadBytes { get; set; }` |
| `ulong MaxWriteBytes { get; set; }` |
| `ulong TimeoutMs { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreOperation</summary>

| Declaration |
| --- |
| `CollectionClear = 41` |
| `CollectionSize = 40` |
| `HashDelete = 3` |
| `HashGet = 1` |
| `HashScan = 4` |
| `HashSet = 2` |
| `IndexLookup = 50` |
| `IndexQuery = 51` |
| `ListGet = 24` |
| `ListPopLeft = 22` |
| `ListPopRight = 23` |
| `ListPushLeft = 20` |
| `ListPushRight = 21` |
| `ListRange = 26` |
| `ListSet = 25` |
| `RecordProject = 52` |
| `SetAdd = 11` |
| `SetContains = 10` |
| `SetRemove = 12` |
| `SetScan = 13` |
| `SortedAdd = 30` |
| `SortedRange = 33` |
| `SortedRemove = 31` |
| `SortedScore = 32` |

</details>

<details>
<summary>FengquDB.StoreOptions</summary>

| Declaration |
| --- |
| `ulong AllTransactionBytes { get; set; }` |
| `bool Buffered { get; set; }` |
| `ulong CacheBytes { get; set; }` |
| `bool Create { get; set; }` |
| `ulong DeltaBytes { get; set; }` |
| `ulong MaxSnapshots { get; set; }` |
| `ulong PinnedBytes { get; set; }` |
| `bool Segmented { get; set; }` |
| `ulong TransactionBytes { get; set; }` |
| `ulong WriterBytes { get; set; }` |

</details>

<details>
<summary>FengquDB.StorePut</summary>

| Declaration |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Key { get; }` |
| `byte[] Value { get; }` |

</details>

<details>
<summary>FengquDB.StoreQuery</summary>

| Declaration |
| --- |
| `bool AllowScan=true,CountTotal,Distinct` |
| `QueryColumn[] Columns=Array.Empty<QueryColumn>()` |
| `QueryField[] Group=Array.Empty<QueryField>()` |
| `QueryJoin[] Joins=Array.Empty<QueryJoin>()` |
| `ulong Limit=100,Offset,MemoryBytes=16UL<<20,MaxCandidates=1000000,MaxWork=100000000,TimeoutMs=1000` |
| `QueryOrder[] Order=Array.Empty<QueryOrder>()` |
| `byte[] Prefix=Array.Empty<byte>(),After=Array.Empty<byte>(),IndexHint=Array.Empty<byte>()` |
| `QueryExpr Where=new QueryExpr(),Having=new QueryExpr()` |

</details>

<details>
<summary>FengquDB.StoreQueryResult</summary>

| Declaration |
| --- |
| `List<QueryAccess> Access=new List<QueryAccess>()` |
| `byte[] Cursor=Array.Empty<byte>()` |
| `List<QueryRow> Rows=new List<QueryRow>()` |
| `QueryStats Stats=new QueryStats()` |

</details>

<details>
<summary>FengquDB.StoreReadBatch</summary>

| Declaration |
| --- |
| `int Count { get; }` |
| `StoreSharedEntry this[int index]` |

</details>

<details>
<summary>FengquDB.StoreRow</summary>

| Declaration |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Key { get; }` |
| `byte[] Value { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreRuntimeInfo</summary>

| Declaration |
| --- |
| `ulong Running, LastStatus, ExpiryBatches, Compactions, GroupFlushes, GroupTransactions` |

</details>

<details>
<summary>FengquDB.StoreRuntimeOptions</summary>

| Declaration |
| --- |
| `bool Background { get; set; }` |
| `ulong CompactGrowthBytes { get; set; }` |
| `ulong CompactMinBytes { get; set; }` |
| `uint ExpiryKeys { get; set; }` |
| `uint GarbagePercent { get; set; }` |
| `uint GroupDelayUs { get; set; }` |
| `uint GroupMax { get; set; }` |
| `uint IntervalMs { get; set; }` |
| `ulong JobMaxReadBytes { get; set; }` |
| `ulong JobMaxWriteBytes { get; set; }` |
| `ulong JobTimeoutMs { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreSharedEntry</summary>

| Declaration |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `bool Found { get; }` |
| `ReadOnlySpan<byte> Value { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreWatchOptions</summary>

| Declaration |
| --- |
| `bool IncludeSystem { get; set; }` |
| `ulong MaxBatchBytes { get; set; }` |
| `uint MaxScanCommits { get; set; }` |
| `bool MetadataOnly { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreWriteMode</summary>

| Declaration |
| --- |
| `Performance = 1` |
| `Reliable = 0` |

</details>

<details>
<summary>FengquDB.StoreWriteOptions</summary>

| Declaration |
| --- |
| `StoreWriteMode Mode { get; set; }` |
| `uint PeriodMs { get; set; }` |
| `ulong ThresholdBytes { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreWriteState</summary>

| Declaration |
| --- |
| `ulong Version, DurableVersion, PendingBytes, LastStatus` |

</details>
