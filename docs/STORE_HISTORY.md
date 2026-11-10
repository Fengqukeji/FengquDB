# Store 备份历史保留

默认性能模式支持同步接口：内存原子生效后确认，后台按50 ms／1 MiB刷盘。关键写入传force_reliable／forceReliable，或配置Reliable模式；提交凭据完成遵循所选保证。见[写入策略](STORE_WRITE_POLICY.md)。

这是开发版接口，C/C++、C#、Java、Python 共用同一引擎。保留水位解决持续增量备份遇到在线整理后 HISTORY_LOST 的问题；未声明水位且没有存活变更订阅时，整理只保留当前状态，不额外固定历史文件。

## 工作流程

```cpp
// 初次启动新的备份任务。省略版本，在线性化的写队列中固定当前版本。
db.hold_history("backup-main");
auto base = db.backup("save/bak/base-1");
db.hold_history("backup-main", base.snapshot_version);
uint64_t archived = base.snapshot_version;

// 业务继续修改；此处可以执行或由后台触发整理。
db.compact();
auto increment = db.backup_incremental("save/bak/inc-1", archived);
db.hold_history("backup-main", increment.final_version);
archived = increment.final_version;

// 停止这个备份任务，明确解除其保留要求。
db.release_history("backup-main");
db.compact();
```

水位表示“已经完整归档的最后版本”。只有增量包成功发布后才能前移，失败时保持旧水位。COMMIT_UNKNOWN 不表示可安全重复使用同一目标，也不能据此前移水位；保留旧保护并核对输出，或导出到另一个明确的新目标。

初次备份应先固定当前版本、再创建完整备份，避免完整备份返回与登记水位之间发生整理。若同名任务已经保护一条旧备份链，创建新完整备份时继续保留旧水位，待新备份成功后再前移；不要为了开始新备份而提前放弃旧链。创建新任务名也可以暂时保护两条链。

默认 SYNC 下，成功的水位修改已可靠同步；BUFFERED 沿用普通写入语义，需主动 Sync 才能依赖它跨突然终止保留。完整备份、增量包仍有各自同步发布契约。应用重启后水位继续生效，不需要靠进程内句柄维持。

## API

| 语言 | 固定/前移 | 解除 | 状态 |
| --- | --- | --- | --- |
| C | `fqdb_store_history_hold` | `fqdb_store_history_release` | `fqdb_store_history_info_get` |
| C++ | `hold_history` | `release_history` | `history_info` |
| C# | `HoldHistory` | `ReleaseHistory` | `HistoryInfo` |
| Java | `holdHistory` | `releaseHistory` | `historyInfo` |
| Python | `hold_history` | `release_history` | `history_info` |

名称为 1～128 字节的二进制值，不是文件名。C 的 FQDB_HISTORY_CURRENT（UINT64_MAX）表示原子选择最新版本；C++/C#/Python 省略版本、Java 使用单参数重载即为此行为。明确版本须在当前可用历史范围内，否则返回 HISTORY_LOST。相同名称和版本不写 WAL；解除不存在的名称不写 WAL。

修改返回已有 AtomicInfo 结构，changed 表示是否创建提交，version 是完成时的提交视图版本；observed_version 是此前控制记录的版本，不是新的备份水位。失败或结果不确定时输出清零。水位修改本身也是提交，因此版本编号包括这些控制操作；不要用业务修改次数计算增量起点。

状态包含当前 version、可用历史下限 floor、最早有效水位 oldest_pin、有效 active_pins、存储的 total_pins，以及保留代际数和文件字节数。没有有效水位时 oldest_pin 为捕获的当前版本。file_bytes 等原 StoreInfo 统计仍针对活动文件；评估存储占用时还要考虑 retained_file_bytes、未释放旧快照及临时输出。

## 保留和回收

整理继续生成紧凑的当前状态；所需旧代际作为不可变 WAL 段保留，代际链、边界和可用下限随 CURRENT 对应的检查点一起发布。新代际发布前同步需要保留的源 WAL，包括 BUFFERED 源的已发布数据。跨代际增量导出依次读取这些 WAL，保留原提交边界、顺序与时间。

普通写入不维护额外事件队列，不复制一份常驻历史值。读取历史时才解析需要的记录；大值仍按原事务预算物化，原 64 MiB 事务/归档限制不变。极高压缩比的补丁事务在物化为增量包后仍可能达到该限制，此时不会发布半个包。

多个任务和存活订阅取最早水位；快任务不能替慢任务释放历史。前移或解除后，下一次整理跳过不再需要的祖先，并在最后一个旧视图释放后删除相应文件。未变化的重复整理不会延长空代际链。保留文件仍使用数据库目录下的短名，目录内平铺，不引入历史子目录。

最多 64 个存储水位、128 个保留代际，元数据计入固定内存预算。达到代际上限时整理返回 LIMIT，保持原活动库可读写，不擅自丢掉慢任务需要的历史。任务前移或解除后可以继续整理。该上限不是磁盘字节上限；长期停止归档的任务仍会固定旧文件，须监控状态和可用空间。

完整备份和恢复目标有独立数据库身份。复制过来的控制记录保留原身份，在新库中不生效，避免新库错误地保护并不存在的源历史；可用同名 hold 绑定到新库的有效水位，或解除原任务名。total_pins 包含这些外来控制记录，仍计入 64 项限制。

缺失或损坏必需祖先时打开/校验/导出会失败并保留文件，不把祖先缺失当成可以回收其余文件。校验会检查保留范围的 WAL 和引用值；启动仅检查链头、身份和边界，实际 WAL 按需读取。旧开发库不理解历史功能标记，须配套新库和绑定，不能用旧库强行打开这类目录。

进程内拉取式变更订阅已接入，见 [Store 变更订阅](STORE_WATCH.md)。维护字节预算和完整资源压力验证仍在持续目标中。
