# Store 集合、索引、维护与恢复

默认性能模式支持同步接口：内存原子生效后确认，后台按50 ms／1 MiB刷盘。关键写入传force_reliable／forceReliable，或配置Reliable模式；提交凭据完成遵循所选保证。见[写入策略](STORE_WRITE_POLICY.md)。

本文描述源码构建中的 Store 扩展能力。项目尚未发布，当前 C/C++、C#、Java、Python 调用同一个 Store 引擎。旧单文件引擎及迁移器已删除。

## 原生集合

集合名称是最多 256 字节的二进制值。名称固定集合类型，混用类型返回 `CONSTRAINT`。Hash 字段、Set 成员和 SortedSet 成员最多 64 KiB，值最多 16 MiB。元素分别存储，更新一个元素不会重写整个集合；成员数量变化才更新小型计数元数据。有序集合按分值和成员二进制顺序排序，只接受有限 double，正负零视作同一分值。

| 类型 | 操作 |
| --- | --- |
| Hash | Get、Set、Delete、分页 Scan |
| Set | Contains、Add、Remove、分页 Scan |
| List | 两端 Push/Pop、Get、Set、Range；负下标从尾部计数 |
| SortedSet | Add/更新分值、Remove、Score、按分值升序分页 Range |
| 公共 | Size、分批 Clear |

```cpp
fengqudb::Store db("data/game");
db.execute({FQDB_HASH_SET, "player/42", "name", "Alice"});
auto name = db.hash_get("player/42", "name");
if (name) use(name->view()); // 共享只读视图；持有 name 期间有效

auto tx = db.transaction();
tx.execute({FQDB_SET_ADD, "team/red", "player/42"});
tx.put("player/42/profile", serialized_protobuf);
auto committed = tx.commit(); // 集合元素和业务记录一起提交
if (!committed) throw fengqudb::Error(static_cast<fqdb_status>(committed.status));
```

`Store.execute()` 自动提交一次命令，事务对象的 `execute()` 只暂存。保存点可以回滚集合修改。`CONFLICT` 应从新事务重试；没有隐藏的无限重试。读取结果在提交前分配并解析，避免先成功写入、再因分配整个结果失败而报告失败。C 的 `fqdb_store_txn_execute` 请求及响应 wire 字段见公共头文件。

`hash_get()` / `HashGet()` / `hashGet()` 是直接读取接口：C/C++ 使用共享值句柄；托管语言复制一次结果，不经过命令结果编码。未命中分别返回空 optional/null/None，空字节仍是有效值。普通 KV 的共享读取和私有二进制补丁减法裁剪路径继续保留。

扫描命令默认最多 256 项，单次上限 4096。结果的 `cursor`/`Cursor` 是不透明二进制续扫标记，下一次扫描放入 `key` 参数；空标记表示结束。编码后的游标可能超过业务键的 64 KiB 限制，扫描/索引查询允许最多 263168 字节的游标。恰好填满最后一页时，允许再请求一个空页。独立调用按各自快照读取，需固定数据集时在同一事务内分页。List Range 使用起始下标和 limit；空 List Range 返回空结果。单次命令结果和扫描暂存最多 64 MiB，超限应降低 limit。集合 Clear 一次处理 limit 个元素，按返回的剩余 count 继续调用。

```python
from fengqudb import Store, StoreOperation as Op
with Store("data/game", library="/absolute/path/to/library") as db:
    db.execute(Op.HASH_SET, b"player/42", b"name", b"Alice")
    assert db.hash_get(b"player/42", b"name") == b"Alice"
    with db.transaction() as tx:
        tx.execute(Op.LIST_PUSH_RIGHT, b"events", value=b"joined")
        tx.execute(Op.SORTED_ADD, b"ranking", b"player/42", score=120.5)
```

C# 使用 `StoreOperation` 和 `Execute`，Java 使用 `Store.Operation` 和 `execute`，参数和结果相同。Hash/Set/SortedSet 扫描返回的 count 是集合总成员数；索引查询的 count 是本页结果数。

## 自动字段索引与约束

业务显式声明字段及键前缀，随后引擎自动维护索引，不会默认给所有字段建立索引。顶层单值字段已扩展为 10 种类型，并支持复合字段与范围查询，详见 [Store 查询](STORE_QUERY.md)。重复出现的单值标签采用最后一个值。唯一约束针对当前可见记录；必填约束检查字段是否存在，空 bytes 可以存在。错误类型、缺失必填字段或重复唯一值返回 `CONSTRAINT`。

```cpp
// name、业务键前缀、字段号、类型（1 uint64 / 2 int64 / 3 double / 4 bytes）、标记
db.define_index("email", "user/", 3, 4, 1 | 2); // unique | required
fengqudb::wire::Writer query;
query.bytes_field(3, "alice@example.test");
auto matches = db.execute({FQDB_INDEX_LOOKUP, "email", {}, query.data()});
db.drop_index("email");
```

同名同参数的 define 是完成或续建；同名不同定义返回 `ALREADY_EXISTS`，需先 drop。最多 64 个定义，名称最多 128 字节、业务键前缀最多 4096 字节、被索引字段值最多 64 KiB。查询值使用该字段的单字段 Protobuf 编码，结果是业务主键。提供单字段或复合字段的等值、范围及反向分页，不提供完整 SQL 或嵌套字段查询。

编译后的定义按目录版本复用，并计入固定对象预算。记录修改只比较声明字段；字段及到期时间都没变时不更新对应索引。唯一旧所有者先按整笔事务移除，再检查新所有者，支持同笔事务交换唯一值。记录 TTL 同步到索引条目；过期记录的清理不读取旧业务载荷，也不会删掉已分配给新记录的唯一项。

构建先发布 BUILDING 定义，再按预算处理捕获的键；并发新写入从注册时便维护索引。每批索引和续扫键一起提交，崩溃后从已提交位置继续。完成前查询返回 `LOCKED`。取消、超时或约束错误可能留下 BUILDING 定义，可修正原数据后同参数续建，或 drop；后台维护启用时也会轮转续建未完成定义，一个错误定义不会独占所有轮次。约束在构建期间已经作用于新修改。暂存/约束准备失败后，事务须回滚到保存点或关闭。

## 自动维护与合组刷盘

```cpp
fengqudb::StoreRuntimeOptions policy;
policy.background = true;
policy.interval_ms = 1000;
policy.expiry_keys = 256;
policy.job_timeout_ms = 1000;
db.configure(policy);
auto status = db.runtime_info();
db.configure(); // 停止后台任务，恢复默认配置
```

后台默认关闭，配置仅作用于当前进程。启用后执行分批 TTL 清理、未完成索引续建，并按最小文件大小、增长量与估算垃圾比例触发在线整理。垃圾估算包括索引等开销，不是精确压缩收益预测；增长阈值避免没有足够变化时反复整理。遇到其他维护操作持有租约便跳过。状态提供 running、last_status、清理轮次和整理次数；关闭 Store 会请求取消并等待线程退出，子句柄继续保留数据库资源。期限和停止不能中断已经开始的操作系统 I/O。

可选 `job_max_read_bytes`、`job_max_write_bytes` 限制一轮后台工作的合计文件请求量；前台维护支持独立限额与 I/O 统计，详见 [维护 I/O 预算](STORE_IO_BUDGET.md)。超额返回 `LIMIT`，整理/完整备份的未发布候选会清理，索引已提交进度可续建。

默认将已排队的最多 128 笔事务合组，最多允许 1024 个排队请求，不固定增加等待。需要更多聚合时可设置 `group_delay_us`（0–10000），组大小为 1–256。每笔事务有独立版本、冲突检查和结果；整个组共享一次可靠刷盘。刷盘成功前读者只看到之前发布的视图。写入或同步失败会阻止该实例继续写入，受影响的已追加事务返回 `COMMIT_UNKNOWN`，须重开核对。

runtime_info 中 group_transactions 统计成功完成日志追加的事务，包括 BUFFERED 及结果未知的提交；group_flushes 只统计成功的组同步，不含检查点和显式 sync。BUFFERED 直接追加，不进入合组队列，也不提供逐组持久性承诺。配置 C API 结构时应填全参数，或传 NULL 恢复默认值。

索引定义/删除、增量备份、恢复链和运行配置接入常规 API 数值日志。后台任务不调用用户日志回调，错误通过 `runtime_info.last_status` 和计数器观察。

## 完整备份、增量包与时间点恢复

```cpp
auto base = db.backup("backup/base");
// 业务继续写入
auto increment = db.backup_incremental("backup/inc-1", base.snapshot_version);
// 下一包从 increment.final_version 开始
fengqudb::Store::recover_chain("backup/base", {"backup/inc-1"}, "data/recovered");
fengqudb::Store::recover_chain("backup/base", {"backup/inc-1"}, "data/at-version", wanted_version);
```

完整备份、每个增量包和恢复目标均必须是新目录。增量包校验数据库身份和连续版本，保留每笔事务及其时间；普通值和补丁最终值按原事务物化，不把补丁引用留在源文件里。内存保留受预算限制；物化后单笔日志仍受 64 MiB 上限约束，超限返回 `LIMIT`，不会发布半个包。压缩比很高、修改多个大值的补丁事务可能触发该限制。

增量包是专用归档，不能当作普通 Store 打开。恢复先检查整条链的身份、版本和覆盖范围，保持输入文件租约，再复制基础状态并回放到私有目标；校验通过、同步并关闭目标句柄后才发布。不覆盖原库或已有目标，不改变输入增量包。完整备份源的打开与恢复语义见对应接口说明。

版本和绝对 Unix 毫秒两个目标参数只能选一个，零表示回放到可用链末端。时间点恢复选择提交顺序中不晚于目标时间的连续前缀，新提交使用不递减的时间水位；不支持回到基础备份之前或增量覆盖时间之后。恢复保留原绝对 TTL，已经到期的数据不会因为恢复而重新变为可见。

在线整理会提高保留历史下限。from_version 早于下限时返回 `HISTORY_LOST`，需要重新做完整备份；可以用命名水位跨整理保留所需历史，见 [历史保留](STORE_HISTORY.md)；没有自动外送归档。持续备份应由业务根据维护节奏保存完整加增量链。Linux 的新目录发布仍需要 `renameat2(RENAME_NOREPLACE)`，不支持的文件系统返回 `UNSUPPORTED`；WSL `/mnt/d` 的 DrvFS 已知不支持，原生 Linux 文件系统流程已验证。

## 系统记录与兼容边界

扩展记录使用保留前缀 `00 46 51 44 42 ff`。首次启用扩展时先检查旧业务记录碰撞，再将启用标记与集合/索引变更一起写入 WAL；碰撞返回 `CONSTRAINT`，不会静默隐藏原数据。新普通 put 不接受该前缀；旧未启用库中的记录仍可读取和删除，以便迁出该范围。旧快照保留原视图语义。

普通读取与扫描隐藏系统记录，普通范围事务的冲突检查也排除已启用的系统范围，避免无关集合变化产生幻读冲突。StoreInfo、维护及 TTL 清理的存储行统计包含内部元数据和索引条目，不能直接作为业务对象数量；集合数量用 Size 查询。检查点/备份/整理/恢复保留扩展标记，校验还检查就绪索引的双向一致性及集合计数和顺序。

新增文件元数据要求使用匹配的新构建及绑定，较旧的开发版 Store 构建不保证向前兼容。运行时仍不依赖 SQL 服务、Google Protobuf 或模型运行库。真实断电、长期生产压力和硬件故障验收需要相应环境，不能用单元测试通过代替。

## CLI

```text
fqdb_store_cli index-define SOURCE NAME KEY_PREFIX FIELD TYPE FLAGS
fqdb_store_cli index-drop SOURCE NAME
fqdb_store_cli backup-incremental SOURCE NEW_ARCHIVE FROM_VERSION
fqdb_store_cli recover-chain BASE NEW_DEST ARCHIVE... [--target-version N | --target-time-ms N]
```

维护 CLI 需要独占租约。进程内并发调用应使用 API；后台调度配置属于长期运行的库实例，不写成会立即退出的 CLI 设置。

本轮性能、二进制体积、审查问题及验证结果见 [性能与代码审查记录](STORE_PERFORMANCE_REVIEW_20261106.md)。

## 进程内变更订阅

Store 支持按原提交顺序、显式确认和前缀过滤的 WAL 拉取订阅，存活订阅保护未确认历史，五种语言接入见 [变更订阅](STORE_WATCH.md)。跨重启消费进度须配合持久历史水位，未启用时不分配通知对象或复制事件值。
