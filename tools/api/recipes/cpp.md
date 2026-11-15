<!-- api:read -->
## 1. 读取：按键找数据

```cpp
if (auto value = db.get("student/7")) {
    auto row = Codec::decode(value->view());
    auto grade = row.value.score;
}
```

没有记录时返回空 optional。本文 Codec 指接入文档生成的 StudentFqCodec。

<!-- api:save -->
## 2. 新增或保存

```cpp
Student student{7, "Alice", 95};
db.put(Codec::key(student), Codec::encode(student));
db.put("name", "Alice"); // 单个字符串可以直接存 UTF-8
```

同键 put 会覆盖旧值。

<!-- api:modify -->
## 3. 修改与原子操作

```cpp
if (auto bytes = db.get("student/7")) {
    auto row = Codec::decode(bytes->view());
    row.value.score = 96;
    db.put(Codec::key(row.value), Codec::encode(row));
}
db.add_i64("login-count", 1);
```

add_i64 使用八字节小端整数；不能对实体编码直接加减。compare_exchange 按版本修改，put_if_absent 仅在不存在时新增，patch 适合原始字节区间修改。

<!-- api:delete -->
## 4. 删除

```cpp
db.erase("student/7");
```

<!-- api:batch -->
## 5. 批量操作

```cpp
db.put_many({{"a", "one"}, {"b", "two"}});
auto values = db.get_many({"a", "b"});
```

put_many 是一笔原子批写。

<!-- api:transaction -->
## 6. 事务：一组修改一起提交

```cpp
auto tx = db.transaction();
tx.put("bag/item", "sword");
tx.erase("reward/pending");
auto result = tx.commit();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));
```

未提交的事务析构时丢弃。CONFLICT 时重建事务并重新读取计算。

<!-- api:query -->
## 7. 筛选、排序和统计

```cpp
#include <fengqudb_query.hpp>
namespace q = fengqudb::q;
auto score = q::s64(Codec::Field_score);
auto name = q::text(Codec::Field_name);
auto query = q::from("student/").where(score >= 90)
    .select(name, score).order_by(score.desc()).limit(10);
auto rows = q::run(db, query);
for (auto row : rows) { auto grade = row[score]; }
```

支持组合条件、聚合、分组及等值关联；prepare 复用同一查询，explain 查看查询计划。

<!-- api:index -->
## 8. 索引：经常按字段查询

```cpp
db.define_index("student-score", "student/", Codec::Field_score, 5);
```

字段类型5表示有符号 ZigZag 整数。其他类型与索引选项见本节参数。

<!-- api:durability -->
## 9. 可靠保存

默认性能模式为 50 ms / 1 MiB，普通写入在内存生效后返回。关键写传最后的可靠标志：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```cpp
db.put("payment/receipt", "paid", 0, true);
db.sync();
auto pending = db.submit_put("payment/next", "paid", 0, true);
auto result = pending.wait();
```

UI 线程可保存凭据、稍后 poll。COMMIT_UNKNOWN 时先核对业务编号，不要盲目重放付款。

<!-- api:expiry -->
## 10. 自动到期

```cpp
db.put("verification", "1234", expires_at_unix_ms);
```

时间为绝对 Unix 毫秒；0 不过期。

<!-- api:fastread -->
## 11. 热点读取

```cpp
auto key = db.prepare_key("student/7");
auto value = key.get();
auto reader = db.prepare_reader({"student/7", "student/8"});
auto rows = reader.read();
```

共享字节是只读借用，保持拥有对象存活。读取器的行只保留到下次 read/release；要长时间保存则复制。

<!-- api:snapshot -->
## 12. 一致快照

```cpp
auto snapshot = db.snapshot();
auto a = snapshot.get("a");
auto b = snapshot.get("b");
```

<!-- api:backup -->
## 13. 备份恢复

```cpp
db.backup("backups/full-001");
```

使用新的目标目录。恢复时使用 restore/recover_chain，先关闭要替换的库，不直接复制正在写入的文件。

<!-- api:collections -->
## 14. 集合操作

execute 接收 Hash/Set/List/有序集合命令，hash_get 读取 Hash 字段。用于原子增删成员、队列和排名。

<!-- api:maintenance -->
## 15. 清理和整理

```cpp
db.purge_expired();
db.compact();
db.verify();
```

<!-- api:watch -->
## 16. 消费变更

watch 创建变更消费者；处理后确认水位。用于进程内缓存更新和后台任务，句柄由 RAII 管理。

<!-- api:serialization -->
## 17. 实体编解码

Codec::encode/encode_into 直接访问自己的结构体；decode 返回原结构及未知字段。读改写用 encode(row)，避免丢掉新版本添加的字段。可复用 string 或调用方缓冲，输出不要与输入字段重叠。
