# FengquDB

## LLM Wiki 底座

本分支提供基于 FengquDB 的增量 LLM Wiki：不可变来源、跨来源主题页、中文 BM25 检索、带引用问答、可恢复编译任务、HTTP API 与独立部署包。默认离线运行，也可连接兼容模型接口。见 [运行与架构说明](docs/LLMWIKI.md)。

原生 C++17 进程内数据库，以共享库或静态库接入 C/C++、C#、Java、Python。使用分页 Store 引擎，页和值按需加载，缓存受预算限制；核心不依赖第三方数据库、Google Protobuf 或 Abseil。

当前为 **0.3.2 / ABI 2 开发源码，尚未正式发布**。库、头文件与绑定须配套构建，旧开发 SDK 和历史测量仅供追溯。

**默认性能模式为 50 ms／1 MiB**：普通写入在完整事务内存原子生效、可读取后确认，后台按周期或 WAL 字节阈值可靠同步。崩溃可能丢失已确认但未同步的后缀；50 ms 是调度目标，不是严格损失时间上限。关键写入传 `force_reliable=true`，或切换可靠模式，等待可靠同步才返回成功。详见 [写入策略](docs/STORE_WRITE_POLICY.md)。

常用能力：主键/批读、事务与快照、保存点、TTL、字段/复合索引及约束、组合条件过滤、排序/Top N、聚合分组、等值关联、原生集合、在线整理、完整/增量备份与恢复。完整二进制缓冲和减法裁剪保留。查询语法、预算和性能边界见 [复杂查询](docs/STORE_SELECT.md)。

| 目的 | 入口 |
| --- | --- |
| 查看当前能力、契约和接入方式 | [当前文档](docs/README.md)、[产品规格](docs/PRODUCT_SPEC.md) |
| 库接口、所有权、错误与线程规则 | [Store API](docs/STORE_API.md) |
| Unity/移动端数据目录 | [目录布局](docs/STORE_LAYOUT.md) |
| 构建规范、版本控制和消毒器 | [开发说明](docs/DEVELOPMENT.md) |
| 查证性能、审查和历史实验 | [历史证据索引](docs/history/README.md) |

## 构建

Windows（安装 Visual Studio C++ 工具及 CMake）：

```powershell
cmake -S . -B build/local -A x64
cmake --build build/local --config Release --parallel
ctest --test-dir build/local -C Release --output-on-failure
cmake --install build/local --config Release --prefix artifacts/sdk
```

Linux（GCC/Clang、CMake 3.24+）：

```sh
cmake -S . -B build/local -DCMAKE_BUILD_TYPE=Release
cmake --build build/local -j
ctest --test-dir build/local --output-on-failure
cmake --install build/local --prefix artifacts/sdk
```

`-DFQDB_SHARED=OFF` 构建静态库。`-DFQDB_BUILD_DIAGNOSTICS=OFF` 排除可选 PTpma 日志后端；后端构建需要 C++20，核心和公共 C++ 接口仍为 C++17。`-DBUILD_TESTING=OFF -DFQDB_BUILD_TOOLS=OFF` 仅构建运行库。`-DFQDB_BUILD_JNI=ON` 另构建 Java JNI 适配库，配置时设置 `JAVA_HOME`。Windows 使用静态 MSVC CRT；工具链/系统库仍有平台依赖，不能把 DLL 字节数解释为整个应用的大小。

按以上命令构建时，Windows 库为 `build/local/Release/fengqudb.dll`，Linux 库为 `build/local/libfengqudb.so`。这些路径只在对应构建完成后代表该次源码；历史报告中的其他 build 路径不自动更新。消毒器配置见 [开发说明](docs/DEVELOPMENT.md)。

## 接入

新项目使用 `Store`。C++ 包含 `fengqudb_store.hpp`，C 包含 `fengqudb_store.h`；C#、Java、Python 绑定均提供 `Store`。通过 `find_package(FengquDB CONFIG REQUIRED)` 和 `FengquDB::Cpp` 链接 C++，并将 SDK 解压目录设为 `CMAKE_PREFIX_PATH`。数据库根目录由宿主传入，Unity 可使用 `Application.persistentDataPath`，详见 [Store API](docs/STORE_API.md) 和 [目录规范](docs/STORE_LAYOUT.md)。

```cpp
#include <fengqudb_store.hpp>

auto db = fengqudb::Store::open_at(persistent_root + "/db", "main");
auto pending = db.submit_put("player/42", serialized_protobuf_bytes);
// 控制台或工作线程可等待；UI 主线程应保存凭据并在后续帧 poll()。
auto result = pending.wait();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));
auto value = db.get("player/42");
```

- C++17：包含 `fengqudb_store.hpp`，链接 `FengquDB::Cpp`，示例见 `examples/basic.cpp`。
- C：包含 `fengqudb_store.h`，链接核心，示例见 `examples/basic.c`。
- C#：引用 `bindings/csharp/FengquDB.csproj`，用 `Store` 及 `IDisposable` 管理句柄。
- Java：编译 `bindings/java/io/fengqudb/Store.java`，运行时提供核心库与可选 JNI 库。
- Python：将 `bindings/python` 放入 `PYTHONPATH`，使用 `Store`，绑定仅依赖标准库。

应用值是 `bytes`，可以使用各语言官方 Protobuf 生成代码；不要求应用学习内部事务日志格式。内置 wire 编码器是按公开规范独立编写的有限实现；参考来源与验证方式见 [Protobuf 说明](docs/PROTOBUF.md)。

完整业务 `.proto`、三语言官方 DTO、Unity 类型化存档及结构升级示例见 [业务模型接入](examples/protobuf/README.md)，同载荷序列化与跨语言读取成本见 [实测记录](docs/PROTOBUF_BUSINESS_REVIEW_20261107.md)。

开发版 Store 的 Python 固定目标读取器与 Java direct ByteBuffer 可按需减少读取封送和分配，接口与生命周期见 [复用读取](docs/STORE_BINDING_BUFFERS.md)。

```python
from fengqudb import Store
with Store.open_at(persistent_root + "/db", "main", library=native_library) as db:
    # 关键业务可选可靠模式；默认性能模式也支持同步事务。
    db.configure_write(mode="reliable")
    with db.transaction() as tx:
        tx.put(b"player/42", b"serialized protobuf bytes")
        tx.put(b"team/red/42", b"")
    with db.snapshot() as snapshot:
        value = snapshot.get(b"player/42")
```

Python 事务上下文正常退出自动提交；C++、C#、Java、C 须显式提交并检查结果。事务关闭会丢弃未提交修改。冲突应从头重读、重做；`COMMIT_UNKNOWN` 须重新打开并核对业务操作标识，不能盲目重试非幂等操作。完整及增量备份使用新目录，变更订阅过早返回 `HISTORY_LOST` 时须重建基线，见对应 Store 文档。

## 语义与验证

一次仅允许一个进程打开同一数据库目录；该进程可多线程使用数据库句柄。写入串行发布，并发独立事务可共享一次可靠刷盘；读者获得完整快照，刷盘期间仍可读取上一个已发布版本。性能模式成功表示内存原子生效，崩溃或掉电可能丢失未同步后缀；可靠模式／单次强制可靠成功表示刷盘API已返回。后台故障封锁新写，已有内存状态不会自动撤销宿主副作用。显式 BUFFERED 保留手动sync语义。

详见 [Store API](docs/STORE_API.md)、[写入策略](docs/STORE_WRITE_POLICY.md)、[开发验证](docs/DEVELOPMENT.md)。测试中的进程退出和模拟 I/O 故障不能等同于真实断电测试。公平性能比较必须匹配每次提交的持久化保证；当前不宣称优于 Redis、SQLite、LMDB 或 LevelDB。
