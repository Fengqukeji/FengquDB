# 分页 Store 接口（开发版）

Store 是当前唯一的数据库引擎，数据库路径为目录。本项目尚未发布，旧单文件 Database 引擎、接口和迁移器已从源码删除，历史开发包仅供追溯。

默认性能模式在内存原子生效后确认，50 ms／1 MiB后台刷盘；可靠模式或单次force_reliable等待刷盘。同步和异步接口均支持两种保证，详见 [写入策略](STORE_WRITE_POLICY.md)。本轮全局ABI为2，原生库与绑定须配套。

当前已接通原子事务、增删改查、二进制补丁、一致快照、正反向范围读取、点冲突与幻读检查、嵌套保存点、TTL 可见性、事务取消/期限、受预算限制的缓存与增量索引、检查点及重启恢复、显式在线整理、完整备份、校验与恢复。C、C++、C#、Java、Python 共用同一个 C++ 引擎与文件格式。业务值是透明字节，调用方可使用 Protobuf wire 工具或自己的业务序列化器。

当前还提供 TTL 到期索引与分批清理、Hash/Set/List/SortedSet、自动字段索引及唯一/必填约束、后台维护、Store 合组刷盘、完整加增量备份链、按版本/时间恢复。扩展接口、资源限制及兼容边界见 [Store 扩展能力](STORE_FEATURES.md)。这些是开发版源码能力，不等于完成真实断电和长期生产验收，也没有全工作负载超越其他数据库的结论。

目录与 Unity 存档接入见 [目录规范](STORE_LAYOUT.md)，条件原子更新、批量读取和准备对象见 [原子与批量接口](STORE_OPERATIONS.md)，Python/Java 的可选固定缓冲见 [复用读取](STORE_BINDING_BUFFERS.md)，各语言零拷贝与准备读取器见 [共享读取](STORE_SHARED_READ.md)。

编号耗尽、整数与时间边界及 Java 无符号版本约定见 [数值上限](STORE_NUMERIC_LIMITS.md)。

维护操作的可选读写字节额度、统计与旧 ABI 兼容见 [维护 I/O 预算](STORE_IO_BUDGET.md)。额度统计的是文件请求字节，不能代替物理磁盘空间或内存限制。

复合索引、范围分页和字段投影见 [Store 查询](STORE_QUERY.md)。

## C++17

持续备份的持久水位与跨整理历史保留见 [Store 历史保留](STORE_HISTORY.md)，进程内按提交拉取变化见 [Store 变更订阅](STORE_WATCH.md)。

```cpp
#include <fengqudb_store.hpp>
#include <fengqudb_wire.hpp>

auto db = fengqudb::Store::open_at(persistent_root + "/db", "users"); // 宿主提供 UTF-8 根目录
db.configure_write({fengqudb::StoreWriteMode::Reliable, 50, 1048576}); // 本例选严格同步写入并保存配置
fengqudb::wire::Writer user;
user.bytes_field(1, "Alice");
user.uint_field(2, 25);

auto tx = db.transaction();
tx.put("user/42", user.data());
const auto mark = tx.savepoint();
tx.put("temporary", "discard");
tx.rollback_to(mark);
auto result = tx.commit();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));

auto bytes = db.get("user/42");
if (bytes) {
    // 只读共享视图，bytes 对象存活期间有效；string() 才会复制。
    std::string_view serialized = bytes->view();
}
db.checkpoint();
```

`get()` 未命中返回 `std::nullopt`，空值仍是有效结果。快照及其游标固定创建时的数据版本和 TTL 读取时间。游标返回的行视图有效至下一次 `next()` 或关闭；需要长期持有时由调用方复制。事务游标固定创建时的私有写集，后续事务修改不会改变已创建游标。

## Python

```python
from fengqudb import Store

with Store("data/users", library="/path/to/native/library") as db:
    db.configure_write(mode="reliable")  # 本例使用严格同步事务
    with db.transaction() as tx:  # 正常离开时自动提交，异常时关闭并丢弃
        tx.put(b"user/42", b"Alice")
        point = tx.savepoint()
        tx.patch(b"user/42", 0, 5, b"Alicia")
        tx.rollback_to(point)
    with db.snapshot() as snapshot, snapshot.scan() as cursor:
        for key, value, version, expires_at_ms in cursor:
            pass
```

Python、C#、Java 的普通 get/Get 返回该语言拥有的字节副本；各语言另提供显式共享只读接口，C/C++ 默认提供共享值视图。Python 使用 `Store.transaction()`，C#/Java 使用 `Begin()` / `begin()`，C#/Java 需要显式 `Commit()` / `commit()`。所有语言的 TTL 时间都是绝对 Unix 毫秒，零表示不设置过期。

## 事务与失败边界

- 事务读取固定快照，私有修改对自己可见；修改过的读/写键以及范围内的新修改在提交时检查。读操作回滚后仍保留依赖，避免遗漏冲突。
- 保存点只共享根，创建为 O(1)。首次编辑复制被共享的路径和值，后续修改同一私有完整缓冲区；等长覆盖，删除裁剪，变长只移动必要尾部。单次补丁可写成有深度上限的日志配方，同一事务反复修改同值时可退回完整值记录。
- `CONFLICT` 需要从新事务重试。`COMMIT_UNKNOWN` 需要关闭并重开库核对实际结果，不能盲目重试非幂等操作；写入或刷盘失败会阻止该实例后续提交。
- 资源不足等暂存失败后，须回滚到先前保存点或关闭事务；不能提交部分暂存结果。取消可从另一线程发起；其他事务操作由调用方串行调用。
- 事务期限使用单调时钟，从 begin 开始计时。取消与期限在开始写入 WAL 前生效；开始写入后返回真实提交结果。它不是操作系统 I/O 的硬实时截止时间。
- 默认SYNC底层选项下，性能模式成功表示内存原子生效；可靠模式及force_reliable成功表示相关前缀可靠同步。后台失败停止接受新写；显式sync检查可靠状态，close无错误返回值。BUFFERED保留手动sync语义。
- BUFFERED 对单条至少 4 KiB 的完整暂存 put，在实际发生写者竞争时启用有界代办；无争用仍直接提交。持锁调用线程可顺手处理已排队请求，每笔独立发布并保留自身结果。最多连续处理 `min(group_max, 64)` 笔，并在累计日志达到 256 KiB 或处理约 200 微秒后停止接下一笔；单笔事务不会被拆分，实际完成时间还包含调度与 I/O。小增量、删除、多键批量、原子条件接口及受线程内 I/O 额度控制的维护保留原提交路线。此优化不改变 BUFFERED 的持久性含义；SYNC 仍使用原合组刷盘路线。合入与验证见 [定向代办合入记录](BOUNDED_SUBMIT_MERGE_20261108.md)。
- 当前自动修复物理不完整的日志尾部。完整记录的校验或语义错误返回 `CORRUPT`，不静默舍弃；恢复先用有界临时索引校验，全部通过后才截断尾部、合并元数据并发布检查点。

## 内存与文件

默认缓存 64 MiB、增量元数据 4 MiB、页编辑预算 32 MiB、每事务 64 MiB、全部事务合计 256 MiB、外部固定对象 128 MiB、1024 个快照槽。`StoreOptions` 可调整，缓存允许为零。单笔事务还受到增量索引元数据额度约束；超限在写日志前拒绝，可提高额度或缩小事务。

额度是引擎保留对象的计费上限，包含保守的对象开销估算，不等于整个进程 RSS。临时编码、页遍历栈、分配器、操作系统文件缓存和调用方结果还会占用内存。快照对增量根按完整估算计费，共享对象可能被保守地重复计费；外部值和事务游标必须及时关闭。已完成但未关闭的事务也保留其快照槽和目录租约。

普通写入只追加 WAL，并将变化键合并进内存增量索引；达到阈值或显式 `checkpoint()` 时按批固化元数据。检查点保存 WAL 中原有值字节的引用，不重写大值。旧快照继续引用旧根。当前检查点固化会持有写入屏障；文件回收可显式 `compact()`，或启用运行期后台维护策略。

目录中的 `LOCK`、`CURRENT` 和 `g<16hex>.fdb` 属于同一个数据库；旧版 `gen-*.fdb` 也可能仍被快照引用。不要手工移动其中单个文件。子事务、快照、游标和值句柄保留库所需资源，即使父 Store 已关闭；同一目录只允许一个实例持有独占租约。

## TTL 索引与分批清理

普通读取和准备共享读取器每次使用当前 Unix 毫秒判断 TTL；快照和事务固定创建时的读取时间。到期后无需等清理便不可见；`purge_expired()` 将到期记录按批提交为墓碑，降低当前存储键/值计数。它不会立即缩小文件，物理空间由 `compact()` 回收，旧快照仍可读取其创建时未到期的旧值。

```cpp
fengqudb::StoreExpiryOptions expiry;
expiry.max_keys = 1024;
expiry.max_batch_bytes = 1024 * 1024;
expiry.timeout_ms = 1000;
auto batch = db.purge_expired(expiry);
// batch.more 非零时可安排下一批；循环须允许取消或交回业务线程。
```

C 为 `fqdb_store_purge_expired`；C# 为 `PurgeExpired(StoreExpiryOptions)`；Java 为 `purgeExpired(Store.ExpiryOptions)`；Python 为 `purge_expired(max_keys=1024, timeout_ms=0, max_batch_bytes=1048576)`。选项可省略，默认最多 1024 键、1 MiB 估算元数据、不限时。键上限为 65536，仍须满足 Store 的事务和增量索引预算；单个候选也放不进批预算时返回 `LIMIT`。

结果为 `version`、`cutoff_ms`、`removed_keys`、`removed_value_bytes`、`visited_pages` 和 `more`（各语言按命名习惯转换）。`cutoff_ms` 是本批捕获时间；值字节数是逻辑计数，不等于立即释放的磁盘或进程内存。`visited_pages` 统计本次查找访问的索引页，包含缓存命中，不包含提交时的其他页读取。`more` 请求继续下一批或完成一轮回扫，允许出现删除零条但 `more` 非零的批次；不能把返回零条直接当作没有剩余数据。并发写入及时间推进会使结果变化。

索引复用原有 COW 页树和内存增量树，节点维护子树最早到期时间；无 TTL 或尚未到期的分支直接跳过，不另建常驻全键堆。清理只读取键与元数据，不读取业务值。跨批仅保留一个续扫键和弱代际引用，扫完后回到前部，避免反复扫描已处理前缀，并补查游标之前的新增或刚到期数据。续扫状态不持久化，重启从头开始；持续新增到期数据时不保证有限轮内清空。

每批是一个普通原子事务，沿用配置的 SYNC/BUFFERED 持久性和 OCC：候选被并发替换、续期或删除时，整批可返回 `CONFLICT`，由调用方重新调用。与其他维护操作共用维护租约，`cancel_maintenance()` 可取消本批；期限与取消在开始写 WAL 前生效，写入开始后返回真实提交结果。无候选时不创建新提交版本。可通过运行期 `configure` 启用后台分批清理及整理，详见扩展能力文档。

新构建可读取此前 Store 页；旧分支条目没有到期摘要时采用保守遍历，普通写入逐步补齐受影响路径，显式整理会重建完整摘要。新增分支字段可能使较旧的开发版 Store 构建无法读取新页，应使用本次匹配的库和绑定。不提供旧单文件 Database 读取或迁移。

## 在线整理与备份恢复

```cpp
fengqudb::StoreMaintenanceOptions options;
options.timeout_ms = 30'000; // 单调时钟期限；0 表示不限时
options.max_catchup_rounds = 8;
options.max_barrier_bytes = 1024 * 1024;

auto compacted = db.compact(options);
auto checked = db.verify(options);
auto backup = db.backup("data/users-backup", options); // 目标须不存在，父目录须存在
auto restored = fengqudb::Store::restore(
    "data/users-backup", "data/users-restored", options);
// restored 是维护结果；用 Store("data/users-restored") 打开恢复后的库。
```

| 操作 | C ABI | C# | Java | Python |
| --- | --- | --- | --- | --- |
| 在线整理 | `fqdb_store_compact` | `Compact` | `compact` | `compact` |
| 完整备份 | `fqdb_store_backup` | `Backup` | `backup` | `backup` |
| 校验 | `fqdb_store_verify` | `Verify` | `verify` | `verify` |
| 恢复到新目录 | `fqdb_store_restore` | 静态 `Restore` | 静态 `restore` | 静态 `restore` |
| 取消当前维护 | `fqdb_store_cancel_maintenance` | `CancelMaintenance` | `cancelMaintenance` | `cancel_maintenance` |

C++ 的参数类型为 `StoreMaintenanceOptions`，C 为 `fqdb_store_maintenance_options`；C#/Java 使用各自 Store 的维护选项类型，Python 使用关键字参数。结果包含捕获/最终版本、源/输出数据文件字节数、存储键数与值字节数、复制键次数和追赶轮数。复制次数可包含同键追赶；存储统计不是实时未过期键数，`verify` 的输出文件字节数为零。

`compact()` 捕获不可变视图和 TTL 时间，将仍存活的值写入新代际。普通值以 256 KiB 缓冲流式复制并校验；补丁链沿用减法读取器，只保留必要区间，在受单值上限约束的结果缓冲中折叠。追赶通过子树最大修改版本跳过未变化部分，同一键的连续修改只复制捕获视图中的最终值，不重放每条历史 WAL。开始时已删除或已到期的数据被丢弃；追赶中的删除可能留下墓碑，待下次整理回收。

大部分复制和同步在写入屏障之外执行，普通读写可继续。最终切换在写入屏障内完成，默认剩余变化键/值工作的估算上限为 1 MiB，同时受 Store 的 `delta_bytes` 限制；超限则继续在屏障外追赶，默认最多 8 轮。持续写入导致积压无法收敛时返回 `LIMIT`，原代际保持活动。该额度不是毫秒级停顿保证，目标刷盘和清单发布仍可能等待 I/O。显式接口同步返回；可通过运行期 `configure` 启用按阈值触发的后台整理。

新代际先同步，再原子发布 `CURRENT` 并切换内存视图。旧快照、事务和游标继续固定旧代际，退休缓存清空并停止保留新缓存项；最后一个旧视图释放后才删除旧文件。整理前创建的事务提交可能因代际切换返回 `CONFLICT`，应重开事务。长期持有快照会延迟空间回收。重启时只尝试清理同库身份且通过头部检查的非当前代际。

`backup()` 固定调用捕获的版本和 TTL 时间，在并发写入继续时生成独立身份的完整数据库。输出先写到独占创建的同级临时目录，同步并关闭所有目标句柄后发布到新目录，绝不覆盖已有目标。即使源库使用 BUFFERED，成功的备份本身也已经同步。`restore()` 要求源已存在，先执行正常打开/恢复，再校验复制到新的目标目录；不能原地覆盖。另提供 `backup_incremental` / `recover_chain` 增量备份链和版本/时间点恢复，详见扩展能力文档。进程崩溃可能留下未发布的同级 `t<16hex>.tmp` 目录，不能把它当作完整备份。

Linux 的目录发布依赖 `renameat2(RENAME_NOREPLACE)`，目标文件系统须支持原子不覆盖改名；不支持时返回 `UNSUPPORTED`。已验证 Linux 原生 `/tmp` 的完整流程；当前 WSL `/mnt/d` 的 DrvFS 拒绝此操作，返回失败且目标不存在。Windows NTFS 的发布流程已验证。

`verify()` 绕过页/值缓存，校验选中索引的父子边界、排序、最大版本、计数、引用范围及当前引用值的 CRC，也覆盖合并后的增量视图。它不校验所有已被覆盖的历史 WAL 字节，不修复损坏，也不修改已打开 Store 的文件；打开库时的正常恢复是另一阶段。

同一个 Store 同时只允许一项整理/备份/校验，竞争返回 `LOCKED`。取消可由另一线程发起，期限和取消在发布前生效；开始发布后返回实际发布结果，不能把取消当成已回滚。整理清单发布返回 `COMMIT_UNKNOWN` 时保留两代文件并阻止继续写入，须关闭重开核对；备份目录发布返回此状态时源库仍可使用，目标可能已经完整存在，须先核对再决定后续操作。期限不是操作系统 I/O 的硬实时截止时间。

## 命令行维护工具

开启 `FQDB_BUILD_TOOLS`（默认开启）构建并安装 `fqdb_store_cli`：

```text
fqdb_store_cli verify SOURCE
fqdb_store_cli compact SOURCE
fqdb_store_cli backup SOURCE NEW_DEST
fqdb_store_cli restore SOURCE NEW_DEST
fqdb_store_cli purge-expired SOURCE --max-keys 1024 --batch-bytes 1048576
```

整理、备份、校验、恢复的可选参数为 `--timeout-ms N`、`--barrier-bytes N`、`--catchup-rounds N`；`purge-expired` 接受期限、`--max-keys N` 和 `--batch-bytes N`，一次只清理一批。工具输出 JSON，成功退出码为 0，操作失败为 1，用法错误为 2。源路径必须已存在，不会自动创建空库；Windows 支持中文路径。

独立 CLI 需要获得数据库独占租约，使用前须关闭其他实例及其子句柄；进程内在线维护应调用上面的 API。Linux 安装目录中的工具通过相对 RPATH 加载同一 SDK 的库，无须设置 `LD_LIBRARY_PATH`。

## 验证入口

- CTest：`paged_foundation`、`paged_storage`、`paged_store`、`paged_store_sdk`、`paged_maintenance`、`paged_expiry`、`paged_features`。
- Linux ASan/UBSan：`sh scripts/verify-paged-storage.sh`。
- Linux TSan：`sh scripts/verify-store-tsan.sh`。崩溃子进程由 Windows/ASan 检查；TSan 只对其运行前的已知地址布局失败重试，进入测试后的故障立即失败。
- Linux Release 动态库与安装后独立 SDK 消费者：`sh scripts/verify-store-linux-shared.sh`，仅安装到隔离的构建目录。
- 多语言：`python scripts/verify-store-bindings.py --library <本次构建的库> --java-bin <JDK/bin>`，覆盖独立接口以及 Python → Java → C# → Python 共用目录的字节互通。
- CLI：`python tests/store_maintenance_cli_smoke.py <CLI> <原生库>`；Linux 原生目录完整流程与 DrvFS 拒绝发布的安全边界分开验证。

实际断电、长期压力、硬件故障及全面性能对比尚未执行。开发版不可按已完成生产验收的版本发布。
