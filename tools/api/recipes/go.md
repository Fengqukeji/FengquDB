<!-- api:read -->
## 1. 读取

```go
data, err := st.Get([]byte("student/7"))
if errors.Is(err, fengqudb.ErrNotFound) { /* 没有这条记录 */ }
if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data)
```

引入 errors。缺失通过 ErrNotFound 区分，空切片是有效值。

<!-- api:save -->
## 2. 新增、保存、单个字符串和整数

```go
if err := st.Put([]byte("name"), []byte("Alice")); err != nil { return err }
var bytes [8]byte
binary.LittleEndian.PutUint64(bytes[:], uint64(95))
if err := st.Put([]byte("score"), bytes[:]); err != nil { return err }
```

数字例子使用 encoding/binary。同键 Put 覆盖旧值；实体编码写法见接入文档。

<!-- api:modify -->
## 3. 修改成绩

```go
data, err := st.Get([]byte("student/7")); if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data); if err != nil { return err }
row.Value.Score = 96
data, err = row.FqEncode(); if err != nil { return err }
if err = st.Put([]byte("student/7"), data); err != nil { return err }
```

读改写用记录的 FqEncode 保留未知字段。

<!-- api:delete -->
## 4. 删除

```go
if err := st.Delete([]byte("student/7")); err != nil { return err }
```

<!-- api:transaction -->
## 5. 事务和原子批写

```go
tx, err := st.Begin(0); if err != nil { return err }; defer tx.Close()
if err = tx.Put([]byte("a"), []byte("one")); err != nil { return err }
if err = tx.Put([]byte("b"), []byte("two")); err != nil { return err }
_, err = tx.Commit(); if err != nil { return err }
```

未提交即 Close 会丢弃。CONFLICT 时重新创建事务、重新读取计算。PutMany 将一组键值放进同一事务。

<!-- api:query -->
## 6. 条件查询与排序

```go
score := fengqudb.S64(fqmodels.StudentFieldScore)
name := fengqudb.Text(fqmodels.StudentFieldName)
rows, err := fengqudb.From("student/").Where(score.Ge(90)).
    Select(name,score).OrderBy(score.Desc()).Limit(10).Run(st)
if err != nil { return err }; defer rows.Close()
```

And/Or/Not 组合条件，GroupBy/Having 分组统计，Join 关联，Explain 查看查询计划。

<!-- api:index -->
## 7. 索引

DefineCompoundIndex 为经常使用的字段筛选建立索引；DropIndex 删除索引。条件查询会利用可用索引。

<!-- api:durability -->
## 8. 等待可靠落盘

默认性能模式为 50 ms / 1 MiB，普通写成功表示内存生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```go
if err := st.Put([]byte("saved"), []byte("yes")); err != nil { return err }
if err := st.Sync(); err != nil { return err }
```

当前 Go 包没有单笔 forceReliable 参数；Sync 是显式落盘屏障，不要把 Put 单独返回当成可靠确认。整库可靠模式在写入前设置：

```go
options, err := st.WriteOptions(); if err != nil { return err }
options.Mode = fengqudb.WriteReliable
if err := st.ConfigureWrite(options); err != nil { return err }
```

WriteOptions 读取当前配置，ResetWriteOptions 恢复默认性能模式。配置会保存，重开后保留。

<!-- api:expiry -->
## 9. 临时数据

PutExpiring 的到期时间为绝对 Unix 毫秒，0 表示不过期。适合验证码、临时状态和短期缓存。

<!-- api:fastread -->
## 10. 减少热点读取分配

```go
shared, err := st.GetShared([]byte("student/7"))
if err != nil { return err }; defer shared.Close()
```

共享视图需要 Close。准备键/读取器可复用键和批量行，读取器行只保留到下次 Read/Release/Close。

<!-- api:snapshot -->
## 11. 一致快照

Snapshot 用于多次读取同一版本，使用后 Close。不要长时间保留不再需要的快照。

<!-- api:serialization -->
## 12. 实体编解码

StudentFqEncode/StudentFqDecode 使用原 models.Student。StudentFqEncodeInto 接收可复用切片，传 dst[:0] 复用容量。编号锁随业务源码提交，已有字段不要重新分配编号。

当前 Go 客户端没有备份、维护、集合和变更订阅的高层方法；本文件列出的接口才是可直接调用的 Go API。
