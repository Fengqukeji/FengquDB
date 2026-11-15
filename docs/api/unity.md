# Unity API 与调用例子

[接入文档](../languages/unity.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取：找一名学生、读取积分

本页示例中的 db 来自统一入口：`var db = GameDb.Main;`。

```csharp
Student? student = db.GetEntity<Student>(7); // 没有这条记录时返回 null
int score = db.Get<int>("score");             // 缺失时抛异常
bool found = db.TryGet<int>("score", out int value);
```

只查某个字段，不必创建完整学生对象：

```csharp
using var reader = db.PrepareReader(Student.MakeKey(7));
int grade = reader.Read(rows => Student.ReadScore(rows[0].Value));
```

<details>
<summary>函数与参数（8项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public T Get<T>(string key)` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public byte[]? Get(byte[] key)` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public T? GetEntity<T>(long id) where T : class, IFqEntity<T>, new()` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public T? GetEntity<T>(string id) where T : class, IFqEntity<T>, new()` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public T? GetEntity<T>(ulong id) where T : class, IFqEntity<T>, new()` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public StoreEntry?[] GetEntries(byte[][] keys)` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public StoreEntry? GetEntry(byte[] key)` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public bool TryGet<T>(string key, out T value)` | 读取对应的数据或状态 |

</details>

## 2. 新增或保存：保存档案、单个数字和字符串

同一个键再次 Put 会覆盖原值。下例的 Student 定义见接入文档。

```csharp
db.Put(new Student { Id=7, Name="张三", Score=95 });
db.Put("score", 95);
db.Put("name", "张三");
db.Put("enabled", true);
```

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public ulong Put<T>(T entity, ulong expiresAtMs = 0, bool forceReliable = false) where T : class, IFqEntity<T>` | 保存或设置数据 |
| `FengquDB.Store` | `public ulong Put<T>(string key, T value, ulong expiresAtMs = 0, bool forceReliable = false)` | 保存或设置数据 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Put(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value, ulong expiresAtMs = 0, bool forceReliable = false)` | 保存或设置数据 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Put(byte[] key, byte[] value, ulong expiresAtMs = 0, bool forceReliable = false)` | 保存或设置数据 |

</details>

## 3. 修改：更新成绩、条件写入、原子计数

```csharp
var student = db.GetEntity<Student>(7);
if (student != null) { student.Score = 96; db.Put(student); }
```

多个线程同时加计数时用原子接口，避免各自读旧值再覆盖：

```csharp
byte[] key = System.Text.Encoding.UTF8.GetBytes("login-count");
var changed = db.AddInt64(key, 1);
```

AddInt64 使用专用八字节整数格式；不要对 `Put("score", 95)` 保存的键调用它。`CompareExchange` 按版本更新，`PutIfAbsent` 仅在不存在时写入，`Patch` 适合业务自己管理的原始二进制局部修改。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo AddInt64(byte[] key, long delta, long minimum = long.MinValue, long maximum = long.MaxValue, ulong timeoutMs = 0, bool forceReliable = false)` | 条件更新或局部修改 |
| `FengquDB.Store` | `public StoreAtomicInfo AddInt64Fields(StoreI64FieldUpdate[] items, ulong timeoutMs = 0, bool forceReliable = false)` | 条件更新或局部修改 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo CompareExchange(byte[] key, ulong expectedVersion, byte[] value, ulong expiresAtMs = 0, ulong timeoutMs = 0, bool forceReliable = false)` | 条件更新或局部修改 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo DeleteIfVersion(byte[] key, ulong expectedVersion, ulong timeoutMs = 0, bool forceReliable = false)` | 删除记录或成员 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Patch(byte[] key, ulong offset, ulong remove, byte[] insert, bool forceReliable = false)` | 条件更新或局部修改 |
| `FengquDB.Store` | `public StoreAtomicInfo PutIfAbsent(byte[] key, byte[] value, ulong expiresAtMs = 0, ulong timeoutMs = 0, bool forceReliable = false)` | 保存或设置数据 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo PutIfChanged(byte[] key, byte[] value, ulong expiresAtMs = 0, ulong timeoutMs = 0, bool forceReliable = false)` | 保存或设置数据 |

</details>

## 4. 删除：删除记录

```csharp
db.Delete(Student.MakeKey(7));
db.Delete(System.Text.Encoding.UTF8.GetBytes("score"));
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo Delete(byte[] key, bool forceReliable = false)` | 删除记录或成员 |

</details>

## 5. 批量：一组档案一起读写

```csharp
byte[][] keys = { Student.MakeKey(7), Student.MakeKey(8) };
var values = db.GetMany(keys);
```

保存多条实体可放进同一事务，避免逐笔提交。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public ulong DeleteMany(byte[][] keys, bool forceReliable = false)` | 删除记录或成员 |
| `FengquDB.Store` | `public byte[]?[] GetMany(byte[][] keys)` | 读取对应的数据或状态 |
| `FengquDB.Store` | `public StorePreparedBatch PrepareBatch(StorePut[] items)` | 批量处理 |
| `FengquDB.Store` | `public ulong PutBatch(StorePreparedBatch batch, bool forceReliable = false)` | 保存或设置数据 |
| `FengquDB.Store` | `public ulong PutMany(StorePut[] items, bool forceReliable = false)` | 保存或设置数据 |

</details>

## 6. 事务：多项修改必须一起成功

```csharp
using var tx = db.Begin();
tx.Put("score", 100);
tx.Put("level", 2);
tx.Commit();
```

未 Commit 就 Dispose 会丢弃修改。遇到 CONFLICT 时，重新创建事务、读取并计算；不要只重放旧值。

<details>
<summary>函数与参数（32项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreTransaction Begin(ulong timeoutMs = 0)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public void Cancel()` | 事务操作 |
| `FengquDB.StoreTransaction` | `public ulong Commit(bool forceReliable = false)` | 提交事务或查看提交结果 |
| `FengquDB.StoreTransaction` | `public unsafe void Delete(byte[] key)` | 删除记录或成员 |
| `FengquDB.StoreTransaction` | `public void DeleteMany(byte[][] keys)` | 删除记录或成员 |
| `FengquDB.StoreTransaction` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StoreTransaction` | `public unsafe StoreCommandResult Execute(StoreOperation operation, byte[] scope, byte[]? key = null, byte[]? value = null, long argument = 0, double score = 0, ulong limit = 256, uint flags = 0)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public unsafe void Expire(byte[] key, ulong expiresAtMs)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public StoreQueryResult Explain(StoreQuery query)` | 查看查询计划 |
| `FengquDB.StoreTransaction` | `public T Get<T>(string key)` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public byte[]? Get(byte[] key)` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public T? GetEntity<T>(long id) where T : class, IFqEntity<T>, new()` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public T? GetEntity<T>(string id) where T : class, IFqEntity<T>, new()` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public T? GetEntity<T>(ulong id) where T : class, IFqEntity<T>, new()` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public StoreEntry?[] GetEntries(byte[][] keys)` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public byte[]?[] GetMany(byte[][] keys)` | 读取对应的数据或状态 |
| `FengquDB.StoreTransaction` | `public unsafe void Patch(byte[] key, ulong offset, ulong remove, byte[] insert)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public byte[] Project(byte[] key, uint[] fields)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public unsafe void Put(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value, ulong expiresAtMs = 0)` | 保存或设置数据 |
| `FengquDB.StoreTransaction` | `public unsafe void Put(byte[] key, byte[] value, ulong expiresAtMs = 0)` | 保存或设置数据 |
| `FengquDB.StoreTransaction` | `public void Put<T>(T entity, ulong expiresAtMs = 0) where T : class, IFqEntity<T>` | 保存或设置数据 |
| `FengquDB.StoreTransaction` | `public void Put<T>(string key, T value, ulong expiresAtMs = 0)` | 保存或设置数据 |
| `FengquDB.StoreTransaction` | `public void PutBatch(StorePreparedBatch batch)` | 保存或设置数据 |
| `FengquDB.StoreTransaction` | `public void PutMany(StorePut[] items)` | 保存或设置数据 |
| `FengquDB.StoreTransaction` | `public StoreCommandResult QueryIndex(byte[] name, StoreIndexQuery? query = null, byte[]? cursor = null, ulong limit = 256)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public void Release(ulong id)` | 释放对象或借用的资源 |
| `FengquDB.StoreTransaction` | `public void RollbackTo(ulong id)` | 回退到保存点 |
| `FengquDB.StoreTransaction` | `public ulong Savepoint()` | 创建或操作事务保存点 |
| `FengquDB.StoreTransaction` | `public IEnumerable<StoreRow> Scan(byte[]? begin = null, byte[]? end = null, bool reverse = false)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public StoreQueryResult Select(StoreQuery query)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public StoreCommit Submit(bool forceReliable = false)` | 事务操作 |
| `FengquDB.StoreTransaction` | `public bool TryGet<T>(string key, out T value)` | 读取对应的数据或状态 |

</details>

## 7. 筛选、排序、排行榜、统计

```csharp
var score = Student.Cols.Score;
var name = Student.Cols.Name;
var rows = Q.From("student/").Where(score >= 90L)
    .Select(name, score).OrderBy(score.Desc()).Limit(10).Run(db);
foreach (var row in rows) Console.WriteLine($"{row.Get(name)} {row.Get(score)}");
```

`QCond` 用 `&`、`|`、`!` 组合；`GroupBy/Having` 用于分组统计，`Join` 用于按字段关联记录。`Explain` 查看是否走索引，`Memory/TimeoutMs` 限制大查询资源。下面列出全部查询接口及重载。

<details>
<summary>函数与参数（110项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.PreparedQuery` | `public StoreQueryResult Execute(Store db,bool explain=false)` | 查询构建或结果读取 |
| `FengquDB.PreparedQuery` | `public StoreQueryResult Execute(StoreTransaction tx,bool explain=false)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<double> Avg<T>(QCol<T> c)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<bool> Bool(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<ulong> Count()` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<ulong> Count<T>(QCol<T> c)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<ulong> CountDistinct<T>(QCol<T> c)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<double> F32(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<double> F64(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<ulong> Fixed32(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<ulong> Fixed64(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QueryBuilder From(byte[] prefix)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QueryBuilder From(string prefix)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<long> I64(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<string> Key(uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<T> Max<T>(QCol<T> c)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<T> Min<T>(QCol<T> c)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<T> Nested<T>(uint type, uint source, params uint[] path)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<long> S64(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<long> SFixed32(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<long> SFixed64(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QAgg<T> Sum<T>(QCol<T> c)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<string> Text(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.Q` | `public static QCol<ulong> U64(uint n, uint source = 0)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public QSort Asc(bool nullsFirst = true)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public QSort Desc(bool nullsFirst = true)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public override bool Equals(object? o)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public override int GetHashCode()` | 读取对应的数据或状态 |
| `FengquDB.QAgg` | `public static QCond operator !=(QAgg<T> a, T v)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public static QCond operator <(QAgg<T> a, T v)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public static QCond operator <=(QAgg<T> a, T v)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public static QCond operator ==(QAgg<T> a, T v)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public static QCond operator >(QAgg<T> a, T v)` | 查询构建或结果读取 |
| `FengquDB.QAgg` | `public static QCond operator >=(QAgg<T> a, T v)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCol<T> Any()` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QSort Asc(bool nullsFirst = true)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond Between(T low, T high)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond Contains(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QSort Desc(bool nullsFirst = true)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond EndsWith(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public override bool Equals(object? o)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond Exists()` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond Fuzzy(string s, ulong distance)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public override int GetHashCode()` | 读取对应的数据或状态 |
| `FengquDB.QCol` | `public QCond IContains(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond ILike(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond In(params T[] values)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond IsNull()` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond Like(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCol(QueryField field)` | 创建对象 |
| `FengquDB.QCol` | `public QCond Regex(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public QCond StartsWith(string s)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator !=(QCol<T> a, QCol<T> b)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator !=(QCol<T> c, T v)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator <(QCol<T> a, QCol<T> b)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator <(QCol<T> c, T v)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator <=(QCol<T> a, QCol<T> b)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator <=(QCol<T> c, T v)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator ==(QCol<T> a, QCol<T> b)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator ==(QCol<T> c, T v)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator >(QCol<T> a, QCol<T> b)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator >(QCol<T> c, T v)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator >=(QCol<T> a, QCol<T> b)` | 查询构建或结果读取 |
| `FengquDB.QCol` | `public static QCond operator >=(QCol<T> c, T v)` | 查询构建或结果读取 |
| `FengquDB.QCond` | `public static QCond operator !(QCond a)` | 查询构建或结果读取 |
| `FengquDB.QCond` | `public static QCond operator &(QCond a, QCond b)` | 查询构建或结果读取 |
| `FengquDB.QCond` | `public static bool operator false(QCond _)` | 查询构建或结果读取 |
| `FengquDB.QCond` | `public static bool operator true(QCond _)` | 查询构建或结果读取 |
| `FengquDB.QCond` | `public static QCond operator \|(QCond a, QCond b)` | 查询构建或结果读取 |
| `FengquDB.QRow` | `public T Get<T>(QAgg<T> a)` | 读取对应的数据或状态 |
| `FengquDB.QRow` | `public T Get<T>(QCol<T> c)` | 读取对应的数据或状态 |
| `FengquDB.QRow` | `public bool TryGet<T>(QCol<T> c, out T value)` | 读取对应的数据或状态 |
| `FengquDB.QRows` | `public IEnumerator<QRow> GetEnumerator()` | 读取对应的数据或状态 |
| `FengquDB.QueryBuilder` | `public QueryBuilder After(byte[] cursor)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder AllowScan(bool on)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public StoreQuery Build()` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder CountTotal(bool on = true)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Distinct(bool on = true)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder GroupBy(params IQColumn[] items)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Having(QCond c)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Index(string name)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Join<T>(string prefix, QCol<T> left, QCol<T> right, QueryJoinKind kind = QueryJoinKind.Inner)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Limit(ulong n)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder MaxCandidates(ulong n)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Memory(ulong bytes)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Offset(ulong n)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder OrderBy(params IQOrder[] items)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QPrepared Prepare()` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QRows Run(Store db)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QRows Run(StoreTransaction tx)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Select(params IQColumn[] items)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder TimeoutMs(ulong ms)` | 查询构建或结果读取 |
| `FengquDB.QueryBuilder` | `public QueryBuilder Where(QCond c)` | 查询构建或结果读取 |
| `FengquDB.QueryColumn` | `public QueryColumn(QueryField field,QueryAggregate aggregate=QueryAggregate.None)` | 创建对象 |
| `FengquDB.QueryExpr` | `public static QueryExpr All(params QueryExpr[] children)` | 查询构建或结果读取 |
| `FengquDB.QueryExpr` | `public static QueryExpr Any(params QueryExpr[] children)` | 查询构建或结果读取 |
| `FengquDB.QueryExpr` | `public static QueryExpr Not(QueryExpr child)` | 查询构建或结果读取 |
| `FengquDB.QueryField` | `public QueryField Any()` | 查询构建或结果读取 |
| `FengquDB.QueryField` | `public static QueryField Column(uint index)` | 查询构建或结果读取 |
| `FengquDB.QueryField` | `public QueryExpr Compare(QueryOp op,QueryField right)` | 查询构建或结果读取 |
| `FengquDB.QueryField` | `public static QueryField Key(uint source=0)` | 查询构建或结果读取 |
| `FengquDB.QueryField` | `public QueryField()` | 创建对象 |
| `FengquDB.QueryField` | `public QueryField(uint number,uint type,uint source=0)` | 创建对象 |
| `FengquDB.QueryField` | `public static QueryField Record(uint source=0)` | 查询构建或结果读取 |
| `FengquDB.QueryField` | `public QueryExpr Test(QueryOp op,params object?[] values)` | 查询构建或结果读取 |
| `FengquDB.QueryOrder` | `public QueryOrder(QueryField field,bool descending=false)` | 创建对象 |
| `FengquDB.Store` | `public StoreQueryResult Explain(StoreQuery query)` | 查看查询计划 |
| `FengquDB.Store` | `public StoreQueryResult Select(StoreQuery query)` | 查询构建或结果读取 |
| `FengquDB.StoreQuery` | `public byte[] Encode()` | 查询构建或结果读取 |
| `FengquDB.StoreQuery` | `public PreparedQuery Prepare()` | 查询构建或结果读取 |

</details>

## 8. 索引：经常按某个字段找记录

```csharp
byte[] index = System.Text.Encoding.UTF8.GetBytes("student-score");
byte[] prefix = System.Text.Encoding.UTF8.GetBytes("student/");
db.DefineIndex(index, prefix, 3, 5); // 字段3：有符号 ZigZag 整数
```

先按索引缩小范围，再筛选其余条件。唯一、必填和复合字段限制通过索引选项设置。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreMaintenanceInfo DefineCompoundIndex(byte[] name, byte[] prefix, StoreIndexField[] fields, uint flags = 0, StoreMaintenanceOptions? options = null)` | 定义或使用索引 |
| `FengquDB.Store` | `public unsafe StoreMaintenanceInfo DefineIndex(byte[] name, byte[] prefix, uint field, uint type, uint flags = 0, StoreMaintenanceOptions? options = null)` | 定义或使用索引 |
| `FengquDB.Store` | `public unsafe StoreMaintenanceInfo DropIndex(byte[] name, StoreMaintenanceOptions? options = null)` | 定义或使用索引 |
| `FengquDB.Store` | `public StoreCommandResult QueryIndex(byte[] name, StoreIndexQuery? query = null, byte[]? cursor = null, ulong limit = 256)` | 定义或使用索引 |
| `FengquDB.StoreIndexField` | `public StoreIndexField(uint field, uint type)` | 创建对象 |

</details>

## 9. 可靠保存：付款记录必须等到落盘

默认性能模式为 50 ms / 1 MiB，普通写入在内存生效后返回。关键写入这样调用：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```csharp
db.Put("payment/receipt", "paid", forceReliable: true);
using (var tx = db.Begin()) { tx.Put("paid", true); tx.Commit(forceReliable: true); }
db.Sync(); // 等待之前的写入可靠落盘
```

整个库都要求可靠确认时：

```csharp
db.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
```

UI 线程需要不阻塞等待时，使用 SubmitPut 返回的凭据，再 await WaitAsync。COMMIT_UNKNOWN 表示结果不确定，需按订单号核对，不要直接重复扣款。

<details>
<summary>函数与参数（13项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public void ConfigureWrite(StoreWriteOptions options)` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public Task ConfigureWriteAsync(StoreWriteOptions options)` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public void ResetWriteOptions()` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public Task ResetWriteOptionsAsync()` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public unsafe StoreCommit SubmitPut(byte[] key, byte[] value, ulong expiresAtMs = 0, bool forceReliable = false)` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public void Sync()` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public StoreWriteOptions WriteOptions()` | 控制可靠确认或提交等待 |
| `FengquDB.Store` | `public StoreWriteState WriteState()` | 控制可靠确认或提交等待 |
| `FengquDB.StoreCommit` | `public void Cancel()` | 控制可靠确认或提交等待 |
| `FengquDB.StoreCommit` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StoreCommit` | `public ulong? Poll()` | 控制可靠确认或提交等待 |
| `FengquDB.StoreCommit` | `public ulong Wait(ulong timeoutMs = 0)` | 控制可靠确认或提交等待 |
| `FengquDB.StoreCommit` | `public async Task<ulong> WaitAsync(int pollIntervalMs = 5)` | 控制可靠确认或提交等待 |

</details>

## 10. 到期数据：验证码、临时状态

```csharp
ulong expires = (ulong)DateTimeOffset.UtcNow.AddMinutes(5).ToUnixTimeMilliseconds();
db.Put("verification", "1234", expiresAtMs: expires);
```

时间是绝对 Unix 毫秒；0 表示不过期。到期后读不到，物理空间随后由清理或整理回收。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreExpiryInfo PurgeExpired(StoreExpiryOptions? options = null)` | 到期数据操作 |

</details>

## 11. 热点读取：复用键和读取缓冲

```csharp
using var scoreReader = db.PrepareValue<int>("score");
int score = scoreReader.Read();
using var entityReader = db.PrepareEntity<Student>(7);
Student student = entityReader.Read();
```

普通 Get 返回可独立保存的值。共享读和读取器要及时 Dispose；读取器给出的字节视图不能保存到下一次 Read 之后。

<details>
<summary>函数与参数（19项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareEntity<T>(long id) where T : class, IFqEntity<T>, new()` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareEntity<T>(string id) where T : class, IFqEntity<T>, new()` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareEntity<T>(ulong id) where T : class, IFqEntity<T>, new()` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public unsafe StorePreparedKey PrepareKey(byte[] key)` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public StoreReader PrepareReader(byte[] key)` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public StoreReader PrepareReader(byte[][] keys)` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public StoreTypedReader<T> PrepareValue<T>(string key)` | 复用键、缓冲或共享字节 |
| `FengquDB.Store` | `public unsafe TResult ReadShared<TResult>(byte[] key, StoreValueAction<TResult> action)` | 读取对应的数据或状态 |
| `FengquDB.StorePreparedKey` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StorePreparedKey` | `public byte[]? Get()` | 读取对应的数据或状态 |
| `FengquDB.StorePreparedKey` | `public StoreEntry? GetEntry()` | 读取对应的数据或状态 |
| `FengquDB.StorePreparedKey` | `public unsafe ulong? GetInto(byte[] destination)` | 读取对应的数据或状态 |
| `FengquDB.StoreReader` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StoreReader` | `public TResult Read<TResult>(StoreReadAction<TResult> action)` | 读取对应的数据或状态 |
| `FengquDB.StoreReader` | `public void Release()` | 释放对象或借用的资源 |
| `FengquDB.StoreTypedReader` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StoreTypedReader` | `public T Read()` | 读取对应的数据或状态 |
| `FengquDB.StoreTypedReader` | `public void Release()` | 释放对象或借用的资源 |
| `FengquDB.StoreTypedReader` | `public bool TryRead(out T value)` | 复用键、缓冲或共享字节 |

</details>

## 12. 一致读取：一组读取使用同一时刻的数据

```csharp
using var snapshot = db.Snapshot();
var student = snapshot.GetEntity<Student>(7);
int score = snapshot.Get<int>("score");
```

<details>
<summary>函数与参数（6项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreSnapshot Snapshot()` | 一致读取或范围遍历 |
| `FengquDB.StoreSnapshot` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StoreSnapshot` | `public byte[]? Get(byte[] key)` | 读取对应的数据或状态 |
| `FengquDB.StoreSnapshot` | `public StoreEntry?[] GetEntries(byte[][] keys)` | 读取对应的数据或状态 |
| `FengquDB.StoreSnapshot` | `public byte[]?[] GetMany(byte[][] keys)` | 读取对应的数据或状态 |
| `FengquDB.StoreSnapshot` | `public IEnumerable<StoreRow> Scan(byte[]? begin = null, byte[]? end = null, bool reverse = false)` | 一致读取或范围遍历 |

</details>

## 13. 备份和恢复：更新前留一份存档

```csharp
db.Backup("backups/full-001"); // 使用新的目标目录
```

恢复时关闭要替换的数据库，调用 Restore/RecoverChain；不要直接复制正在写入的数据库目录。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreMaintenanceInfo Backup(string destination, StoreMaintenanceOptions? options = null)` | 备份、恢复或历史保留 |
| `FengquDB.Store` | `public StoreMaintenanceInfo BackupIncremental(string destination, ulong fromVersion, StoreMaintenanceOptions? options = null)` | 备份、恢复或历史保留 |
| `FengquDB.Store` | `public StoreHistoryInfo HistoryInfo()` | 备份、恢复或历史保留 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo HoldHistory(byte[] name, ulong fromVersion = ulong.MaxValue, ulong timeoutMs = 0)` | 备份、恢复或历史保留 |
| `FengquDB.Store` | `public static StoreMaintenanceInfo RecoverChain(string source, string[] archives, string destination, ulong targetVersion = 0, ulong targetUnixMs = 0, StoreMaintenanceOptions? options = null)` | 备份、恢复或历史保留 |
| `FengquDB.Store` | `public unsafe StoreAtomicInfo ReleaseHistory(byte[] name, ulong timeoutMs = 0)` | 释放对象或借用的资源 |
| `FengquDB.Store` | `public static StoreMaintenanceInfo Restore(string source, string destination, StoreMaintenanceOptions? options = null)` | 备份、恢复或历史保留 |

</details>

## 14. 集合：Hash、Set、List 和有序集合

Execute 接收集合命令；HashGet 读取 Hash 字段。适合背包成员、去重集合和排行榜，不需要自己把整份集合反复读回修改。各命令参数见本节类型和重载。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public StoreCommandResult Execute(StoreOperation operation, byte[] scope, byte[]? key = null, byte[]? value = null, long argument = 0, double score = 0, ulong limit = 256, uint flags = 0, bool forceReliable = false)` | 原生集合操作 |
| `FengquDB.Store` | `public unsafe byte[]? HashGet(byte[] scope, byte[] key)` | 读取对应的数据或状态 |

</details>

## 15. 整理、清理和检查

```csharp
db.PurgeExpired();
db.Compact();
db.Verify();
```

放在维护时段调用；大库可用选项控制每次处理量。

<details>
<summary>函数与参数（6项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public void Checkpoint()` | 查看状态或维护数据库 |
| `FengquDB.Store` | `public StoreMaintenanceInfo Compact(StoreMaintenanceOptions? options = null)` | 查看状态或维护数据库 |
| `FengquDB.Store` | `public void Configure(StoreRuntimeOptions? options = null)` | 查看状态或维护数据库 |
| `FengquDB.Store` | `public StoreInfo Info()` | 查看状态或维护数据库 |
| `FengquDB.Store` | `public StoreRuntimeInfo RuntimeInfo()` | 查看状态或维护数据库 |
| `FengquDB.Store` | `public StoreMaintenanceInfo Verify(StoreMaintenanceOptions? options = null)` | 查看状态或维护数据库 |

</details>

## 16. 变更通知：消费本进程内的提交变化

Watch 创建消费者，读取变化后 Ack 确认处理水位，关闭时 Dispose。适合更新业务缓存或触发后台任务。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Store` | `public unsafe StoreWatch Watch(ulong fromVersion = ulong.MaxValue, byte[]? prefix = null, StoreWatchOptions? options = null)` | 消费变更或定位问题 |
| `FengquDB.StoreWatch` | `public void Ack(ulong version)` | 消费变更或定位问题 |
| `FengquDB.StoreWatch` | `public void Cancel()` | 消费变更或定位问题 |
| `FengquDB.StoreWatch` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StoreWatch` | `public unsafe StoreChanges? Next(ulong timeoutMs = 0)` | 消费变更或定位问题 |

</details>

## 17. 编解码与手动生成

正常编译会生成实体的 Decode、EncodeTo、MakeKey、Read字段和 Cols，调用不使用运行时反射。旧字段编号不要改动。

手动工具的放置和双击步骤见本语言接入页。原生工具读取 `// fqdb:entity prefix=student/ key=Id` 标记，生成独立 codec；它与编译器自动生成的实体接口调用方式不同。使用手动兜底时按下面方式存取，无需运行时反射：

```csharp
byte[] key = StudentFqCodec.Key(student);
db.Put(key, StudentFqCodec.Encode(student));
var row = StudentFqCodec.Decode(db.Get(key)!);
row.Value.Score++;
db.Put(row.Key(), row.Encode());
```

读改写时通过记录 Encode 保留未知字段。复用编码缓冲时，不要与实体的字节字段重叠。

<details>
<summary>函数与参数（32项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.Entities.FqEntityAttribute` | `public FqEntityAttribute(string keyPrefix)` | 创建对象 |
| `FengquDB.Entities.FqEntityStorage` | `public static void ValidateSize(int keyBufferBytes, int valueBytes)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqGeneratedEntityAttribute` | `public FqGeneratedEntityAttribute(string schema)` | 创建对象 |
| `FengquDB.Entities.FqWire` | `public static int BytesSize(int length)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static byte[] CopyUnknown(ReadOnlySpan<byte> source, int length, Func<uint, bool> known)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int KeySize(ReadOnlySpan<byte> prefix, long id)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int KeySize(ReadOnlySpan<byte> prefix, string id)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int KeySize(ReadOnlySpan<byte> prefix, ulong id)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static bool ReadBool(ReadOnlySpan<byte> input, ref int position)` | 读取对应的数据或状态 |
| `FengquDB.Entities.FqWire` | `public static ReadOnlySpan<byte> ReadBytes(ReadOnlySpan<byte> input, ref int position)` | 读取对应的数据或状态 |
| `FengquDB.Entities.FqWire` | `public static double ReadDouble(ReadOnlySpan<byte> input, ref int position)` | 读取对应的数据或状态 |
| `FengquDB.Entities.FqWire` | `public static float ReadSingle(ReadOnlySpan<byte> input, ref int position)` | 读取对应的数据或状态 |
| `FengquDB.Entities.FqWire` | `public static uint ReadTag(ReadOnlySpan<byte> input, ref int position)` | 读取对应的数据或状态 |
| `FengquDB.Entities.FqWire` | `public static ulong ReadVarint(ReadOnlySpan<byte> input, ref int position)` | 读取对应的数据或状态 |
| `FengquDB.Entities.FqWire` | `public static void Skip(ReadOnlySpan<byte> input, ref int position, uint tag)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int StringSize(string value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static long UnZigZag(ulong value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int VarintSize(ulong value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteBytes(Span<byte> output, ReadOnlySpan<byte> value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteDouble(Span<byte> output, double value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, long id)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, string id)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, ulong id)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteSingle(Span<byte> output, float value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteString(Span<byte> output, string value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static int WriteVarint(Span<byte> output, ulong value)` | 实体或二进制编解码 |
| `FengquDB.Entities.FqWire` | `public static ulong ZigZag(long value)` | 实体或二进制编解码 |
| `FengquDB.Entities.IFqEntity` | `int EncodeTo(Span<byte> destination)` | 实体或二进制编解码 |
| `FengquDB.Entities.IFqEntity` | `int EncodedSize()` | 实体或二进制编解码 |
| `FengquDB.Entities.IFqEntity` | `T ReadEntity(ReadOnlySpan<byte> source)` | 读取对应的数据或状态 |
| `FengquDB.Entities.IFqEntity` | `ulong WriteTo(global::FengquDB.Store store, ulong expiresAtMs, bool forceReliable)` | 实体或二进制编解码 |
| `FengquDB.Entities.IFqEntity` | `void WriteTo(global::FengquDB.StoreTransaction transaction, ulong expiresAtMs)` | 实体或二进制编解码 |

</details>

## 其他辅助接口

<details>
<summary>函数与参数（16项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `FengquDB.DatabaseException` | `public DatabaseException(int code) : base($"FengquDB {code}: {Marshal.PtrToStringAnsi(Native.fqdb_status_message(code))}")` | 创建对象 |
| `FengquDB.Entities.FqFieldAttribute` | `public FqFieldAttribute(int number)` | 创建对象 |
| `FengquDB.IQOrder` | `QueryOrder Resolve(IReadOnlyList<QueryColumn> columns)` | 辅助操作 |
| `FengquDB.QPrepared` | `public QRows Run(Store db)` | 辅助操作 |
| `FengquDB.QPrepared` | `public QRows Run(StoreTransaction tx)` | 辅助操作 |
| `FengquDB.QSort` | `public QueryOrder Resolve(IReadOnlyList<QueryColumn> columns)` | 辅助操作 |
| `FengquDB.Store` | `public void CancelMaintenance()` | 辅助操作 |
| `FengquDB.Store` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.Store` | `public static Store OpenAt(string root, string name, StoreOptions? options = null)` | 辅助操作 |
| `FengquDB.Store` | `public static string PathAt(string root, string name)` | 辅助操作 |
| `FengquDB.Store` | `public StoreReader PrepareProjectReader(byte[][] keys, uint[] fields)` | 辅助操作 |
| `FengquDB.Store` | `public unsafe byte[] Project(byte[] key, uint[] fields)` | 辅助操作 |
| `FengquDB.Store` | `public Store(string directory, StoreOptions? options = null)` | 创建对象 |
| `FengquDB.StoreI64FieldUpdate` | `public StoreI64FieldUpdate(byte[] key, ulong offset, long delta, long minimum = long.MinValue, long maximum = long.MaxValue)` | 创建对象 |
| `FengquDB.StorePreparedBatch` | `public void Dispose()` | 释放对象或借用的资源 |
| `FengquDB.StorePut` | `public StorePut(byte[] key, byte[] value, ulong expiresAtMs = 0)` | 创建对象 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>FengquDB</summary>

| 声明 |
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

| 声明 |
| --- |
| `ulong RequiredSize { get; }` |

</details>

<details>
<summary>FengquDB.DatabaseException</summary>

| 声明 |
| --- |
| `int Code { get; }` |

</details>

<details>
<summary>FengquDB.Entities</summary>

| 声明 |
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

| 声明 |
| --- |
| `string KeyPrefix { get; }` |

</details>

<details>
<summary>FengquDB.Entities.FqEntityKeyKind</summary>

| 声明 |
| --- |
| `Signed` |
| `Text` |
| `Unsigned` |

</details>

<details>
<summary>FengquDB.Entities.FqFieldAttribute</summary>

| 声明 |
| --- |
| `int Number { get; }` |

</details>

<details>
<summary>FengquDB.Entities.FqGeneratedEntityAttribute</summary>

| 声明 |
| --- |
| `string Schema { get; }` |

</details>

<details>
<summary>FengquDB.Entities.FqWire</summary>

| 声明 |
| --- |
| `Encoding Utf8 = new UTF8Encoding(false, true)` |

</details>

<details>
<summary>FengquDB.Entities.IFqEntity</summary>

| 声明 |
| --- |
| `FqEntityKeyKind KeyKind { get; }` |
| `ReadOnlySpan<byte> KeyPrefix { get; }` |

</details>

<details>
<summary>FengquDB.IQColumn</summary>

| 声明 |
| --- |
| `QueryColumn Column { get; }` |

</details>

<details>
<summary>FengquDB.QAgg</summary>

| 声明 |
| --- |
| `QueryColumn Column { get; }` |

</details>

<details>
<summary>FengquDB.QCol</summary>

| 声明 |
| --- |
| `QueryColumn Column { get; }` |
| `QueryField Field` |

</details>

<details>
<summary>FengquDB.QRow</summary>

| 声明 |
| --- |
| `byte[] Key { get; }` |

</details>

<details>
<summary>FengquDB.QRows</summary>

| 声明 |
| --- |
| `int Count { get; }` |
| `byte[] Cursor { get; }` |
| `StoreQueryResult Raw { get; }` |
| `QueryStats Stats { get; }` |
| `QRow this[int i]` |

</details>

<details>
<summary>FengquDB.QueryAccess</summary>

| 声明 |
| --- |
| `bool Covering,Ordered,NeedsScan` |
| `ulong SampledCandidates` |
| `uint Source` |
| `string Strategy="",Index="",Reason=""` |

</details>

<details>
<summary>FengquDB.QueryAggregate</summary>

| 声明 |
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

| 声明 |
| --- |
| `QueryAggregate Aggregate` |
| `bool Distinct` |
| `QueryField Field` |
| `string Name=""` |

</details>

<details>
<summary>FengquDB.QueryExpr</summary>

| 声明 |
| --- |
| `QueryExpr[] Children=Array.Empty<QueryExpr>()` |
| `QueryField Field=new QueryField()` |
| `QueryOp Op` |
| `QueryField? Right` |
| `object?[] Values=Array.Empty<object?>()` |

</details>

<details>
<summary>FengquDB.QueryField</summary>

| 声明 |
| --- |
| `uint[] Path=Array.Empty<uint>()` |
| `bool Repeated` |
| `uint Source, Type=4` |

</details>

<details>
<summary>FengquDB.QueryJoin</summary>

| 声明 |
| --- |
| `QueryJoinKind Kind` |
| `QueryField[] Left=Array.Empty<QueryField>(),Right=Array.Empty<QueryField>()` |
| `QueryExpr On=new QueryExpr()` |
| `byte[] Prefix=Array.Empty<byte>()` |

</details>

<details>
<summary>FengquDB.QueryJoinKind</summary>

| 声明 |
| --- |
| `Anti` |
| `Inner` |
| `Left` |
| `Semi` |

</details>

<details>
<summary>FengquDB.QueryOp</summary>

| 声明 |
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

| 声明 |
| --- |
| `bool Descending,NullsFirst=true` |
| `QueryField Field` |

</details>

<details>
<summary>FengquDB.QueryRow</summary>

| 声明 |
| --- |
| `byte[] Key=Array.Empty<byte>()` |
| `List<object?> Values=new List<object?>()` |

</details>

<details>
<summary>FengquDB.QueryStats</summary>

| 声明 |
| --- |
| `ulong Candidates,Matched,ResultTotal,Work,PeakMemory,PayloadReads,JoinProbes` |
| `bool Complete` |

</details>

<details>
<summary>FengquDB.StoreAtomicInfo</summary>

| 声明 |
| --- |
| `bool Changed { get; }` |
| `bool Matched { get; }` |
| `long Value` |
| `ulong Version, ObservedVersion` |

</details>

<details>
<summary>FengquDB.StoreChange</summary>

| 声明 |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Key { get; }` |
| `uint Kind { get; }` |
| `byte[]? Value { get; }` |

</details>

<details>
<summary>FengquDB.StoreChanges</summary>

| 声明 |
| --- |
| `ulong DurableVersion { get; }` |
| `uint Flags { get; }` |
| `StoreChange[] Rows { get; }` |
| `ulong UnixMs { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreCommandResult</summary>

| 声明 |
| --- |
| `bool Changed` |
| `byte[] Cursor = Array.Empty<byte>()` |
| `List<StoreFeatureRow> Rows { get; }` |
| `byte[]? Value` |
| `ulong Version, Count` |

</details>

<details>
<summary>FengquDB.StoreEntry</summary>

| 声明 |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Value { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreExpiryInfo</summary>

| 声明 |
| --- |
| `ulong Version, CutoffMs, RemovedKeys, RemovedValueBytes, VisitedPages, More` |

</details>

<details>
<summary>FengquDB.StoreExpiryOptions</summary>

| 声明 |
| --- |
| `ulong MaxBatchBytes { get; set; }` |
| `uint MaxKeys { get; set; }` |
| `ulong TimeoutMs { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreFeatureRow</summary>

| 声明 |
| --- |
| `byte[] Key = Array.Empty<byte>(), Value = Array.Empty<byte>()` |
| `double Score` |

</details>

<details>
<summary>FengquDB.StoreHistoryInfo</summary>

| 声明 |
| --- |
| `ulong Version, Floor, OldestPin, ActivePins, TotalPins, RetainedGenerations, RetainedFileBytes` |

</details>

<details>
<summary>FengquDB.StoreI64FieldUpdate</summary>

| 声明 |
| --- |
| `long Delta { get; }` |
| `byte[] Key { get; }` |
| `long Maximum { get; }` |
| `long Minimum { get; }` |
| `ulong Offset { get; }` |

</details>

<details>
<summary>FengquDB.StoreIndexField</summary>

| 声明 |
| --- |
| `uint Field, Type` |

</details>

<details>
<summary>FengquDB.StoreIndexQuery</summary>

| 声明 |
| --- |
| `byte[] Equal = Array.Empty<byte>()` |
| `uint[] Fields = Array.Empty<uint>()` |
| `byte[]? Lower, Upper` |
| `bool LowerExclusive, UpperInclusive, Reverse, IncludeValues` |

</details>

<details>
<summary>FengquDB.StoreInfo</summary>

| 声明 |
| --- |
| `ulong DeltaBytes, CacheBytes, PinnedBytes, TransactionBytes, RecoveredTransactions, RepairedBytes` |
| `ulong Version, DurableVersion, FileBytes, IndexedKeys, StoredKeys, StoredValueBytes` |

</details>

<details>
<summary>FengquDB.StoreMaintenanceIO</summary>

| 声明 |
| --- |
| `ulong ReadBytes, WriteBytes, SyncCalls` |

</details>

<details>
<summary>FengquDB.StoreMaintenanceInfo</summary>

| 声明 |
| --- |
| `ulong SnapshotVersion, FinalVersion, SourceFileBytes, OutputFileBytes` |
| `ulong StoredKeys, StoredValueBytes, CopiedKeys, CatchupRounds` |

</details>

<details>
<summary>FengquDB.StoreMaintenanceOptions</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Key { get; }` |
| `byte[] Value { get; }` |

</details>

<details>
<summary>FengquDB.StoreQuery</summary>

| 声明 |
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

| 声明 |
| --- |
| `List<QueryAccess> Access=new List<QueryAccess>()` |
| `byte[] Cursor=Array.Empty<byte>()` |
| `List<QueryRow> Rows=new List<QueryRow>()` |
| `QueryStats Stats=new QueryStats()` |

</details>

<details>
<summary>FengquDB.StoreReadBatch</summary>

| 声明 |
| --- |
| `int Count { get; }` |
| `StoreSharedEntry this[int index]` |

</details>

<details>
<summary>FengquDB.StoreRow</summary>

| 声明 |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `byte[] Key { get; }` |
| `byte[] Value { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreRuntimeInfo</summary>

| 声明 |
| --- |
| `ulong Running, LastStatus, ExpiryBatches, Compactions, GroupFlushes, GroupTransactions` |

</details>

<details>
<summary>FengquDB.StoreRuntimeOptions</summary>

| 声明 |
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

| 声明 |
| --- |
| `ulong ExpiresAtMs { get; }` |
| `bool Found { get; }` |
| `ReadOnlySpan<byte> Value { get; }` |
| `ulong Version { get; }` |

</details>

<details>
<summary>FengquDB.StoreWatchOptions</summary>

| 声明 |
| --- |
| `bool IncludeSystem { get; set; }` |
| `ulong MaxBatchBytes { get; set; }` |
| `uint MaxScanCommits { get; set; }` |
| `bool MetadataOnly { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreWriteMode</summary>

| 声明 |
| --- |
| `Performance = 1` |
| `Reliable = 0` |

</details>

<details>
<summary>FengquDB.StoreWriteOptions</summary>

| 声明 |
| --- |
| `StoreWriteMode Mode { get; set; }` |
| `uint PeriodMs { get; set; }` |
| `ulong ThresholdBytes { get; set; }` |

</details>

<details>
<summary>FengquDB.StoreWriteState</summary>

| 声明 |
| --- |
| `ulong Version, DurableVersion, PendingBytes, LastStatus` |

</details>
