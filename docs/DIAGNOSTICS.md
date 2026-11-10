# 数据库日志与定位

核心为打开/恢复、提交、同步、检查点、变更游标错误产生数值事件：数据库进程内 ID、版本、阶段、文件偏移、状态、系统错误码、操作数、载荷字节、耗时。事件不含键、值或数据库路径。默认记录警告、错误及达到 5 ms 的成功提交，普通点读不记录。未注册接收器时不启动线程，跳过取时和回调。

## 接入

```cpp
#include <fengqudb_diagnostics.hpp>
#include <fengqudb_store.hpp>
#include <filesystem>

// persistent_root 由宿主传入，使用可写的持久化绝对目录。
const auto root = std::filesystem::u8path(persistent_root);
fengqudb::Diagnostics diagnostics((root / "log").u8string());
auto database = fengqudb::Store::open_at((root / "db").u8string(), "main");
auto transaction = database.transaction();
transaction.put("key", "protobuf bytes");
auto committed = transaction.commit();
if (!committed) throw fengqudb::Error(static_cast<fqdb_status>(committed.status));
auto conflicts = diagnostics.query(R"({"event":"transaction.conflict","limit":100})");
diagnostics.flush(2000);
auto state = diagnostics.status(); // 检查 dropped、io_errors、callback_errors
```

CMake 链接 `FengquDB::Cpp`、`FengquDB::Diagnostics`。公共头文件要求 C++17，可选后端内部要求 C++20。C 管理入口见 `fengqudb_diagnostics.h`，返回的 JSON 用核心 `fqdb_buffer_free` 释放，避免 Windows 跨 CRT 释放。C#、Java、Python 通过同一个共享核心产生的事件也会自动进入此接收器；目前这些封装没有 Diagnostics 管理类，宿主可经 C ABI 初始化。

推荐统一使用 `<持久化目录>/db/<库名>`、`<持久化目录>/log` 和 `<持久化目录>/bak/<备份名>`，见 [Store 目录规范](STORE_LAYOUT.md)。日志目录由接口显式传入，不从工作目录猜测，不自动写入安装目录或系统临时目录。

`DiagnosticsOptions.min_level=FQDB_LOG_INFO` 增加打开/同步成功事件；`slow_commit_us=0` 可临时记录所有提交，负载明显更高。`reader_only=true` 仅查询，不注册核心接收器、不启动写入线程。一个核心库实例同时仅有一个接收器；静态核心分别链接进多个模块会各有独立注册表，应统一由宿主提供核心。

`fqdb_diagnostics_cli DIRECTORY status` 查看状态，`fqdb_diagnostics_cli DIRECTORY query '{"event":"commit.failed","limit":100}'` 查询。CLI 为只读入口；Windows 按 Shell 规则传递 JSON 引号，Linux 需将 SDK lib 加入动态库加载路径。

## 常用问题

| 事件 | 定位信息 |
| --- | --- |
| database.open.failed | status/os_error/stage/file_offset，区分独占锁、路径、I/O 与完整数据损坏 |
| database.recovery.truncated_tail | repaired_bytes、恢复版本；完整 CRC 错误不会被自动修复 |
| database.transaction.conflict | base_version、当前 version；重新开始事务并重读 |
| database.commit.failed / database.sync.failed | write/flush 阶段与 COMMIT_UNKNOWN；重开核对业务幂等标识 |
| database.commit.slow | duration_us、durability、count、bytes，结合系统指标判断锁竞争/大批量/刷盘 |
| database.checkpoint.failed | 发布阶段和结果；源库仍可使用，结果不确定时检查目标 |
| database.changes.failed | HISTORY_LOST 时从快照重建上层索引 |

提交耗时包括锁等待、准备、写入和封存资源释放；慢提交不等同于磁盘慢。成功阶段为 complete，失败保留失败阶段。database_id 仅在进程实例内有效，跨重启结合 writer_id、process_id、时间定位。日志状态中的 directory 是日志自身的路径。

## 资源与可靠性

后端排队上限为 4096 条和约 2 MiB 估算内存，每批最多 256 条，文件约 8 MiB 轮换。队列限额不包含处理中的批次、序列化缓存、线程栈和分配器开销，不是进程总内存限额。

拥塞或 I/O 错误允许丢弃，优先保留错误；status 包含 submitted、written、dropped、cleared、io_errors、callback_errors 和高水位。flush 成功只表示调用时的队列水位已处理，可能是写入、丢弃或清理；不提供数据库级断电持久化保证，不能作为审计/恢复依据。日志写入失败不回滚数据库提交。

configure 接受 enabled、retention_days、max_bytes；enabled=false 停止接收，既有线程仍等待，close 才注销并结束线程。clear 仅清理本组件识别的日志，保留其他文件，建议使用独立目录。管理调用不可与同一句柄的 close/销毁并发。

默认保留 7 天、合计 64 MiB，单文件约 8 MiB 轮转。首次写入、轮转和后续写入触发的周期维护会回收过期或超量的非活跃自有文件，并为当前文件预留增长空间；受保护的活跃文件不能强制删除，相关状态可查看 protected_files、directory_truncated 和 retention_errors。Windows/Linux 跨越默认上限的实际压力结果见 [资源与日志验证](RESOURCE_REVIEW_20261107.md)。

自定义接收器见 `fengqudb_log.h`：回调在数据库锁释放后执行，可能并发，事件仅在调用期间有效。递归日志抑制、回调异常隔离；禁止在自身回调中关闭自身接收器。close 等待已进入的回调结束，回调应尽快入队返回。

## 来源与适配

从用户 PTpma 工作区读取并移植，原工作区未修改：

* `D:/work/PTpma/src/core/diagnostics.cpp`，SHA256 `5588253f39ec2ffad73a8e259930d564af2d133edba586af4f3f90fec970ecc6`。
* `D:/work/PTpma/src/core/diagnostics.h`，SHA256 `f42efb541436bd94bf67b558725bb1c059f59b0d735ba69d361fc25e495d76f4`。

本地为 `src/diagnostics/ptpma_log.*`：保留有界异步队列、批处理、轮换/保留、清理代际、查询与计数；更换命名空间，移除桌面模型依赖、未使用的操作跨度/工厂/默认目录工具，缩减字段白名单。临时字段列表替代逐事件 map，内部状态改为唯一所有权。

JSON 使用 nlohmann/json v3.12.0，来源/commit/SHA256 见 `third_party/json/origin.json`，MIT 许可随 SDK 安装，仅进入可选后端。`-DFQDB_BUILD_DIAGNOSTICS=OFF` 排除后端和 JSON，核心接收器接口仍可使用。
