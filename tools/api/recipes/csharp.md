<!-- api:read -->
## 1. 读取：找一名学生、读取积分

本页示例中的 db 来自统一入口：`var db = AppDb.Main;`。

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

<!-- api:save -->
## 2. 新增或保存：保存档案、单个数字和字符串

同一个键再次 Put 会覆盖原值。下例的 Student 定义见接入文档。

```csharp
db.Put(new Student { Id=7, Name="张三", Score=95 });
db.Put("score", 95);
db.Put("name", "张三");
db.Put("enabled", true);
```

<!-- api:modify -->
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

<!-- api:delete -->
## 4. 删除：删除记录

```csharp
db.Delete(Student.MakeKey(7));
db.Delete(System.Text.Encoding.UTF8.GetBytes("score"));
```

<!-- api:batch -->
## 5. 批量：一组档案一起读写

```csharp
byte[][] keys = { Student.MakeKey(7), Student.MakeKey(8) };
var values = db.GetMany(keys);
```

保存多条实体可放进同一事务，避免逐笔提交。

<!-- api:transaction -->
## 6. 事务：多项修改必须一起成功

```csharp
using var tx = db.Begin();
tx.Put("score", 100);
tx.Put("level", 2);
tx.Commit();
```

未 Commit 就 Dispose 会丢弃修改。遇到 CONFLICT 时，重新创建事务、读取并计算；不要只重放旧值。

<!-- api:query -->
## 7. 筛选、排序、排行榜、统计

```csharp
var score = Student.Cols.Score;
var name = Student.Cols.Name;
var rows = Q.From("student/").Where(score >= 90L)
    .Select(name, score).OrderBy(score.Desc()).Limit(10).Run(db);
foreach (var row in rows) Console.WriteLine($"{row.Get(name)} {row.Get(score)}");
```

`QCond` 用 `&`、`|`、`!` 组合；`GroupBy/Having` 用于分组统计，`Join` 用于按字段关联记录。`Explain` 查看是否走索引，`Memory/TimeoutMs` 限制大查询资源。下面列出全部查询接口及重载。

<!-- api:index -->
## 8. 索引：经常按某个字段找记录

```csharp
byte[] index = System.Text.Encoding.UTF8.GetBytes("student-score");
byte[] prefix = System.Text.Encoding.UTF8.GetBytes("student/");
db.DefineIndex(index, prefix, 3, 5); // 字段3：有符号 ZigZag 整数
```

先按索引缩小范围，再筛选其余条件。唯一、必填和复合字段限制通过索引选项设置。

<!-- api:durability -->
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

<!-- api:expiry -->
## 10. 到期数据：验证码、临时状态

```csharp
ulong expires = (ulong)DateTimeOffset.UtcNow.AddMinutes(5).ToUnixTimeMilliseconds();
db.Put("verification", "1234", expiresAtMs: expires);
```

时间是绝对 Unix 毫秒；0 表示不过期。到期后读不到，物理空间随后由清理或整理回收。

<!-- api:fastread -->
## 11. 热点读取：复用键和读取缓冲

```csharp
using var scoreReader = db.PrepareValue<int>("score");
int score = scoreReader.Read();
using var entityReader = db.PrepareEntity<Student>(7);
Student student = entityReader.Read();
```

普通 Get 返回可独立保存的值。共享读和读取器要及时 Dispose；读取器给出的字节视图不能保存到下一次 Read 之后。

<!-- api:snapshot -->
## 12. 一致读取：一组读取使用同一时刻的数据

```csharp
using var snapshot = db.Snapshot();
var student = snapshot.GetEntity<Student>(7);
int score = snapshot.Get<int>("score");
```

<!-- api:backup -->
## 13. 备份和恢复：更新前留一份存档

```csharp
db.Backup("backups/full-001"); // 使用新的目标目录
```

恢复时关闭要替换的数据库，调用 Restore/RecoverChain；不要直接复制正在写入的数据库目录。

<!-- api:collections -->
## 14. 集合：Hash、Set、List 和有序集合

Execute 接收集合命令；HashGet 读取 Hash 字段。适合背包成员、去重集合和排行榜，不需要自己把整份集合反复读回修改。各命令参数见本节类型和重载。

<!-- api:maintenance -->
## 15. 整理、清理和检查

```csharp
db.PurgeExpired();
db.Compact();
db.Verify();
```

放在维护时段调用；大库可用选项控制每次处理量。

<!-- api:watch -->
## 16. 变更通知：消费本进程内的提交变化

Watch 创建消费者，读取变化后 Ack 确认处理水位，关闭时 Dispose。适合更新业务缓存或触发后台任务。

<!-- api:serialization -->
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
