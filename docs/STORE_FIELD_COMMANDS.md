# 写线程原子字段命令

`add_i64_fields` 将多条“读当前值、检查范围、加减、修改”作为一笔原子命令执行。它适用于热点计数、配额扣减和多账户转账，复用数据库现有写入调度。

每次传入 1～256 项。每项包含记录 key、字节偏移 offset、增量 delta，以及运算结果的 minimum / maximum。数据库读取并修改现有记录中的小端有符号 64 位整数，记录的其他字节和 TTL 保持原样。重复 key、重叠字段按输入顺序计算，后面的项看到前面的修改。

偏移是序列化二进制中的字节位置，不能填写 protobuf 字段编号。该接口适用于已知位置的 `sfixed64` 或按相同规则解释的 8 字节字段，不适用于变长 varint。业务应依据自己的固定格式确定偏移。

例如，以下 protobuf 记录的第一个字段为 sfixed64，标签占 1 字节，金额从 offset=1 开始：

```cpp
// debit 和 credit 均为已存在的记录；余额须保持非负。
auto result = db.add_i64_fields(
    std::vector<fengqudb::StoreI64FieldUpdate>{
        {"debit",  1, -10, 0, INT64_MAX},
        {"credit", 1,  10, 0, INT64_MAX}},
    0,    // timeout_ms，0 表示不设置准备阶段超时
    true  // force_reliable，等待本笔及之前的提交持久化
);
```

对应接口：

| 语言 | 接口和输入 |
|---|---|
| C | `fqdb_store_add_i64_fields(store, items, count, timeout_ms, &info, force_reliable)`，项类型 `fqdb_store_i64_field_update` |
| C++ | `Store::add_i64_fields(vector<StoreI64FieldUpdate>, timeout_ms, force_reliable)`；另提供直接接收 C 结构体数组的重载 |
| C# | `Store.AddInt64Fields(StoreI64FieldUpdate[], timeoutMs: 0, forceReliable: true)` |
| Java | `Store.addInt64Fields(Int64FieldUpdate[], timeout, forceReliable)` |
| Python | `store.add_i64_fields([(key, offset, delta, minimum, maximum), ...], force_reliable=True)`；省略范围时每项可写为三元组 |

成功结果沿用 AtomicInfo：version 为完成版本，observed_version 为本命令开始计算时的数据库版本，value 为最后一项的计算结果，matched=true，changed 表示是否有项实际改变。所有项均无变化时不创建新版本，但强制可靠提交仍会持久化之前的内存写入。返回结果不分配逐项结果数组。

任意项遇到记录不存在/已过期、偏移错误、整数溢出、越界或索引约束失败，整笔命令失败，修改不发布。输入及暂存数据共同计入事务预算；准备失败、排队超时会释放私有资源。超时不取消已经开始的磁盘持久化屏障。

性能模式在内存原子生效后返回；可靠模式或 `force_reliable=true` 等待持久化。磁盘失败返回 `COMMIT_UNKNOWN` 时，持久化结果可能不确定，不能把非幂等加减命令直接重试。普通任意读改写事务仍保留快照和冲突检查；此接口不会将宿主回调放到数据库写线程中执行。

## 整理参数

启用后台整理后，C/C++、C#、Java、Python 的默认运行参数为：检查间隔 200 ms，最小占用和增长触发量均为 32 MiB。显式传入的运行参数继续有效，后台维护仍须由调用者启用。

文件首次转为可原位回收的镜像时，无 I/O 预算的路径使用临时 256 KiB 复制缓冲；有 I/O 预算的路径仍按 4 KiB 推进，以保留小预算下的可续做能力。缓冲在调用结束释放，不持续占用每个数据库 256 KiB。

写入模式的默认值仍为性能模式 50 ms / 1 MiB。大值和大原子批量的写入路径、字节阈值没有调整。
