# Store 原生身份与 Python 公开租约

状态：2026-11-14 已实现，并在 Windows x64 / Linux x64 容器执行定向验证。此模块仅提供公共 Core 桥接，不表示 RAG/Index 已经联调或 WordGame 已被替换。

## 1. 实际 C ABI

签名与协作请求一致，独立桥接 ABI=1，Core ABI 仍为 2：

```c
typedef struct fqdb_store_bridge_info {
    uint32_t struct_size, abi_version, kind, reserved;
    uint64_t store_token, snapshot_version;
    unsigned char uuid[16];
} fqdb_store_bridge_info;

uint32_t fqdb_store_bridge_abi_version(void);
fqdb_status fqdb_store_bridge_identity(fqdb_store *, fqdb_store_bridge_info *);
fqdb_status fqdb_store_txn_bridge_identity(fqdb_store_txn *, fqdb_store_bridge_info *);
fqdb_status fqdb_store_snapshot_bridge_identity(const fqdb_store_snapshot *,
                                               fqdb_store_bridge_info *);
```

输出结构为 48 字节。调用前设置 struct_size>=48、abi_version=1；无效 header 不写输出；合法 header 后发生错误会清零身份字段。大于 48 字节的尾部保留。kind=1 Store、2 transaction、3 snapshot，reserved 输出为零。

store_token 复用 Core 的单调诊断 ID，分配器改为耗尽时报 LIMIT、永不环回。它不是对象地址，同一 Core 的 Store/事务/快照相同，关闭并重开后不同。uuid 来自已有 generation 身份，原库重开和 compaction 保持，备份/恢复副本使用独立身份；不修改持久文件格式。

**运行时边界：** token 的分配域为进程中这一份加载的 Core 运行时，按集成约定 Index/RAG 共用同一 Core。不能将独立复制并装载的第二份 Core DLL/SO 的 token 或句柄与第一份混用；lease.library 就是调用方必须使用的实际 Core 函数表。跨进程 token 不可比较。多份独立 Core 运行时的进程全局 token 仲裁不在此实现中。

Store 返回调用时的当前已发布 version；事务和快照返回捕获的基准 version，不是事务未来提交版本。事务遵循现有 ready 检查：cancelled→19、timeout→17、sealed/broken→3；身份查询不让已取消/封存事务重新可用。C 接口本身不为随意传入的悬空指针提供安全验证，调用方必须保持句柄有效。

## 2. Python 实际接口

`Store`、`StoreTransaction`、`StoreSnapshot` 均提供：

```python
identity = store.native_identity()
token = identity.store_token

with store.snapshot() as snapshot:
    with snapshot.borrow_native(expected_store_token=token) as lease:
        pointer = lease.pointer       # int 地址，仅此线程/上下文内使用
        library = lease.library       # 实际持有此句柄的 ctypes.CDLL
        identity = lease.identity     # 不可变 StoreBridgeIdentity
        print(identity.kind, identity.snapshot_version, identity.uuid.hex())

# 此处再访问 lease.pointer 会抛 RuntimeError
```

identity 包含 struct_size、abi_version、kind、reserved、store_token、snapshot_version、uuid（16 字节 bytes）。租约一经进入不可重复使用；元数据可在退出后检查，但地址失效。复制出的整数地址不能由 Python 撤回，消费者不得缓存它或交给异步任务越过租约生命周期。

expected_store_token 若传入，必须为非零 uint64，并与此 Core 实例一致，否则 ValueError。调用方仍须使用实际 library，不能仅靠 token 为另一份 Core 的裸指针背书。

生命周期规则：

- 其他线程 close 会停止新租约并等待已有租约释放，不提前释放原生句柄。
- 持有租约的同线程 close 抛 RuntimeError，退出租约后可正常关闭，避免自等待死锁。
- 事务租约持有现有 operation lock，阻止其他线程同时 put/commit/submit；同线程可顺序调用事务方法。
- cancel 不等待 operation lock，可从另一线程发出。已取得的指针仍有效，但后续原生操作返回取消状态，不能借租约绕过 ready 检查。
- Store 关闭后，已有事务/快照按原 Core 行为继续保活；最后释放后才可重开。新租约不会延长已经封存事务的使用权限。
- CPython 通过现有强引用 Owner 保活；其他解释器的路径保持显式并发计数。定向测试强制运行了后者的同步分支，未宣称已经在 PyPy 或无 GIL 解释器上实测。

租约不 commit、不更改 durability、不解析模型或调用 Index。宿主仍决定事务的 reliable 提交与回执处理。

## 3. 旧库兼容

桥接符号延迟到 native_identity/borrow_native 首次使用才绑定。缺符号或桥接 ABI 不支持时抛 Error(code=12)，旧有 Store 读写、事务和快照继续可用；不因加载绑定时缺少新符号而破坏原接口。

Windows 和 Linux 都使用真实旧 ABI2 二进制验证此行为，并另有隐藏桥接符号的测试确保普通构造/读写不尝试探测。旧库兼容测试不表示旧库具备桥接能力。

## 4. 文件与整合

必须整合的源码：

| 文件 | 修改 |
| --- | --- |
| `include/fengqudb_store.h` | ABI1 结构、版本和三个身份接口 |
| `src/paged/core.hpp` | Core ID 耗尽保护，防令牌环回 |
| `src/store.cpp` | C 身份查询和输出校验；无持久格式变化 |
| `bindings/python/fengqudb.py` | 可选符号探测、不可变身份、公开租约及关闭/事务串行化 |
| `tests/store_bridge.py` | 独立 14 项生命周期/跨库/取消/重开/旧库测试 |

不改根 CMake，既有 Linux `fqdb_*` 导出规则已覆盖新符号。Index 工作树需要将四个源码文件作为同一批整合并重建共用 Core；不能只复制新 Python 绑定后宣称旧 Core 支持新能力。

## 5. 实际验证

独立构建输出位于本工作树 `build/bridge-win`、`build/bridge-linux`，未使用其他工作树 DLL 代替新库。旧二进制仅供兼容测试。

| 平台 | 实际执行 | 结果 |
| --- | --- | --- |
| Windows x64，MSVC 19.51，Python 3.12 | store_bridge.py，含真实旧 DLL | 14/14 通过，无跳过 |
| Linux x64，Docker fqdb-lang-dev:1，GCC 15.2 | store_bridge.py，含真实旧 SO | 14/14 通过，无跳过 |
| 两平台 | 现有 fqdb_store_sdk_tests，含 C 头文件编译和 C++ SDK | 通过 |
| 两平台 | 现有 store_shared_smoke.py | 通过；Linux 数据目录在容器原生 /tmp |

Linux 的共享读取脚本最初使用 Windows 挂载目录，其数据文件返回 UNSUPPORTED；改用容器 /tmp 后通过。不把挂载文件系统的选项限制当作桥接通过，也不宣称已修复该文件系统行为。

测试中 Store 显式使用 BUFFERED；重开后的持久化断言使用 force_reliable=True。初版测试误将普通 buffered commit 当可靠提交，已修正测试；没有为此改变引擎持久性策略。

复现入口：

```powershell
python tests/store_bridge.py --library build/bridge-win/Release/fengqudb.dll --legacy-library D:/study/FengquDB/build/qp-win/Release/fengqudb.dll -v
build/bridge-win/Release/fqdb_store_sdk_tests.exe
python tests/store_shared_smoke.py build/bridge-shared-win build/bridge-win/Release/fengqudb.dll
```

Linux 用同一测试脚本，library 指向新构建 libfengqudb.so，legacy-library 指向旧发行 SO；共享读取的数据根使用新建的容器 /tmp 目录。未运行全量数据库回归、设备测试或 RAG/Index 联调；本次只覆盖桥接及直接相关已有生命周期路径。
