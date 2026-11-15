<!-- api:read -->
## 1. 读取：按键找档案

```python
data = db.get(b"student/7")
student = None if data is None else Codec.decode(data).value
```

缺失返回 None，空 bytes 是有效数据。本文的 Codec 是接入文档生成的 StudentFqCodec。

<!-- api:save -->
## 2. 新增或保存：档案、数字和字符串

```python
student = Student(7, "张三", 95)
db.put(Codec.key(student), Codec.encode(student))
db.put(b"name", "张三".encode("utf-8"))
db.put(b"score", (95).to_bytes(8, "little", signed=True))
score = int.from_bytes(db.get(b"score"), "little", signed=True)
```

同键 Put 覆盖旧值。单个字符串和整数不需要定义实体；上面的整数格式也可用于 add_i64。

<!-- api:modify -->
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

<!-- api:delete -->
## 4. 删除

```python
db.delete(b"student/7")
```

<!-- api:batch -->
## 5. 批量：一次读写多条记录

```python
values = db.get_many([b"student/7", b"student/8"])
db.put_many([(b"a", b"one"), (b"b", b"two")])
db.delete_many([b"a", b"b"])
```

put_many/delete_many 中的修改作为一笔原子操作提交。

<!-- api:transaction -->
## 6. 事务：一组修改一起成功

```python
with db.transaction() as tx:
    tx.put(b"bag/item", b"sword")
    tx.delete(b"reward/pending")
```

with 正常退出自动提交；异常退出丢弃。需要中途回退可使用 savepoint/rollback_to。CONFLICT 时重新创建事务并重新读取计算。

<!-- api:query -->
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

<!-- api:index -->
## 8. 索引：反复按同一字段查询

```python
db.define_index(b"student-score", b"student/", Codec.Fields.score, 5)
```

5 表示有符号 ZigZag 整数。复合索引和唯一/必填限制使用 define_compound_index 的选项。

<!-- api:durability -->
## 9. 关键写入：等待可靠落盘

默认性能模式为 50 ms / 1 MiB，普通成功表示内存已生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```python
db.put(b"payment/receipt", b"paid", force_reliable=True)
db.sync()
db.configure_write(mode="reliable") # 整个库每笔都等待可靠确认
```

异步 submit_put 返回凭据，需等待或轮询其结果。COMMIT_UNKNOWN 时先按业务编号核对，不要直接重复扣款。

<!-- api:expiry -->
## 10. 到期数据

```python
import time
expires = int(time.time() * 1000) + 5 * 60_000
db.put(b"verification", b"1234", expires_at_ms=expires)
```

到期时间是绝对 Unix 毫秒，0 表示不过期。

<!-- api:fastread -->
## 11. 热点和共享读取

```python
with db.prepare_reader([b"student/7", b"student/8"]) as reader:
    rows = reader.read()
```

普通 get 返回独立 bytes。共享值需要 close；读取器返回的内存视图只保留到下一次 read/release/close。跨次保存请复制。

<!-- api:snapshot -->
## 12. 一致快照

```python
with db.snapshot() as snapshot:
    first = snapshot.get(b"a")
    second = snapshot.get(b"b")
```

<!-- api:backup -->
## 13. 备份与恢复

```python
db.backup("backups/full-001")
```

目标使用新目录。恢复调用 restore/recover_chain，替换目标前先关闭数据库；不要复制正在写入的目录文件。

<!-- api:collections -->
## 14. 原生集合

execute 执行 Hash/Set/List/有序集合命令，hash_get 读取 Hash 字段。适合原子增加成员、出入队和维护排名。

<!-- api:maintenance -->
## 15. 清理、整理、校验

```python
db.purge_expired(max_keys=1000)
db.compact()
db.verify()
```

<!-- api:watch -->
## 16. 订阅提交变化

watch 返回消费者；读取后 ack 确认水位，用完 close。适合进程内缓存更新和后台任务。

<!-- api:serialization -->
## 17. 实体编解码

Codec.encode 接收 Student，decode 返回包含原 Student 的记录。修改后用 row.fq_encode 保留未知字段。encode_into 可复用 bytearray，复用前 clear。生成只在开发启动或构建时运行，不放在每次读写中。
