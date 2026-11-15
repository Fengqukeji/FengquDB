<!-- api:read -->
## 1. 读取

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let student = Codec::decode(&bytes)?.value;
}
```

缺失是 None，空 Vec 是有效值。Codec 是接入文档中的 StudentFqCodec。

<!-- api:save -->
## 2. 新增、保存、单个字符串和整数

```rust
db.put(b"name", "Alice".as_bytes())?;
db.put(b"score", &95_i64.to_le_bytes())?;
let score = db.get(b"score")?
    .map(|b| b.as_slice().try_into().map(i64::from_le_bytes)).transpose()?;
db.put(&Codec::key(&student)?, &Codec::encode(&student)?)?;
```

同键 put 覆盖旧值。整数例子需要使用可容纳长度转换错误的 Result。

<!-- api:modify -->
## 3. 修改成绩

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let mut row = Codec::decode(&bytes)?;
    row.value.score = 96;
    db.put(&row.fq_key()?, &row.fq_encode()?)?;
}
```

记录的 fq_encode 会保留未知字段。

<!-- api:delete -->
## 4. 删除

```rust
db.delete(b"student/7")?;
```

<!-- api:transaction -->
## 5. 事务和原子批写

```rust
let mut tx = db.begin(0)?;
tx.put(b"a", b"one")?;
tx.put(b"b", b"two")?;
tx.commit()?;
```

commit 消耗事务；未提交即 Drop 会丢弃。CONFLICT 时重新创建事务、读取和计算。put_many 批量暂存多条修改。

<!-- api:query -->
## 6. 筛选、排序、聚合

```rust
use fengqudb::builder;
let score = builder::sint64(Codec::FIELD_SCORE, 0);
let name = builder::text(Codec::FIELD_NAME, 0);
let rows = builder::from_(b"student/".to_vec()).where_(score.ge(90_i64))
    .select(&[&name, &score]).order_by([score.desc()]).limit(10).run(&db)?;
```

组合条件、group_by/having 和 join 用于更复杂筛选；prepare 复用查询，explain 查看执行计划。

<!-- api:index -->
## 7. 索引

define_compound_index 建立字段/复合索引，drop_index 删除。索引字段类型要和实体编码一致。

<!-- api:durability -->
## 8. 等待可靠落盘

默认性能模式为 50 ms / 1 MiB，普通成功表示内存生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```rust
db.put(b"saved", b"yes")?;
db.sync()?;
```

当前安全客户端没有单笔 forceReliable 参数；sync 提供显式落盘屏障。整库可靠模式在写入前设置：

```rust
let mut options = db.write_options()?;
options.mode = fengqudb::WriteMode::Reliable;
db.configure_write(&options)?;
```

write_options 读取当前配置，reset_write_options 恢复默认性能模式。配置会保存，重开后保留。

<!-- api:expiry -->
## 9. 到期数据

put_with_expiry 传绝对 Unix 毫秒；0 不过期。适合短期状态和临时缓存。

<!-- api:fastread -->
## 10. 热点共享读取

```rust
if let Some(view) = db.get_shared(b"student/7")? {
    consume(view.as_bytes());
}
```

租约 Drop 后借用失效；Reader 的 Rows 借用阻止在下一次 read 后继续使用旧行。需要长时间保存则复制。

<!-- api:snapshot -->
## 11. 一致快照

snapshot 创建一致视图，Drop 自动释放。不要保留不再使用的快照。

<!-- api:serialization -->
## 12. 实体编解码

Codec::encode_into 接受可复用 Vec，编码前 clear 可以保留容量。源码结构字段需要 pub 和 Default。生成发生在 cargo build 的 build script 中，不在数据库读写时运行。

当前安全客户端没有备份、集合、维护和订阅的高层方法。末尾 fengqudb_sys 是底层 unsafe 接口；普通业务优先使用本页前面的安全方法。
