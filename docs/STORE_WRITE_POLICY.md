# 性能模式与可靠模式

默认采用**性能模式，50 ms／1 MiB**。普通写入由数据库独立写线程按顺序执行，完整事务在内存原子发布、立即可读后返回成功；后台在周期到达或待刷 WAL 字节达到阈值时可靠同步。可靠模式，以及单次 `force_reliable=true`，必须等可靠刷盘才返回成功。

符合条件的完整覆盖写可自动生成多段差量日志，阈值仍按实际逻辑WAL字节及帧开销统计，确认和可靠前缀语义不变。适用范围、保守回退和当前格式见 [差异内核接入](STORE_BINARY_DIFF_MERGE_20261110.md)。

| 操作 | 性能模式普通写 | 可靠模式／强制可靠写 |
| --- | --- | --- |
| 同步调用成功／异步凭据完成 | 内存原子生效，可立即读取 | 当前事务及此前完整前缀已可靠同步 |
| 崩溃／断电前未同步的提交 | 可能丢失 | 成功返回的提交应可恢复，依赖设备正确执行刷盘 |
| 读写事务 | 固定原始快照并检查冲突 | 相同冲突与原子性规则 |

性能模式成功不表示已落盘。强制可靠会刷出当前事务依赖的此前提交前缀；不能只刷付费记录却丢掉它之前的余额变化。进程崩溃后只恢复有效磁盘事务前缀；不会重造 RAM 数据。完整记录校验错误仍报 CORRUPT，不能一律静默截掉。

## 配置和状态

| 参数 | 默认 | 范围 |
| --- | --- | --- |
| mode | Performance / PERFORMANCE / performance | Reliable 或 Performance |
| period_ms / PeriodMs / periodMs | 50 | 1～60000 ms |
| threshold_bytes / ThresholdBytes / thresholdBytes | 1048576 | 1 B～64 MiB |

阈值统计待刷 WAL 的逻辑字节，含帧开销；不是每周期固定写入量。完整事务不拆分，执行组／调度／磁盘 I/O 会让实际触发时间及字节数超过设置。50 ms 是刷盘调度目标，**不是掉电损失的严格最大时间**。没有待写请求且待刷字节为零时，写线程使用无截止时间的条件等待；空库不启动周期写线程，最后一批落盘后也不会每50 ms唤醒或空刷盘。新写入会唤醒线程。

新库和没有 `WRITE` 的目录采用 50 ms／1 MiB。已有保存配置继续按原值加载；升级默认值不会覆盖业务选择，调用 reset／C传NULL才会恢复并保存新默认值。

配置按数据库目录保存在带版本和 CRC 的40字节 `WRITE`；在线更新前停止接收、排空旧请求并可靠同步旧前缀，再保存及应用。相同已保存配置不重复写盘；reset／C传NULL恢复并保存默认值。保存前失败保留旧设置，发布不确定返回 COMMIT_UNKNOWN，损坏配置拒绝打开。设置的 abi_version 仍为1，与全局库 ABI 2 是不同字段。

`write_state`／`WriteState`／`writeState` 返回 `version`、`durable_version`、`pending_bytes`、`last_status`。它反映数据库整体状态，不能取代每笔返回值。后台刷盘失败后保留当前可读内存视图、记录故障，停止接受新写；业务应停止依赖这些未可靠数据继续推进，并重开核对磁盘状态。已经对外发送的支付、邮件等副作用无法由数据库自动撤销。付费等关键操作应在强制可靠成功后再发起外部副作用。

## 接口

| 语言 | 同步直接操作／事务 | 可选可靠参数 | 异步凭据 |
| --- | --- | --- | --- |
| C | store_put/delete/patch、txn_commit及原子操作 | 最后传0或1 | put_submit / txn_submit，poll / wait |
| C++ | put / erase / patch、commit及批量原子操作 | bool force_reliable=false | submit_put / submit |
| C# | Put / Delete / Patch、Commit及批量原子操作 | bool forceReliable=false | SubmitPut / Submit，WaitAsync |
| Java | put / delete / patch、commit及批量原子操作 | boolean forceReliable | submitPut / submit，poll / await |
| Python | put / delete / patch、commit及批量原子操作 | force_reliable=False | submit_put / submit，wait_async |

同步性能写仍会等待前序执行、内存分配和正在进行的刷盘，延迟不保证为零；需要 UI 平滑时仍推荐异步凭据。库不会占用宿主主线程。可靠同步调用阻塞调用它的线程，可由宿主工作线程调用。

`submit_put` 返回前复制输入；事务 `submit` 接管写集和原快照依赖，成功入队后原事务封闭。凭据完成表示所选模式的保证，默认仅为内存成功。等待超时不取消请求，cancel 尽力取消，开始不可撤销写入后仍可能成功。提前关闭未完成凭据会等待，须先 poll / await 再释放。队列有16384请求及总输入字节上限，满时 LIMIT，配置切换时异步入队可返回 LOCKED，应由宿主背压。

```cpp
auto db = fengqudb::Store::open_at(root, "main");
db.put("player/42", serialized);          // 内存生效后返回
db.put("payment/7", payment, 0, true);    // 刷盘完成才返回
auto tx = db.transaction();
tx.put("balance/42", new_balance);
auto result = tx.commit(true);            // 整笔事务强制可靠
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));
db.configure_write({fengqudb::StoreWriteMode::Reliable, 50, 1048576});
db.reset_write_options();
```

```csharp
db.Put(key, protobufBytes); // 默认性能模式
db.Put(paymentKey, paymentBytes, forceReliable:true);
using (var receipt = db.SubmitPut(key, protobufBytes, forceReliable:true)) {
    await receipt.WaitAsync();
}
await db.ConfigureWriteAsync(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
await db.ResetWriteOptionsAsync();
```

```python
db.put(b"player/42", protobuf_bytes)
db.put(b"payment/7", payment_bytes, force_reliable=True)
with db.transaction() as tx:
    tx.put(b"balance/42", new_balance)
    tx.commit(force_reliable=True)
db.configure_write(mode="reliable", period_ms=50, threshold_bytes=1048576)
db.reset_write_options()
```

`sync()` 是显式可靠屏障；配置切换、检查点和正常关闭也会等待相关请求并处理待写前缀。C的close无返回值，关键退出流程必须先显式sync并检查错误。显式低层 `FQDB_DURABILITY_BUFFERED` 保留手动sync语义，是测试及高级工具选项；普通接入使用默认SYNC底层选项和上述mode。

本轮 C 函数签名改变，**全局 ABI 升为2，Linux SONAME为libfengqudb.so.2**。必须同时重新编译头文件、绑定与原生库，历史SDK包不能混用。C++／C#／Java／Python入口检查ABI。移动平台只有源码适配，尚未完成Android/iOS真机与真实硬件掉电验证。

同步完成通知按调用线程复用，Windows／Linux 使用原生地址等待及有界短暂等待，其他平台和原生等待失败时退回条件变量；异步凭据仍支持多等待者。补丁生成的完整结果及1～64 KiB重复覆盖值可进入有界缓存，供后续差量复用；首次插入不因自动差量填充缓存，其他整值覆盖继续按实际读取情况预热。缓存不持有事务暂存预算，cache_bytes=0仍可正常读写与恢复。周期、阈值及可靠屏障不变，当前实现见[差异内核接入](STORE_BINARY_DIFF_MERGE_20261110.md)，此前通知与缓存优化见[提交路径优化](STORE_HOT_WRITE_REVIEW_20261109.md)。
