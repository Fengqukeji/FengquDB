# Python API 与调用例子

[接入文档](../languages/python.md) · [English](../en/api/python.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取：按键找档案

```python
data = db.get(b"student/7")
student = None if data is None else Codec.decode(data).value
```

缺失返回 None，空 bytes 是有效数据。本文的 Codec 是接入文档生成的 StudentFqCodec。

<details>
<summary>函数与参数（3项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def get(self, key)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `def get_entries(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `def get_entry(self, key)` | 读取对应的数据或状态 |

</details>

## 2. 新增或保存：档案、数字和字符串

```python
student = Student(7, "张三", 95)
db.put(Codec.key(student), Codec.encode(student))
db.put(b"name", "张三".encode("utf-8"))
db.put(b"score", (95).to_bytes(8, "little", signed=True))
score = int.from_bytes(db.get(b"score"), "little", signed=True)
```

同键 Put 覆盖旧值。单个字符串和整数不需要定义实体；上面的整数格式也可用于 add_i64。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def put(self, key, value, *, expires_at_ms=0, force_reliable=False)` | 保存或设置数据 |

</details>

## 3. 修改：只改业务需要的内容

```python
data = db.get(b"student/7")
if data is not None:
    row = Codec.decode(data)
    row.value.score = 96
    db.put(row.fq_key(), row.fq_encode())
db.add_i64(b"login-count", 1) # 并发计数用原子加减
```

patch 修改原始二进制的指定区间；compare_exchange 按版本更新；put_if_absent 防止覆盖已存在的记录。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def add_i64(self, key, delta, minimum=-(1 << 63), maximum=(1 << 63) - 1, timeout_ms=0, *, force_reliable=False)` | 条件更新或局部修改 |
| `fengqudb.Store` | `def add_i64_fields(self, items, timeout_ms=0, *, force_reliable=False)` | 条件更新或局部修改 |
| `fengqudb.Store` | `def compare_exchange(self, key, expected_version, value, expires_at_ms=0, timeout_ms=0, *, force_reliable=False)` | 条件更新或局部修改 |
| `fengqudb.Store` | `def delete_if_version(self, key, expected_version, timeout_ms=0, *, force_reliable=False)` | 删除记录或成员 |
| `fengqudb.Store` | `def patch(self, key, offset, remove, insert, *, force_reliable=False)` | 条件更新或局部修改 |
| `fengqudb.Store` | `def put_if_absent(self, key, value, expires_at_ms=0, timeout_ms=0, *, force_reliable=False)` | 保存或设置数据 |
| `fengqudb.Store` | `def put_if_changed(self, key, value, expires_at_ms=0, timeout_ms=0, *, force_reliable=False)` | 保存或设置数据 |

</details>

## 4. 删除

```python
db.delete(b"student/7")
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def delete(self, key, *, force_reliable=False)` | 删除记录或成员 |

</details>

## 5. 批量：一次读写多条记录

```python
values = db.get_many([b"student/7", b"student/8"])
db.put_many([(b"a", b"one"), (b"b", b"two")])
db.delete_many([b"a", b"b"])
```

put_many/delete_many 中的修改作为一笔原子操作提交。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def delete_many(self, keys, *, force_reliable=False)` | 删除记录或成员 |
| `fengqudb.Store` | `def get_many(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `def prepare_batch(self, items)` | 批量处理 |
| `fengqudb.Store` | `def put_batch(self, batch, *, force_reliable=False)` | 保存或设置数据 |
| `fengqudb.Store` | `def put_many(self, items, *, force_reliable=False)` | 保存或设置数据 |

</details>

## 6. 事务：一组修改一起成功

```python
with db.transaction() as tx:
    tx.put(b"bag/item", b"sword")
    tx.delete(b"reward/pending")
```

with 正常退出自动提交；异常退出丢弃。需要中途回退可使用 savepoint/rollback_to。CONFLICT 时重新创建事务并重新读取计算。

<details>
<summary>函数与参数（26项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def transaction(self, timeout_ms=0)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def __enter__(self)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def __exit__(self, *_)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def __exit__(self, kind, *_)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def __init__(self, native, handle)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def cancel(self)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreTransaction` | `def commit(self, force_reliable=False)` | 提交事务或查看提交结果 |
| `fengqudb.StoreTransaction` | `def delete(self, key)` | 删除记录或成员 |
| `fengqudb.StoreTransaction` | `def delete_many(self, keys)` | 删除记录或成员 |
| `fengqudb.StoreTransaction` | `def execute(self, operation, scope, key=b'', value=b'', **options)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def expire(self, key, expires_at_ms)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def get(self, key)` | 读取对应的数据或状态 |
| `fengqudb.StoreTransaction` | `def get_entries(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.StoreTransaction` | `def get_many(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.StoreTransaction` | `def patch(self, key, offset, remove, insert)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def project(self, key, fields)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def put(self, key, value, expires_at_ms=0)` | 保存或设置数据 |
| `fengqudb.StoreTransaction` | `def put_batch(self, batch)` | 保存或设置数据 |
| `fengqudb.StoreTransaction` | `def put_many(self, items)` | 保存或设置数据 |
| `fengqudb.StoreTransaction` | `def query_index(self, name, *, cursor=b'', limit=256, **query)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def release(self, savepoint)` | 释放对象或借用的资源 |
| `fengqudb.StoreTransaction` | `def rollback_to(self, savepoint)` | 回退到保存点 |
| `fengqudb.StoreTransaction` | `def savepoint(self)` | 创建或操作事务保存点 |
| `fengqudb.StoreTransaction` | `def scan(self, begin=b'', end=b'', *, reverse=False)` | 事务操作 |
| `fengqudb.StoreTransaction` | `def submit(self, force_reliable=False)` | 事务操作 |

</details>

## 7. 条件筛选、排序和统计

```python
from fengqudb_query import from_, s64, text
score = s64(Codec.Fields.score)
name = text(Codec.Fields.name)
rows = from_(b"student/").where(score >= 90).select(name, score) \
    .order_by(score.desc()).limit(10).run(db)
for row in rows:
    print(row[name], row[score])
```

组合条件用 `& / | / ~`，不要用 Python 的 and/or/not。group_by/having 做分组统计，join 做字段等值关联，explain 查看查询计划。

<details>
<summary>函数与参数（107项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def select_raw(self, operation, value)` | 查询构建或结果读取 |
| `fengqudb_query` | `def avg(col)` | 查询构建或结果读取 |
| `fengqudb_query` | `def boolean(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def count()` | 查询构建或结果读取 |
| `fengqudb_query` | `def count_distinct(col)` | 查询构建或结果读取 |
| `fengqudb_query` | `def count_of(col)` | 查询构建或结果读取 |
| `fengqudb_query` | `def f32(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def f64(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def fixed32(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def fixed64(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def from_(prefix)` | 查询构建或结果读取 |
| `fengqudb_query` | `def i64(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def key(source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def max_(col)` | 查询构建或结果读取 |
| `fengqudb_query` | `def min_(col)` | 查询构建或结果读取 |
| `fengqudb_query` | `def nested(path, type, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def s64(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def sfixed32(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def sfixed64(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def sum_(col)` | 查询构建或结果读取 |
| `fengqudb_query` | `def text(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query` | `def u64(n, source=0)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __eq__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __ge__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __gt__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __init__(self, column)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __le__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __lt__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def __ne__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def asc(self, nulls_first=True)` | 查询构建或结果读取 |
| `fengqudb_query.Agg` | `def desc(self, nulls_first=True)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def __init__(self, prefix)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def after(self, cursor)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def allow_scan(self, on)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def build(self)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def count_total(self, on=True)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def distinct(self, on=True)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def explain(self, database)` | 查看查询计划 |
| `fengqudb_query.Builder` | `def group_by(self, *cols)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def having(self, cond)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def index(self, name)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def join(self, prefix, left, right, kind=JoinKind.INNER)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def limit(self, n)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def memory(self, n)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def offset(self, n)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def order_by(self, *items)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def prepare(self)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def run(self, database)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def select(self, *items)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def timeout_ms(self, n)` | 查询构建或结果读取 |
| `fengqudb_query.Builder` | `def where(self, cond)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __eq__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __ge__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __gt__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __init__(self, field, cast=None)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __le__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __lt__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def __ne__(self, v)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def any(self)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def asc(self, nulls_first=True)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def between(self, low, high)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def contains(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def desc(self, nulls_first=True)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def ends_with(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def exists(self)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def fuzzy(self, s, distance)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def icontains(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def ilike(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def in_(self, values)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def is_null(self)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def like(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def regex(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Col` | `def starts_with(self, s)` | 查询构建或结果读取 |
| `fengqudb_query.Cond` | `def __and__(self, other)` | 查询构建或结果读取 |
| `fengqudb_query.Cond` | `def __bool__(self)` | 查询构建或结果读取 |
| `fengqudb_query.Cond` | `def __init__(self, resolve, aggregate=False)` | 查询构建或结果读取 |
| `fengqudb_query.Cond` | `def __invert__(self)` | 查询构建或结果读取 |
| `fengqudb_query.Cond` | `def __or__(self, other)` | 查询构建或结果读取 |
| `fengqudb_query.Cond` | `def build(self, columns)` | 查询构建或结果读取 |
| `fengqudb_query.Prepared` | `def __init__(self, prepared, columns)` | 查询构建或结果读取 |
| `fengqudb_query.Prepared` | `def run(self, database)` | 查询构建或结果读取 |
| `fengqudb_query.Rows` | `def __getitem__(self, i)` | 读取对应的数据或状态 |
| `fengqudb_query.Rows` | `def __init__(self, columns, result)` | 查询构建或结果读取 |
| `fengqudb_query.Rows` | `def __iter__(self)` | 查询构建或结果读取 |
| `fengqudb_query.Rows` | `def __len__(self)` | 查询构建或结果读取 |
| `fengqudb_select.Column` | `def encode(self)` | 查询构建或结果读取 |
| `fengqudb_select.Expr` | `def __and__(self, other)` | 查询构建或结果读取 |
| `fengqudb_select.Expr` | `def __invert__(self)` | 查询构建或结果读取 |
| `fengqudb_select.Expr` | `def __or__(self, other)` | 查询构建或结果读取 |
| `fengqudb_select.Expr` | `def encode(self, depth=0)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def any(self)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def column(index)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def compare_field(self, op, other)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def encode(self)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def key(source=0)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def record(source=0)` | 查询构建或结果读取 |
| `fengqudb_select.Field` | `def test(self, op, *values)` | 查询构建或结果读取 |
| `fengqudb_select.Join` | `def encode(self)` | 查询构建或结果读取 |
| `fengqudb_select.Order` | `def encode(self)` | 查询构建或结果读取 |
| `fengqudb_select.PreparedQuery` | `def __post_init__(self)` | 查询构建或结果读取 |
| `fengqudb_select.PreparedQuery` | `def execute(self, database)` | 查询构建或结果读取 |
| `fengqudb_select.PreparedQuery` | `def explain(self, database)` | 查看查询计划 |
| `fengqudb_select.Query` | `def encode(self)` | 查询构建或结果读取 |
| `fengqudb_select.Query` | `def execute(self, database)` | 查询构建或结果读取 |
| `fengqudb_select.Query` | `def explain(self, database)` | 查看查询计划 |
| `fengqudb_select.Query` | `def prepare(self)` | 查询构建或结果读取 |
| `fengqudb_select.Result` | `def __init__(self, data)` | 查询构建或结果读取 |

</details>

## 8. 索引：反复按同一字段查询

```python
db.define_index(b"student-score", b"student/", Codec.Fields.score, 5)
```

5 表示有符号 ZigZag 整数。复合索引和唯一/必填限制使用 define_compound_index 的选项。

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def define_compound_index(self, name, key_prefix, fields, flags=0, **options)` | 定义或使用索引 |
| `fengqudb.Store` | `def define_index(self, name, key_prefix, field, field_type, flags=0, **options)` | 定义或使用索引 |
| `fengqudb.Store` | `def drop_index(self, name, **options)` | 定义或使用索引 |
| `fengqudb.Store` | `def query_index(self, name, *, cursor=b'', limit=256, **query)` | 定义或使用索引 |

</details>

## 9. 关键写入：等待可靠落盘

默认性能模式为 50 ms / 1 MiB，普通成功表示内存已生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```python
db.put(b"payment/receipt", b"paid", force_reliable=True)
db.sync()
db.configure_write(mode="reliable") # 整个库每笔都等待可靠确认
```

异步 submit_put 返回凭据，需等待或轮询其结果。COMMIT_UNKNOWN 时先按业务编号核对，不要直接重复扣款。

<details>
<summary>函数与参数（16项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def configure_write(self, *, mode=StoreWriteMode.PERFORMANCE, period_ms=50, threshold_bytes=1048576)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `async def configure_write_async(self, **options)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `def reset_write_options(self)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `async def reset_write_options_async(self)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `def submit_put(self, key, value, *, expires_at_ms=0, force_reliable=False)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `def sync(self)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `def write_options(self)` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `def write_state(self)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `def __enter__(self)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `def __exit__(self, *_)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `def __init__(self, native, handle)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `def cancel(self)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreCommit` | `def poll(self)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `def wait(self, timeout_ms=0)` | 控制可靠确认或提交等待 |
| `fengqudb.StoreCommit` | `async def wait_async(self, poll_interval=0.005)` | 控制可靠确认或提交等待 |

</details>

## 10. 到期数据

```python
import time
expires = int(time.time() * 1000) + 5 * 60_000
db.put(b"verification", b"1234", expires_at_ms=expires)
```

到期时间是绝对 Unix 毫秒，0 表示不过期。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def purge_expired(self, *, max_keys=1024, timeout_ms=0, max_batch_bytes=1048576)` | 到期数据操作 |

</details>

## 11. 热点和共享读取

```python
with db.prepare_reader([b"student/7", b"student/8"]) as reader:
    rows = reader.read()
```

普通 get 返回独立 bytes。共享值需要 close；读取器返回的内存视图只保留到下一次 read/release/close。跨次保存请复制。

<details>
<summary>函数与参数（36项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def get_many_shared(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `def get_shared(self, key)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `def prepare_key(self, key)` | 复用键、缓冲或共享字节 |
| `fengqudb.Store` | `def prepare_reader(self, keys)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreBufferReader` | `def __del__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreBufferReader` | `def __enter__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreBufferReader` | `def __exit__(self, *_)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreBufferReader` | `def __init__(self, key, destination)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreBufferReader` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreBufferReader` | `def read(self)` | 读取对应的数据或状态 |
| `fengqudb.StorePreparedKey` | `def __enter__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StorePreparedKey` | `def __exit__(self, *_)` | 复用键、缓冲或共享字节 |
| `fengqudb.StorePreparedKey` | `def __init__(self, native, handle)` | 复用键、缓冲或共享字节 |
| `fengqudb.StorePreparedKey` | `def bind_buffer(self, destination)` | 复用键、缓冲或共享字节 |
| `fengqudb.StorePreparedKey` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StorePreparedKey` | `def get(self)` | 读取对应的数据或状态 |
| `fengqudb.StorePreparedKey` | `def get_entry(self)` | 读取对应的数据或状态 |
| `fengqudb.StorePreparedKey` | `def get_into(self, destination)` | 读取对应的数据或状态 |
| `fengqudb.StorePreparedKey` | `def get_shared(self)` | 读取对应的数据或状态 |
| `fengqudb.StoreReader` | `def __enter__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreReader` | `def __exit__(self, *_)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreReader` | `def __init__(self, native, handle, count)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreReader` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreReader` | `def read_shared(self)` | 读取对应的数据或状态 |
| `fengqudb.StoreReader` | `def release(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreSharedBatch` | `def __enter__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedBatch` | `def __exit__(self, *_)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedBatch` | `def __getitem__(self, index)` | 读取对应的数据或状态 |
| `fengqudb.StoreSharedBatch` | `def __init__(self, lease, count)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedBatch` | `def __len__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedBatch` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreSharedValue` | `def __enter__(self)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedValue` | `def __exit__(self, *_)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedValue` | `def __init__(self, row, lease)` | 复用键、缓冲或共享字节 |
| `fengqudb.StoreSharedValue` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreSharedValue` | `def value(self)` | 复用键、缓冲或共享字节 |

</details>

## 12. 一致快照

```python
with db.snapshot() as snapshot:
    first = snapshot.get(b"a")
    second = snapshot.get(b"b")
```

<details>
<summary>函数与参数（14项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def snapshot(self)` | 一致读取或范围遍历 |
| `fengqudb.StoreCursor` | `def __enter__(self)` | 一致读取或范围遍历 |
| `fengqudb.StoreCursor` | `def __exit__(self, *_)` | 一致读取或范围遍历 |
| `fengqudb.StoreCursor` | `def __iter__(self)` | 一致读取或范围遍历 |
| `fengqudb.StoreCursor` | `def __next__(self)` | 一致读取或范围遍历 |
| `fengqudb.StoreCursor` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreSnapshot` | `def __enter__(self)` | 一致读取或范围遍历 |
| `fengqudb.StoreSnapshot` | `def __exit__(self, *_)` | 一致读取或范围遍历 |
| `fengqudb.StoreSnapshot` | `def __init__(self, native, handle)` | 一致读取或范围遍历 |
| `fengqudb.StoreSnapshot` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreSnapshot` | `def get(self, key)` | 读取对应的数据或状态 |
| `fengqudb.StoreSnapshot` | `def get_entries(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.StoreSnapshot` | `def get_many(self, keys)` | 读取对应的数据或状态 |
| `fengqudb.StoreSnapshot` | `def scan(self, begin=b'', end=b'', *, reverse=False)` | 一致读取或范围遍历 |

</details>

## 13. 备份与恢复

```python
db.backup("backups/full-001")
```

目标使用新目录。恢复调用 restore/recover_chain，替换目标前先关闭数据库；不要复制正在写入的目录文件。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def backup(self, destination, **options)` | 备份、恢复或历史保留 |
| `fengqudb.Store` | `def backup_incremental(self, destination, from_version, **options)` | 备份、恢复或历史保留 |
| `fengqudb.Store` | `def history_info(self)` | 备份、恢复或历史保留 |
| `fengqudb.Store` | `def hold_history(self, name, from_version=None, timeout_ms=0)` | 备份、恢复或历史保留 |
| `fengqudb.Store` | `def recover_chain(base, archives, destination, *, target_version=0, target_unix_ms=0, library=None, **options)` | 备份、恢复或历史保留 |
| `fengqudb.Store` | `def release_history(self, name, timeout_ms=0)` | 释放对象或借用的资源 |
| `fengqudb.Store` | `def restore(source, destination, *, library=None, **options)` | 备份、恢复或历史保留 |

</details>

## 14. 原生集合

execute 执行 Hash/Set/List/有序集合命令，hash_get 读取 Hash 字段。适合原子增加成员、出入队和维护排名。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def execute(self, operation, scope, key=b'', value=b'', *, force_reliable=False, **options)` | 原生集合操作 |
| `fengqudb.Store` | `def hash_get(self, scope, key)` | 读取对应的数据或状态 |

</details>

## 15. 清理、整理、校验

```python
db.purge_expired(max_keys=1000)
db.compact()
db.verify()
```

<details>
<summary>函数与参数（6项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def checkpoint(self)` | 查看状态或维护数据库 |
| `fengqudb.Store` | `def compact(self, **options)` | 查看状态或维护数据库 |
| `fengqudb.Store` | `def configure(self, *, background=False, interval_ms=200, expiry_keys=256, group_max=128, group_delay_us=0, garbage_percent=30, compact_min_bytes=33554432, compact_growth_bytes=33554432, job_timeout_ms=1000, job_max_read_bytes=0, job_max_write_bytes=0)` | 查看状态或维护数据库 |
| `fengqudb.Store` | `def info(self)` | 查看状态或维护数据库 |
| `fengqudb.Store` | `def runtime_info(self)` | 查看状态或维护数据库 |
| `fengqudb.Store` | `def verify(self, **options)` | 查看状态或维护数据库 |

</details>

## 16. 订阅提交变化

watch 返回消费者；读取后 ack 确认水位，用完 close。适合进程内缓存更新和后台任务。

<details>
<summary>函数与参数（10项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `def watch(self, from_version=None, prefix=b'', *, metadata_only=False, include_system=False, max_scan_commits=256, max_batch_bytes=67108864)` | 消费变更或定位问题 |
| `fengqudb.StoreChange` | `def __init__(self, key, value, kind, expires_at_ms)` | 消费变更或定位问题 |
| `fengqudb.StoreChanges` | `def __init__(self, data)` | 消费变更或定位问题 |
| `fengqudb.StoreWatch` | `def __enter__(self)` | 消费变更或定位问题 |
| `fengqudb.StoreWatch` | `def __exit__(self, *_)` | 消费变更或定位问题 |
| `fengqudb.StoreWatch` | `def __init__(self, native, handle)` | 消费变更或定位问题 |
| `fengqudb.StoreWatch` | `def ack(self, version)` | 消费变更或定位问题 |
| `fengqudb.StoreWatch` | `def cancel(self)` | 消费变更或定位问题 |
| `fengqudb.StoreWatch` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreWatch` | `def next_batch(self, timeout_ms=0)` | 消费变更或定位问题 |

</details>

## 17. 实体编解码

Codec.encode 接收 Student，decode 返回包含原 Student 的记录。修改后用 row.fq_encode 保留未知字段。encode_into 可复用 bytearray，复用前 clear。生成只在开发启动或构建时运行，不放在每次读写中。

## 其他辅助接口

<details>
<summary>函数与参数（18项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.BufferTooSmall` | `def __init__(self, required_size)` | 辅助操作 |
| `fengqudb.Error` | `def __init__(self, code, message)` | 辅助操作 |
| `fengqudb.Store` | `def __enter__(self)` | 辅助操作 |
| `fengqudb.Store` | `def __exit__(self, *_)` | 辅助操作 |
| `fengqudb.Store` | `def __init__(self, path, *, name=None, library=None, buffered=False, create=True, segmented=False, cache_bytes=64 * 1024 * 1024, delta_bytes=4 * 1024 * 1024, writer_bytes=32 * 1024 * 1024, transaction_bytes=64 * 1024 * 1024, all_transaction_bytes=256 * 1024 * 1024, pinned_bytes=128 * 1024 * 1024, max_snapshots=1024)` | 辅助操作 |
| `fengqudb.Store` | `def cancel_maintenance(self)` | 辅助操作 |
| `fengqudb.Store` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.Store` | `def open_at(cls, root, name, **options)` | 辅助操作 |
| `fengqudb.Store` | `def path_at(root, name, *, library=None)` | 辅助操作 |
| `fengqudb.Store` | `def prepare_project_reader(self, keys, fields)` | 辅助操作 |
| `fengqudb.Store` | `def project(self, key, fields)` | 辅助操作 |
| `fengqudb.StoreBatch` | `def __enter__(self)` | 辅助操作 |
| `fengqudb.StoreBatch` | `def __exit__(self, *_)` | 辅助操作 |
| `fengqudb.StoreBatch` | `def __init__(self, native, handle)` | 辅助操作 |
| `fengqudb.StoreBatch` | `def close(self)` | 释放对象或借用的资源 |
| `fengqudb.StoreCommandResult` | `def __init__(self)` | 辅助操作 |
| `fengqudb.StoreEntry` | `def __init__(self, value, version, expires_at_ms)` | 辅助操作 |
| `fengqudb.StoreWriteOptions` | `def __init__(self, mode=StoreWriteMode.PERFORMANCE, period_ms=50, threshold_bytes=1048576)` | 辅助操作 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>fengqudb</summary>

| 声明 |
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

| 声明 |
| --- |
| `required_size` |

</details>

<details>
<summary>fengqudb.Error</summary>

| 声明 |
| --- |
| `code` |

</details>

<details>
<summary>fengqudb.Slice</summary>

| 声明 |
| --- |
| `data: C.c_void_p` |
| `size: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreAtomicInfo</summary>

| 声明 |
| --- |
| `changed: C.c_uint32` |
| `matched: C.c_uint32` |
| `observed_version: C.c_uint64` |
| `value: C.c_int64` |
| `version: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreChanges</summary>

| 声明 |
| --- |
| `rows` |

</details>

<details>
<summary>fengqudb.StoreCommandResult</summary>

| 声明 |
| --- |
| `cursor` |

</details>

<details>
<summary>fengqudb.StoreIndexField</summary>

| 声明 |
| --- |
| `field: C.c_uint32` |
| `type: C.c_uint32` |

</details>

<details>
<summary>fengqudb.StoreMaintenanceIO</summary>

| 声明 |
| --- |
| `read_bytes: C.c_uint64` |
| `sync_calls: C.c_uint64` |
| `write_bytes: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreWriteMode</summary>

| 声明 |
| --- |
| `PERFORMANCE = 1` |
| `RELIABLE = 0` |

</details>

<details>
<summary>fengqudb.StoreWriteOptions</summary>

| 声明 |
| --- |
| `abi_version: C.c_uint32` |
| `mode: C.c_uint32` |
| `period_ms: C.c_uint32` |
| `struct_size: C.c_uint32` |
| `threshold_bytes: C.c_uint64` |

</details>

<details>
<summary>fengqudb.StoreWriteState</summary>

| 声明 |
| --- |
| `durable_version: C.c_uint64` |
| `last_status: C.c_uint64` |
| `pending_bytes: C.c_uint64` |
| `version: C.c_uint64` |

</details>

<details>
<summary>fengqudb_query</summary>

| 声明 |
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

| 声明 |
| --- |
| `column` |

</details>

<details>
<summary>fengqudb_select</summary>

| 声明 |
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
<summary>fengqudb_select.Column</summary>

| 声明 |
| --- |
| `aggregate: Aggregate = Aggregate.NONE` |
| `distinct: bool = False` |
| `field: Field = Field()` |
| `name: str = ''` |

</details>

<details>
<summary>fengqudb_select.Expr</summary>

| 声明 |
| --- |
| `children: tuple = ()` |
| `field: Field = Field()` |
| `op: Op = Op.TRUE` |
| `right: object = None` |
| `values: tuple = ()` |

</details>

<details>
<summary>fengqudb_select.Field</summary>

| 声明 |
| --- |
| `number: int = 0` |
| `path: tuple = ()` |
| `repeated: bool = False` |
| `source: int = 0` |
| `type: int = 4` |

</details>

<details>
<summary>fengqudb_select.Join</summary>

| 声明 |
| --- |
| `kind: JoinKind = JoinKind.INNER` |
| `left: tuple` |
| `on: Expr = Expr()` |
| `prefix: bytes` |
| `right: tuple` |

</details>

<details>
<summary>fengqudb_select.JoinKind</summary>

| 声明 |
| --- |
| `ANTI = 3` |
| `INNER = 0` |
| `LEFT = 1` |
| `SEMI = 2` |

</details>

<details>
<summary>fengqudb_select.Op</summary>

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
<summary>fengqudb_select.Order</summary>

| 声明 |
| --- |
| `descending: bool = False` |
| `field: Field` |
| `nulls_first: bool = True` |

</details>

<details>
<summary>fengqudb_select.PreparedQuery</summary>

| 声明 |
| --- |
| `encoded: bytes` |

</details>

<details>
<summary>fengqudb_select.Query</summary>

| 声明 |
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

| 声明 |
| --- |
| `access` |
| `cursor` |
| `rows` |
| `stats` |

</details>
