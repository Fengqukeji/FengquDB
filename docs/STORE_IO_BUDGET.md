# 维护 I/O 字节预算

开发版 Store 可按次限制维护任务的文件读写请求字节，并返回累计读字节、写字节和文件同步尝试次数。适用于在线整理、完整及增量备份、校验、索引定义/删除、恢复。后台维护另有每轮共享额度。默认关闭，沿用原来的读写与 SYNC 合组路径；完整私有二进制缓存的覆盖和减法裁剪保持原样。

## 额度的含义

| 参数或统计 | 含义 |
| --- | --- |
| `max_read_bytes` | 本次维护最多发起的文件读取请求字节，0 表示不限 |
| `max_write_bytes` | 本次维护最多发起的文件写入请求字节，0 表示不限 |
| `io_stats.read_bytes` / `write_bytes` | 获准发起的逻辑文件请求量，包含请求失败或只完成部分 I/O 时的整个请求长度 |
| `io_stats.sync_calls` | 普通文件同步的尝试次数，包含失败尝试 |
| `job_max_read_bytes` / `job_max_write_bytes` | 后台线程一轮 TTL 清理、索引检查/续建和整理共享的累计请求额度，0 表示不限 |

缓存命中不产生文件请求，不计读取额度；缓存状态会影响实际消耗。短读写重试不会重复计算同一个逻辑请求。打开、文件大小查询、创建、改名、删除、截断和目录同步等元数据操作不在字节额度中。这些计数不等于设备物理流量、文件占用空间、每秒带宽或进程内存上限。

超额请求在发起前拒绝，不增加请求统计，返回 `LIMIT`。每个 WAL、检查点封存写单元和 CURRENT 清单在首字节前预留完整写额度，避免单纯因为写额度不足而截断该写单元。读取额度仍可能在复制候选文件期间耗尽；未发布候选按原有规则清理，原库保持可用。

真实设备写入/同步错误继续使用原有错误语义：已经开始的 WAL 可能返回 `COMMIT_UNKNOWN` 并阻止实例继续写入，需要重开核对；不会因为开启额度而将真实结果不确定降级为普通 `LIMIT`。

## 任务边界与性能

额度或统计启用时，维护任务的写入在调用线程内取得 writer gate，SYNC 先可靠同步再发布，不加入其他线程的合组队列。这样普通业务请求不消耗维护额度，维护也不借用其他线程绕开计量。此模式可能增加维护自身的同步次数；统计也有成本。未启用时不建立额度队列或额外工作线程，普通业务 SYNC 合组路径保持不变。

恢复及恢复链的计量覆盖源文件打开/恢复和私有目标操作。嵌套维护继承外层额度，后台一轮不会为每个子任务重新领取额度。后台错误通过 `runtime_info.last_status` 观察，下一轮获得新的额度。普通业务采用的性能/可靠写入策略见 [写入策略](STORE_WRITE_POLICY.md)，与本页的维护 I/O 额度开关独立。

索引 BUILDING 定义和已经成功提交的构建批次保留，可用相同定义重试续建。索引删除也可能已有部分进度提交，遵循原有重试语义。整理和完整备份没有跨调用的暂停游标，超额后放弃未发布候选；它们的额度须足以完成一次任务，反复设置极小额度不会自动拼成完整整理或备份。手动 `purge_expired` 仍使用 `max_keys`、`max_batch_bytes` 和期限；后台 TTL 清理额外受整轮 I/O 额度约束。

统计对象由调用者持有，必须存活至调用结束或错误返回；结束后再读取。并发任务应使用不同统计对象，不要在执行期间轮询或修改它。C# 仅在指定统计对象时固定其内存，Python ctypes 保持指针所有权，Java 在正常或异常返回时收集统计。

## 接入示例

C++：

```cpp
fengqudb::StoreMaintenanceIO io{};
fengqudb::StoreMaintenanceOptions options;
options.max_read_bytes = 64ull * 1024 * 1024;
options.max_write_bytes = 64ull * 1024 * 1024;
options.io_stats = &io;
auto result = db.compact(options);
// 发生异常时，catch 中同样可以查看 io。
```

C：

```c
fqdb_store_maintenance_io io = {0};
fqdb_store_maintenance_options options = {0};
options.struct_size = sizeof(options);
options.abi_version = 2;
options.max_catchup_rounds = 8;
options.max_barrier_bytes = 1048576;
options.max_write_bytes = 64ull * 1024 * 1024;
options.io_stats = &io;
fqdb_store_maintenance_info result = {0};
fqdb_status status = fqdb_store_compact(db, &options, &result);
```

C#：

```csharp
var io = new StoreMaintenanceIO();
var options = new StoreMaintenanceOptions {
    MaxWriteBytes = 64UL * 1024 * 1024,
    IOStats = io
};
db.Compact(options);
```

Java：

```java
Store.MaintenanceIO io = new Store.MaintenanceIO();
Store.MaintenanceOptions options = new Store.MaintenanceOptions();
options.maxWriteBytes = 64L * 1024 * 1024;
options.ioStats = io;
db.compact(options);
```

Python：

```python
from fengqudb import StoreMaintenanceIO

io = StoreMaintenanceIO()
db.compact(max_write_bytes=64 * 1024 * 1024, io_stats=io)
```

上述 `db` 是已打开的 Store。只需要计数时可仅指定统计对象，不设置字节限额。

后台配置的 C++ 示例：

```cpp
fengqudb::StoreRuntimeOptions policy;
policy.background = true;
policy.job_max_read_bytes = 128ull * 1024 * 1024;
policy.job_max_write_bytes = 128ull * 1024 * 1024;
db.configure(policy);
```

C# 使用 `JobMaxReadBytes` / `JobMaxWriteBytes`，Java 使用 `jobMaxReadBytes` / `jobMaxWriteBytes`，Python 在 `configure()` 中传 `job_max_read_bytes` / `job_max_write_bytes`。应按实际数据与维护工作量选择额度。

## ABI 兼容

维护 options 的 ABI 1 仅读取原 32 字节前缀；ABI 2 要求完整结构，追加两个 64 位额度和统计指针。运行配置的 ABI 1 仅读取原 56 字节前缀；ABI 2 包含两个后台额度。C 调用者须同时设置 `struct_size` 与 `abi_version`；版本不符或 ABI 2 结构不完整返回 `INVALID_ARGUMENT`。

C++ 和各语言绑定在没有额度/统计时使用维护/运行配置结构的 abi_version=1，启用后使用这些结构的 abi_version=2；它们不是全局库 ABI，当前全局库 ABI 为 2。新功能需要匹配的新原生库。Java 的维护重载使用独立 JNI 入口，避免参数位置错位；接入时应配套构建当前类、JNI 和核心库。

本批代码审查、跨平台验证、默认路径性能及包体结果见 [审查与验收记录](STORE_IO_BUDGET_REVIEW_20261107.md)。
