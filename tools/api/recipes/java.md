<!-- api:read -->
## 1. 读取学生

```java
byte[] bytes = db.get("student/7".getBytes(StandardCharsets.UTF_8));
Student student = bytes == null ? null : StudentFqCodec.decode(bytes).value;
```

缺失为 null，空数组是有效值。本文用到 `java.nio.charset.StandardCharsets`。

<!-- api:save -->
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

<!-- api:modify -->
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

<!-- api:delete -->
## 4. 删除

```java
db.delete("student/7".getBytes(StandardCharsets.UTF_8));
```

<!-- api:batch -->
## 5. 批量读取和写入

```java
byte[][] keys = { "student/7".getBytes(StandardCharsets.UTF_8), "student/8".getBytes(StandardCharsets.UTF_8) };
byte[][] values = db.getMany(keys);
```

putMany/deleteMany 把多项修改放在同一笔原子提交中。

<!-- api:transaction -->
## 6. 事务：一组修改一起成功

```java
try (Store.Transaction tx = db.begin()) {
    tx.put("a".getBytes(StandardCharsets.UTF_8), new byte[]{1});
    tx.put("b".getBytes(StandardCharsets.UTF_8), new byte[]{2});
    tx.commit();
}
```

未提交即 close 会丢弃修改。CONFLICT 时重建事务、重新读取计算。

<!-- api:query -->
## 7. 条件查询与排序

```java
var score = io.fengqudb.Q.s64(StudentFqCodec.Field_score);
var name = io.fengqudb.Q.text(StudentFqCodec.Field_name);
var rows = io.fengqudb.Q.from("student/").where(score.ge(90L))
    .select(name, score).orderBy(score.desc()).limit(10).run(db);
```

条件可 and/or/not 组合；groupBy/having 做分组统计，join 做等值关联，explain 查看索引选择。

<!-- api:index -->
## 8. 索引

```java
db.defineIndex("student-score".getBytes(StandardCharsets.UTF_8),
    "student/".getBytes(StandardCharsets.UTF_8), StudentFqCodec.Field_score, 5, 0);
```

5 表示有符号 ZigZag 整数。

<!-- api:durability -->
## 9. 付款等关键写入：可靠确认

默认性能模式为 50 ms / 1 MiB，普通写入确认内存生效。关键写入传 true：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```java
db.put("payment/receipt".getBytes(StandardCharsets.UTF_8), "paid".getBytes(StandardCharsets.UTF_8), true);
db.sync();
```

事务使用 commit(true)。submitPut 返回提交凭据，需等待或轮询。COMMIT_UNKNOWN 时按业务编号核对，不能盲目重复扣款。

<!-- api:expiry -->
## 10. 临时数据与到期时间

写入时使用带 expiresAtMs 的重载，传入 `System.currentTimeMillis() + 300_000` 即五分钟后过期；0 表示不过期。

<!-- api:fastread -->
## 11. 高频读取

```java
try (var key = db.prepareKey("student/7".getBytes(StandardCharsets.UTF_8))) {
    byte[] bytes = key.get();
}
```

普通 get 返回独立数组。共享 ByteBuffer/租约和读取器要及时 close；不要在关闭后使用它们派生的视图。

<!-- api:snapshot -->
## 12. 一致快照

```java
try (var snapshot = db.snapshot()) {
    byte[] value = snapshot.get("student/7".getBytes(StandardCharsets.UTF_8));
}
```

<!-- api:backup -->
## 13. 备份恢复

```java
db.backup("backups/full-001");
```

备份用新目录；恢复使用 restore/recoverChain，先关闭要替换的库。

<!-- api:collections -->
## 14. Hash、Set、List 和有序集合

execute 执行集合命令，hashGet 读取 Hash 字段。适用于原子成员操作、队列和排名。

<!-- api:maintenance -->
## 15. 清理和整理

```java
db.purgeExpired();
db.compact();
db.verify();
```

<!-- api:watch -->
## 16. 提交变化

watch 创建消费者，处理完成后 ack，使用完 close。适合本进程业务缓存更新。

<!-- api:serialization -->
## 17. 实体编解码

StudentFqCodec.encode/decode 直接访问 Student。记录的 fqEncode 保留未知字段；encodeInto(value, buffer, offset) 复用数组。自动生成在 Gradle 编译前运行，原生工具已随 SDK 提供。
