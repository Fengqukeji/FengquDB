# Java API 与调用例子

[接入文档](../languages/java.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取学生

```java
byte[] bytes = db.get("student/7".getBytes(StandardCharsets.UTF_8));
Student student = bytes == null ? null : StudentFqCodec.decode(bytes).value;
```

缺失为 null，空数组是有效值。本文用到 `java.nio.charset.StandardCharsets`。

<details>
<summary>函数与参数（3项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public byte[] get(byte[] key)` | 读取对应的数据或状态 |
| `io.fengqudb.Store` | `public Entry[] getEntries(byte[][] keys)` | 读取对应的数据或状态 |
| `io.fengqudb.Store` | `public Entry getEntry(byte[] key)` | 读取对应的数据或状态 |

</details>

## 2. 新增、保存、存一个字符串或整数

```java
Student student = new Student();
student.id=7; student.name="张三"; student.score=95;
db.put(StudentFqCodec.key(student), StudentFqCodec.encode(student));
db.put("name".getBytes(StandardCharsets.UTF_8), "张三".getBytes(StandardCharsets.UTF_8));
```

单个整数无需实体，可以用八字节小端格式：

```java
byte[] key = "score".getBytes(StandardCharsets.UTF_8);
byte[] value = java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(95).array();
db.put(key, value);
long score = java.nio.ByteBuffer.wrap(db.get(key)).order(java.nio.ByteOrder.LITTLE_ENDIAN).getLong();
```

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public AtomicInfo put(byte[] key, byte[] value, boolean... forceReliable)` | 保存或设置数据 |
| `io.fengqudb.Store` | `public AtomicInfo put(byte[] key, byte[] value, long expiresAtMs, boolean... forceReliable)` | 保存或设置数据 |
| `io.fengqudb.Store.Put` | `public Put(byte[] key, byte[] value)` | 创建对象 |
| `io.fengqudb.Store.Put` | `public Put(byte[] key, byte[] value, long expiresAtMs)` | 创建对象 |

</details>

## 3. 修改成绩和原子加减

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

addInt64 仅用于八字节整数；compareExchange 按版本修改，patch 修改业务自己管理的字节区间。

<details>
<summary>函数与参数（10项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public AtomicInfo addInt64(byte[] key, long delta)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo addInt64(byte[] key, long delta, long minimum, long maximum, long timeout, boolean... forceReliable)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo addInt64Fields(Int64FieldUpdate[] items)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo addInt64Fields(Int64FieldUpdate[] items, long timeout, boolean... forceReliable)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo compareExchange(byte[] key, long expected, byte[] value)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo compareExchange(byte[] key, long expected, byte[] value, long expires, long timeout, boolean... forceReliable)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo deleteIfVersion(byte[] key, long expected, boolean... forceReliable)` | 删除记录或成员 |
| `io.fengqudb.Store` | `public AtomicInfo patch(byte[] key, long offset, long remove, byte[] insert, boolean... forceReliable)` | 条件更新或局部修改 |
| `io.fengqudb.Store` | `public AtomicInfo putIfAbsent(byte[] key, byte[] value)` | 保存或设置数据 |
| `io.fengqudb.Store` | `public AtomicInfo putIfChanged(byte[] key, byte[] value, boolean... forceReliable)` | 保存或设置数据 |

</details>

## 4. 删除

```java
db.delete("student/7".getBytes(StandardCharsets.UTF_8));
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public AtomicInfo delete(byte[] key, boolean... forceReliable)` | 删除记录或成员 |

</details>

## 5. 批量读取和写入

```java
byte[][] keys = { "student/7".getBytes(StandardCharsets.UTF_8), "student/8".getBytes(StandardCharsets.UTF_8) };
byte[][] values = db.getMany(keys);
```

putMany/deleteMany 把多项修改放在同一笔原子提交中。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public long deleteMany(byte[][] keys, boolean... forceReliable)` | 删除记录或成员 |
| `io.fengqudb.Store` | `public byte[][] getMany(byte[][] keys)` | 读取对应的数据或状态 |
| `io.fengqudb.Store` | `public PreparedBatch prepareBatch(Put[] items)` | 批量处理 |
| `io.fengqudb.Store` | `public long putBatch(PreparedBatch batch, boolean... forceReliable)` | 保存或设置数据 |
| `io.fengqudb.Store` | `public long putMany(Put[] items, boolean... forceReliable)` | 保存或设置数据 |

</details>

## 6. 事务：一组修改一起成功

```java
try (Store.Transaction tx = db.begin()) {
    tx.put("a".getBytes(StandardCharsets.UTF_8), new byte[]{1});
    tx.put("b".getBytes(StandardCharsets.UTF_8), new byte[]{2});
    tx.commit();
}
```

未提交即 close 会丢弃修改。CONFLICT 时重建事务、重新读取计算。

<details>
<summary>函数与参数（26项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Transaction begin()` | 事务操作 |
| `io.fengqudb.Store` | `public Transaction begin(long timeoutMs)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public void cancel()` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.Transaction` | `public long commit(boolean... forceReliable)` | 提交事务或查看提交结果 |
| `io.fengqudb.Store.Transaction` | `public void delete(byte[] key)` | 删除记录或成员 |
| `io.fengqudb.Store.Transaction` | `public void deleteMany(byte[][] keys)` | 删除记录或成员 |
| `io.fengqudb.Store.Transaction` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value, long argument, double score, long limit, int flags)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public void expire(byte[] key, long expiresAtMs)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public byte[] get(byte[] key)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.Transaction` | `public Entry[] getEntries(byte[][] keys)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.Transaction` | `public byte[][] getMany(byte[][] keys)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.Transaction` | `public void patch(byte[] key, long offset, long remove, byte[] insert)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public byte[] project(byte[] key, int[] fields)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public void put(byte[] key, byte[] value)` | 保存或设置数据 |
| `io.fengqudb.Store.Transaction` | `public void put(byte[] key, byte[] value, long expiresAtMs)` | 保存或设置数据 |
| `io.fengqudb.Store.Transaction` | `public void putBatch(PreparedBatch batch)` | 保存或设置数据 |
| `io.fengqudb.Store.Transaction` | `public void putMany(Put[] items)` | 保存或设置数据 |
| `io.fengqudb.Store.Transaction` | `public CommandResult queryIndex(byte[] name, IndexQuery query, byte[] cursor, long limit)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public void release(long id)` | 释放对象或借用的资源 |
| `io.fengqudb.Store.Transaction` | `public void rollbackTo(long id)` | 回退到保存点 |
| `io.fengqudb.Store.Transaction` | `public long savepoint()` | 创建或操作事务保存点 |
| `io.fengqudb.Store.Transaction` | `public Cursor scan()` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public Cursor scan(byte[] begin, byte[] end, boolean reverse)` | 事务操作 |
| `io.fengqudb.Store.Transaction` | `public CommitReceipt submit(boolean... forceReliable)` | 事务操作 |

</details>

## 7. 条件查询与排序

```java
var score = io.fengqudb.Q.s64(StudentFqCodec.Field_score);
var name = io.fengqudb.Q.text(StudentFqCodec.Field_name);
var rows = io.fengqudb.Q.from("student/").where(score.ge(90L))
    .select(name, score).orderBy(score.desc()).limit(10).run(db);
```

条件可 and/or/not 组合；groupBy/having 做分组统计，join 做等值关联，explain 查看索引选择。

<details>
<summary>函数与参数（116项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Q` | `public static Agg<Double> avg(Col<?> c)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Boolean> bool(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Agg<Long> count()` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Agg<Long> count(Col<?> c)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Double> f32(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Double> f64(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> fixed32(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> fixed64(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Builder from(String prefix)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Builder from(byte[] prefix)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> i64(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<String> key(int source)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static <T> Agg<T> max(Col<T> c)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static <T> Agg<T> min(Col<T> c)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> s64(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> s64(int n, int source)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> sfixed32(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> sfixed64(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static <T> Agg<T> sum(Col<T> c)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<String> text(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<String> text(int n, int source)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> u64(int n)` | 查询构建或结果读取 |
| `io.fengqudb.Q` | `public static Col<Long> u64(int n, int source)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Sort asc()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Select.Column column()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Sort desc()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Cond eq(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Cond ge(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Cond gt(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Cond le(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Cond lt(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Agg` | `public Cond ne(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder after(byte[] cursor)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder allowScan(boolean on)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Select build()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder countTotal(boolean on)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder distinct(boolean on)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder groupBy(Col<?>... cols)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder having(Cond c)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public <T> Builder join(String prefix, Col<T> left, Col<T> right)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public <T> Builder join(String prefix, Col<T> left, Col<T> right, Select.JoinKind kind)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder limit(long n)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder offset(long n)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder orderBy(Object... items)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Prepared prepare()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Rows run(Store db)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Rows run(Store.Transaction tx)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder select(Object... items)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Builder` | `public Builder where(Cond c)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Col<T> any()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Sort asc()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond between(T low, T high)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Select.Column column()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond contains(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Sort desc()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond endsWith(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond eq(Col<T> o)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond eq(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond exists()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond fuzzy(String s, int distance)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond ge(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond gt(Col<T> o)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond gt(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond icontains(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond ilike(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `@SafeVarargs() public final Cond in(T... values)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond isNull()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond le(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond like(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond lt(Col<T> o)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond lt(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond ne(T v)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond regex(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Col` | `public Cond startsWith(String s)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Cond` | `public Cond and(Cond other)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Cond` | `public Cond not()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Cond` | `public Cond or(Cond other)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Prepared` | `public Rows run(Store db)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Prepared` | `public Rows run(Store.Transaction tx)` | 查询构建或结果读取 |
| `io.fengqudb.Q.Row` | `public <T> T get(Agg<T> a)` | 读取对应的数据或状态 |
| `io.fengqudb.Q.Row` | `public <T> T get(Col<T> c)` | 读取对应的数据或状态 |
| `io.fengqudb.Q.Row` | `public byte[] key()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Rows` | `public byte[] cursor()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Rows` | `public Row get(int i)` | 读取对应的数据或状态 |
| `io.fengqudb.Q.Rows` | `public Iterator<Row> iterator()` | 查询构建或结果读取 |
| `io.fengqudb.Q.Rows` | `public int size()` | 查询构建或结果读取 |
| `io.fengqudb.Select` | `public byte[] encode()` | 查询构建或结果读取 |
| `io.fengqudb.Select` | `public Result execute(Store db)` | 查询构建或结果读取 |
| `io.fengqudb.Select` | `public Result execute(Store.Transaction tx)` | 查询构建或结果读取 |
| `io.fengqudb.Select` | `public Result explain(Store db)` | 查看查询计划 |
| `io.fengqudb.Select` | `public Result explain(Store.Transaction tx)` | 查看查询计划 |
| `io.fengqudb.Select` | `public Prepared prepare()` | 查询构建或结果读取 |
| `io.fengqudb.Select.Column` | `public Column(Field f)` | 创建对象 |
| `io.fengqudb.Select.Column` | `public Column(Field f, Aggregate a)` | 创建对象 |
| `io.fengqudb.Select.Expr` | `public static Expr all(Expr... children)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Expr` | `public static Expr any(Expr... children)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Expr` | `public static Expr not(Expr child)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Field` | `public Field()` | 创建对象 |
| `io.fengqudb.Select.Field` | `public Field(int number, int type)` | 创建对象 |
| `io.fengqudb.Select.Field` | `public Field(int number, int type, int source)` | 创建对象 |
| `io.fengqudb.Select.Field` | `public Field any()` | 查询构建或结果读取 |
| `io.fengqudb.Select.Field` | `public static Field column(int index)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Field` | `public Expr compare(Op op, Field right)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Field` | `public static Field key(int source)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Field` | `public static Field record(int source)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Field` | `public Expr test(Op op, Object... values)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Order` | `public Order(Field f)` | 创建对象 |
| `io.fengqudb.Select.Order` | `public Order(Field f, boolean desc)` | 创建对象 |
| `io.fengqudb.Select.Prepared` | `public Result execute(Store db, boolean explain)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Prepared` | `public Result execute(Store.Transaction tx, boolean explain)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Value` | `public static Value of(Object x)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Value` | `public String text()` | 查询构建或结果读取 |
| `io.fengqudb.Select.Value` | `public static Value uintBits(long bits)` | 查询构建或结果读取 |
| `io.fengqudb.Select.Value` | `public String unsignedText()` | 查询构建或结果读取 |
| `io.fengqudb.Store` | `public CommandResult selectRaw(int operation, byte[] value)` | 查询构建或结果读取 |
| `io.fengqudb.Store.Row` | `public Row(byte[] key, byte[] value, long version, long expiresAtMs)` | 创建对象 |

</details>

## 8. 索引

```java
db.defineIndex("student-score".getBytes(StandardCharsets.UTF_8),
    "student/".getBytes(StandardCharsets.UTF_8), StudentFqCodec.Field_score, 5, 0);
```

5 表示有符号 ZigZag 整数。

<details>
<summary>函数与参数（8项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public MaintenanceInfo defineCompoundIndex(byte[] name, byte[] prefix, IndexField[] fields, int flags)` | 定义或使用索引 |
| `io.fengqudb.Store` | `public MaintenanceInfo defineCompoundIndex(byte[] name, byte[] prefix, IndexField[] fields, int flags, MaintenanceOptions options)` | 定义或使用索引 |
| `io.fengqudb.Store` | `public MaintenanceInfo defineIndex(byte[] name, byte[] prefix, int field, int type, int flags)` | 定义或使用索引 |
| `io.fengqudb.Store` | `public MaintenanceInfo defineIndex(byte[] name, byte[] prefix, int field, int type, int flags, MaintenanceOptions options)` | 定义或使用索引 |
| `io.fengqudb.Store` | `public MaintenanceInfo dropIndex(byte[] name)` | 定义或使用索引 |
| `io.fengqudb.Store` | `public MaintenanceInfo dropIndex(byte[] name, MaintenanceOptions options)` | 定义或使用索引 |
| `io.fengqudb.Store` | `public CommandResult queryIndex(byte[] name, IndexQuery query, byte[] cursor, long limit)` | 定义或使用索引 |
| `io.fengqudb.Store.IndexField` | `public IndexField(int field, int type)` | 创建对象 |

</details>

## 9. 付款等关键写入：可靠确认

默认性能模式为 50 ms / 1 MiB，普通写入确认内存生效。关键写入传 true：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```java
db.put("payment/receipt".getBytes(StandardCharsets.UTF_8), "paid".getBytes(StandardCharsets.UTF_8), true);
db.sync();
```

事务使用 commit(true)。submitPut 返回提交凭据，需等待或轮询。COMMIT_UNKNOWN 时按业务编号核对，不能盲目重复扣款。

<details>
<summary>函数与参数（14项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public void configureWrite(WriteOptions p)` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public java.util.concurrent.CompletableFuture<Void> configureWriteAsync(WriteOptions p, java.util.concurrent.Executor executor)` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public void resetWriteOptions()` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public java.util.concurrent.CompletableFuture<Void> resetWriteOptionsAsync(java.util.concurrent.Executor executor)` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public CommitReceipt submitPut(byte[] key, byte[] value)` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public CommitReceipt submitPut(byte[] key, byte[] value, long expiresAtMs, boolean... forceReliable)` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public void sync()` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public WriteOptions writeOptions()` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store` | `public WriteState writeState()` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store.CommitReceipt` | `public long await()` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store.CommitReceipt` | `public long await(long timeoutMs)` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store.CommitReceipt` | `public void cancel()` | 控制可靠确认或提交等待 |
| `io.fengqudb.Store.CommitReceipt` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.CommitReceipt` | `public Long poll()` | 控制可靠确认或提交等待 |

</details>

## 10. 临时数据与到期时间

写入时使用带 expiresAtMs 的重载，传入 `System.currentTimeMillis() + 300_000` 即五分钟后过期；0 表示不过期。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public ExpiryInfo purgeExpired()` | 到期数据操作 |
| `io.fengqudb.Store` | `public ExpiryInfo purgeExpired(ExpiryOptions options)` | 到期数据操作 |

</details>

## 11. 高频读取

```java
try (var key = db.prepareKey("student/7".getBytes(StandardCharsets.UTF_8))) {
    byte[] bytes = key.get();
}
```

普通 get 返回独立数组。共享 ByteBuffer/租约和读取器要及时 close；不要在关闭后使用它们派生的视图。

<details>
<summary>函数与参数（19项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public SharedValue getShared(byte[] key)` | 读取对应的数据或状态 |
| `io.fengqudb.Store` | `public PreparedKey prepareKey(byte[] key)` | 复用键、缓冲或共享字节 |
| `io.fengqudb.Store` | `public PreparedReader prepareReader(byte[] key)` | 复用键、缓冲或共享字节 |
| `io.fengqudb.Store` | `public PreparedReader prepareReader(byte[][] keys)` | 复用键、缓冲或共享字节 |
| `io.fengqudb.Store.PreparedKey` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.PreparedKey` | `public byte[] get()` | 读取对应的数据或状态 |
| `io.fengqudb.Store.PreparedKey` | `public Entry getEntry()` | 读取对应的数据或状态 |
| `io.fengqudb.Store.PreparedKey` | `public long getInto(byte[] destination)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.PreparedKey` | `public long getIntoDirect(ByteBuffer destination)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.PreparedKey` | `public SharedValue getShared()` | 读取对应的数据或状态 |
| `io.fengqudb.Store.PreparedReader` | `public synchronized void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.PreparedReader` | `public synchronized SharedBatch readShared()` | 读取对应的数据或状态 |
| `io.fengqudb.Store.PreparedReader` | `public synchronized void release()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.SharedBatch` | `public synchronized void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.SharedBatch` | `public synchronized SharedValue get(int index)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.SharedBatch` | `public synchronized int size()` | 复用键、缓冲或共享字节 |
| `io.fengqudb.Store.SharedValue` | `public synchronized ByteBuffer buffer()` | 复用键、缓冲或共享字节 |
| `io.fengqudb.Store.SharedValue` | `public synchronized void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.SharedValue` | `public boolean isZeroCopy()` | 复用键、缓冲或共享字节 |

</details>

## 12. 一致快照

```java
try (var snapshot = db.snapshot()) {
    byte[] value = snapshot.get("student/7".getBytes(StandardCharsets.UTF_8));
}
```

<details>
<summary>函数与参数（11项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Snapshot snapshot()` | 一致读取或范围遍历 |
| `io.fengqudb.Store.Cursor` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.Cursor` | `public synchronized boolean hasNext()` | 一致读取或范围遍历 |
| `io.fengqudb.Store.Cursor` | `public Iterator<Row> iterator()` | 一致读取或范围遍历 |
| `io.fengqudb.Store.Cursor` | `public synchronized Row next()` | 一致读取或范围遍历 |
| `io.fengqudb.Store.Snapshot` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.Snapshot` | `public byte[] get(byte[] key)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.Snapshot` | `public Entry[] getEntries(byte[][] keys)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.Snapshot` | `public byte[][] getMany(byte[][] keys)` | 读取对应的数据或状态 |
| `io.fengqudb.Store.Snapshot` | `public Cursor scan()` | 一致读取或范围遍历 |
| `io.fengqudb.Store.Snapshot` | `public Cursor scan(byte[] begin, byte[] end, boolean reverse)` | 一致读取或范围遍历 |

</details>

## 13. 备份恢复

```java
db.backup("backups/full-001");
```

备份用新目录；恢复使用 restore/recoverChain，先关闭要替换的库。

<details>
<summary>函数与参数（12项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public MaintenanceInfo backup(String destination)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public MaintenanceInfo backup(String destination, MaintenanceOptions options)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public MaintenanceInfo backupIncremental(String destination, long fromVersion)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public MaintenanceInfo backupIncremental(String destination, long fromVersion, MaintenanceOptions options)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public HistoryInfo historyInfo()` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public AtomicInfo holdHistory(byte[] name)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public AtomicInfo holdHistory(byte[] name, long fromVersion)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public static MaintenanceInfo recoverChain(String base, String[] archives, String destination, long targetVersion, long targetUnixMs)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public static MaintenanceInfo recoverChain(String base, String[] archives, String destination, long targetVersion, long targetUnixMs, MaintenanceOptions options)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public AtomicInfo releaseHistory(byte[] name)` | 释放对象或借用的资源 |
| `io.fengqudb.Store` | `public static MaintenanceInfo restore(String source, String destination)` | 备份、恢复或历史保留 |
| `io.fengqudb.Store` | `public static MaintenanceInfo restore(String source, String destination, MaintenanceOptions options)` | 备份、恢复或历史保留 |

</details>

## 14. Hash、Set、List 和有序集合

execute 执行集合命令，hashGet 读取 Hash 字段。适用于原子成员操作、队列和排名。

<details>
<summary>函数与参数（3项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value, boolean... forceReliable)` | 原生集合操作 |
| `io.fengqudb.Store` | `public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value, long argument, double score, long limit, int flags, boolean... forceReliable)` | 原生集合操作 |
| `io.fengqudb.Store` | `public byte[] hashGet(byte[] scope, byte[] key)` | 读取对应的数据或状态 |

</details>

## 15. 清理和整理

```java
db.purgeExpired();
db.compact();
db.verify();
```

<details>
<summary>函数与参数（9项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public void checkpoint()` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public MaintenanceInfo compact()` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public MaintenanceInfo compact(MaintenanceOptions options)` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public void configure()` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public void configure(RuntimeOptions options)` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public long[] info()` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public RuntimeInfo runtimeInfo()` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public MaintenanceInfo verify()` | 查看状态或维护数据库 |
| `io.fengqudb.Store` | `public MaintenanceInfo verify(MaintenanceOptions options)` | 查看状态或维护数据库 |

</details>

## 16. 提交变化

watch 创建消费者，处理完成后 ack，使用完 close。适合本进程业务缓存更新。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Watch watch()` | 消费变更或定位问题 |
| `io.fengqudb.Store` | `public Watch watch(long fromVersion, byte[] prefix, WatchOptions options)` | 消费变更或定位问题 |
| `io.fengqudb.Store.Watch` | `public void ack(long version)` | 消费变更或定位问题 |
| `io.fengqudb.Store.Watch` | `public void cancel()` | 消费变更或定位问题 |
| `io.fengqudb.Store.Watch` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store.Watch` | `public Changes next()` | 消费变更或定位问题 |
| `io.fengqudb.Store.Watch` | `public Changes next(long timeoutMs)` | 消费变更或定位问题 |

</details>

## 17. 实体编解码

StudentFqCodec.encode/decode 直接访问 Student。记录的 fqEncode 保留未知字段；encodeInto(value, buffer, offset) 复用数组。自动生成在 Gradle 编译前运行，原生工具已随 SDK 提供。

## 其他辅助接口

<details>
<summary>函数与参数（13项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `io.fengqudb.Store` | `public Store(String directory)` | 创建对象 |
| `io.fengqudb.Store` | `public Store(String directory, Options options)` | 创建对象 |
| `io.fengqudb.Store` | `public void cancelMaintenance()` | 辅助操作 |
| `io.fengqudb.Store` | `public void close()` | 释放对象或借用的资源 |
| `io.fengqudb.Store` | `public static Store openAt(String root, String name)` | 辅助操作 |
| `io.fengqudb.Store` | `public static Store openAt(String root, String name, Options options)` | 辅助操作 |
| `io.fengqudb.Store` | `public static String pathAt(String root, String name)` | 辅助操作 |
| `io.fengqudb.Store` | `public PreparedReader prepareProjectReader(byte[][] keys, int[] fields)` | 辅助操作 |
| `io.fengqudb.Store` | `public byte[] project(byte[] key, int[] fields)` | 辅助操作 |
| `io.fengqudb.Store.DbException` | `public DbException(int code)` | 创建对象 |
| `io.fengqudb.Store.Int64FieldUpdate` | `public Int64FieldUpdate(byte[] key, long offset, long delta)` | 创建对象 |
| `io.fengqudb.Store.Int64FieldUpdate` | `public Int64FieldUpdate(byte[] key, long offset, long delta, long minimum, long maximum)` | 创建对象 |
| `io.fengqudb.Store.PreparedBatch` | `public void close()` | 释放对象或借用的资源 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>io.fengqudb</summary>

| 声明 |
| --- |
| `public class Q` |
| `public class Select` |
| `public class Store implements AutoCloseable` |

</details>

<details>
<summary>io.fengqudb.Q</summary>

| 声明 |
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

| 声明 |
| --- |
| `public final Select.Field field` |

</details>

<details>
<summary>io.fengqudb.Q.Rows</summary>

| 声明 |
| --- |
| `public final Select.Result raw` |

</details>

<details>
<summary>io.fengqudb.Select</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `public Aggregate aggregate` |
| `public boolean distinct` |
| `public Field field` |
| `public String name` |

</details>

<details>
<summary>io.fengqudb.Select.Expr</summary>

| 声明 |
| --- |
| `public List<Expr> children` |
| `public Field field` |
| `public Op op` |
| `public Field right` |
| `public List<Value> values` |

</details>

<details>
<summary>io.fengqudb.Select.Field</summary>

| 声明 |
| --- |
| `public int[] path` |
| `public boolean repeated` |
| `public int source` |
| `public int type` |

</details>

<details>
<summary>io.fengqudb.Select.Join</summary>

| 声明 |
| --- |
| `public JoinKind kind` |
| `public List<Field> left` |
| `public Expr on` |
| `public byte[] prefix` |
| `public List<Field> right` |

</details>

<details>
<summary>io.fengqudb.Select.JoinKind</summary>

| 声明 |
| --- |
| `public static final JoinKind ANTI` |
| `public static final JoinKind INNER` |
| `public static final JoinKind LEFT` |
| `public static final JoinKind SEMI` |

</details>

<details>
<summary>io.fengqudb.Select.Op</summary>

| 声明 |
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

| 声明 |
| --- |
| `public boolean descending` |
| `public Field field` |
| `public boolean nullsFirst` |

</details>

<details>
<summary>io.fengqudb.Select.Result</summary>

| 声明 |
| --- |
| `public List<Access> access` |
| `public byte[] cursor` |
| `public List<Row> rows` |
| `public Stats stats` |

</details>

<details>
<summary>io.fengqudb.Select.Row</summary>

| 声明 |
| --- |
| `public byte[] key` |
| `public List<Value> values` |

</details>

<details>
<summary>io.fengqudb.Select.Stats</summary>

| 声明 |
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

| 声明 |
| --- |
| `public long bits` |
| `public byte[] bytes` |
| `public int kind` |
| `public double real` |

</details>

<details>
<summary>io.fengqudb.Store</summary>

| 声明 |
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

| 声明 |
| --- |
| `public boolean changed` |
| `public boolean matched` |
| `public long observedVersion` |
| `public long value` |
| `public long version` |

</details>

<details>
<summary>io.fengqudb.Store.Change</summary>

| 声明 |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] key` |
| `public final int kind` |
| `public final byte[] value` |

</details>

<details>
<summary>io.fengqudb.Store.Changes</summary>

| 声明 |
| --- |
| `public final long durableVersion` |
| `public final int flags` |
| `public final Change[] rows` |
| `public final long unixMs` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.CommandResult</summary>

| 声明 |
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

| 声明 |
| --- |
| `public final int code` |

</details>

<details>
<summary>io.fengqudb.Store.Entry</summary>

| 声明 |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] value` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.ExpiryInfo</summary>

| 声明 |
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

| 声明 |
| --- |
| `public long maxBatchBytes` |
| `public int maxKeys` |
| `public long timeoutMs` |

</details>

<details>
<summary>io.fengqudb.Store.FeatureRow</summary>

| 声明 |
| --- |
| `public byte[] key` |
| `public double score` |
| `public byte[] value` |

</details>

<details>
<summary>io.fengqudb.Store.HistoryInfo</summary>

| 声明 |
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

| 声明 |
| --- |
| `public final int field` |
| `public final int type` |

</details>

<details>
<summary>io.fengqudb.Store.IndexQuery</summary>

| 声明 |
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

| 声明 |
| --- |
| `public final long delta` |
| `public final byte[] key` |
| `public final long maximum` |
| `public final long minimum` |
| `public final long offset` |

</details>

<details>
<summary>io.fengqudb.Store.MaintenanceIO</summary>

| 声明 |
| --- |
| `public long readBytes` |
| `public long syncCalls` |
| `public long writeBytes` |

</details>

<details>
<summary>io.fengqudb.Store.MaintenanceInfo</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] key` |
| `public final byte[] value` |

</details>

<details>
<summary>io.fengqudb.Store.Row</summary>

| 声明 |
| --- |
| `public final long expiresAtMs` |
| `public final byte[] key` |
| `public final byte[] value` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.RuntimeInfo</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `public final long expiresAtMs` |
| `public final long version` |

</details>

<details>
<summary>io.fengqudb.Store.WatchOptions</summary>

| 声明 |
| --- |
| `public boolean includeSystem` |
| `public long maxBatchBytes` |
| `public int maxScanCommits` |
| `public boolean metadataOnly` |

</details>

<details>
<summary>io.fengqudb.Store.WriteMode</summary>

| 声明 |
| --- |
| `public static final WriteMode PERFORMANCE` |
| `public static final WriteMode RELIABLE` |

</details>

<details>
<summary>io.fengqudb.Store.WriteOptions</summary>

| 声明 |
| --- |
| `public WriteMode mode` |
| `public long periodMs` |
| `public long thresholdBytes` |

</details>

<details>
<summary>io.fengqudb.Store.WriteState</summary>

| 声明 |
| --- |
| `public final long durableVersion` |
| `public final long lastStatus` |
| `public final long pendingBytes` |
| `public final long version` |

</details>
