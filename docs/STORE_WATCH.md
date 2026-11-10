# Store 进程内变更订阅

默认性能模式支持同步接口：内存原子生效后确认，后台按50 ms／1 MiB刷盘。关键写入传force_reliable／forceReliable，或配置Reliable模式；提交凭据完成遵循所选保证。见[写入策略](STORE_WRITE_POLICY.md)。

开发版 C/C++、C#、Java、Python 提供基于 WAL 的拉取式订阅，适合缓存失效、搜索索引更新和 Wiki/RAG 增量处理。不启动服务，不做跨进程协作。未使用订阅时没有事件队列、事件值复制或通知对象分配；使用时仅在已发布视图改变后通知等待者，消费端按需读取 WAL。

## 事务与确认

起点是最后已消费的版本。省略起点时，在写者屏障内原子选择当前版本；显式起点须位于可用历史范围内，否则 HISTORY_LOST。不把当前全库快照伪装成历史事件。

Next 最多返回一笔原始提交中符合前缀的全部操作，保留版本、提交时间和绝对 TTL。事务内按键排序，同一键只返回该事务的最终结果；补丁和 TTL 修改统一为 PUT，包含修改后的完整值；DELETE 没有值，空字节串 PUT 仍是有效值。不返回旧值。TTL 到时本身不产生提交事件；purge_expired 删除会产生提交，整理中的到期过滤不另造提交。

显式 Ack(batch.version) 后才前进。重复 Next 返回同一不可变批次，原生端共享值，不重新解析或复制；语言绑定创建本语言拥有的数组。消费或语言运行时分配失败时不确认，重取即可。不能确认尚未交付的未来版本，重复确认当前版本无副作用。业务副作用应按版本保持幂等。

存活订阅自动保护未确认 WAL，在线整理保留必要旧代际。确认后不长期持有文件视图或内存索引增量根，下一次整理可回收无用文件。独立批次在 Ack、取消以及关闭订阅或 Store 后仍有效。C/C++ 借用字节到批次关闭有效，托管语言返回独立副本。

进程内保护不能替代跨重启消费进度。需要续传时，用 [持久历史水位](STORE_HISTORY.md) 保护已保存的最后消费版本；消费成功并持久记录进度之后，前移水位，再 Ack。新建任务先建立保护再允许整理，已有任务继续使用旧保护，不提前跳到最新版本。独立身份恢复库的原水位不自动生效，应用需明确选择新起点。

Cancel 为终态，可从其他线程调用，打断等待或正在解析的 Next。其他订阅操作必须串行，并发 Next/Ack 返回 LOCKED。C 的 close 要求无并发调用；C#/Java/Python 显式关闭先取消，以安全句柄或调用租约延迟释放；C# 的 Dispose 可以先返回，native 句柄在最后一个调用退出后释放。关闭 Store 后子订阅仍可使用，直到自己关闭。

## 过滤、等待和预算

前缀为二进制键前缀，空前缀匹配全部。默认隐藏集合系统键，避免历史水位和字段索引成为业务事件。IncludeSystem 返回内部记录，主要用于诊断，不是稳定的逻辑集合事件协议。MetadataOnly 返回键、PUT/DELETE、TTL 和版本，不物化值；仍验证 WAL CRC，不能据此声称跳过大值磁盘读取。

默认每次最多扫描 256 笔提交，可设 1～4096。过滤达到上限或捕获末端仍没有匹配操作时，返回 Rows 为空的进度批次，也需要 Ack。没有新提交时，零超时轮询，正超时等待；C 分别返回 NOT_MODIFIED/TIMEOUT，上层语言返回空结果。其他错误正常抛出，不转换为空结果。

默认单批 64 MiB，可设 1 KiB～256 MiB，包括结构、向量容量、键、值及物化分配余量，计入 pinned_bytes。过大事务返回 LIMIT，位置不变；可从同一确认版本以更大预算或 MetadataOnly 重开。小补丁 WAL 不代表完整结果值也小。

每库最多 64 个活跃订阅，句柄计 max_snapshots，元数据计内存预算。取消解除保留要求，关闭释放句柄。持久水位 64 项限制单独计算；HistoryInfo 的 active_pins/total_pins 仅统计持久控制记录，retained_generations/retained_file_bytes 也包含订阅所需文件。共用 128 个保留代际上限，达到后拒绝整理，不静默丢历史。慢订阅仍可能占据磁盘，宿主须消费、取消或提供足够空间。

游标保存代际号、偏移和版本，确认后从原位置续读；整理换代时重新定位必要段，按提交顺序继续，不维护逐提交常驻索引。调用期间只捕获历史所需状态，不固定完整索引根。

SYNC 只流出可靠同步后发布的提交；BUFFERED 可以先流出未同步提交。DurableVersion 是首次捕获批次时的持久版本，重取不改写旧元数据。刷盘失败且未发布的候选不进入本进程订阅；重开可能恢复一笔结果不确定的完整 WAL，仍须遵守 COMMIT_UNKNOWN。

## 示例

```cpp
auto changes = db.watch();
db.put_if_changed("player/42", protobuf_bytes);
if (auto batch = changes.next(1000)) {
    for (const auto &row : *batch) apply_change(row);
    changes.ack(batch->info().version); // 仅处理成功后确认
}
```

```csharp
using var changes = db.Watch(prefix: playerPrefix);
var batch = changes.Next(1000);
if (batch != null) {
    foreach (var row in batch.Rows) Apply(row.Key, row.Value, row.Kind);
    changes.Ack(batch.Version);
}
```

```java
try (Store.Watch changes = db.watch(-1, playerPrefix, new Store.WatchOptions())) {
    Store.Changes batch = changes.next(1000);
    if (batch != null) {
        for (Store.Change row : batch.rows) apply(row.key, row.value, row.kind);
        changes.ack(batch.version);
    }
}
```

```python
with db.watch(prefix=b"player/") as changes:
    batch = changes.next_batch(timeout_ms=1000)
    if batch is not None:
        for row in batch.rows:
            apply_change(row.key, row.value, row.kind)
        changes.ack(batch.version)
```

C 使用 watch_open/next/ack/cancel/close，通过 change_batch_data 一次取得头和连续行数组，最后 change_batch_close。完整函数名均以 fqdb_store_ 开头。三种语言按批进入 native 层，不逐行调用 native getter；Java 每批一次 JNI 获取键/值数组和数值元数据。

磁盘历史标记 bit4 独立于集合命名空间 bit1。仅订阅触发保留时，不启用集合或隐藏旧原始二进制键；新增公开写入仍遵守既有保留前缀限制。旧开发库不理解此标记组合，须使用配套新库和绑定。
