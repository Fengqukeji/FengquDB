# C++17 接入

包含 `<fengqudb_store.hpp>` 并链接原生核心。C++ RAII 封装通过 C ABI 调用同一个 Store 引擎，STL 和异常留在调用方。`fengqudb.hpp` 只提供公共错误、缓冲等工具；旧 Database 接口已删除，项目尚未发布。

## CMake

```cmake
find_package(FengquDB CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE FengquDB::Cpp)
```

配置时用 `CMAKE_PREFIX_PATH` 指向 SDK 安装前缀。也可 `add_subdirectory(path/to/FengquDB)` 后链接同一目标。C ABI 使用 `FengquDB::Core`。运行时 Windows 要能找到 `fengqudb.dll`；Linux 用安装路径、RPATH 或 `LD_LIBRARY_PATH`。

`-DFQDB_SHARED=OFF` 构建静态库；导出目标传递 `FQDB_STATIC` 和系统链接依赖。Windows 静态库使用 `/MT`（Debug `/MTd`），调用方须使用兼容 CRT。共享库分配的 C 缓冲由 `fqdb_buffer_free` 释放。

## 事务与所有权

```cpp
#include <fengqudb_store.hpp>

auto db = fengqudb::Store::open_at(persistent_root + "/db", "main");
db.configure_write({fengqudb::StoreWriteMode::Reliable, 50, 1048576}); // 本同步示例选择严格模式并保存
auto tx = db.transaction();
tx.put("player/42", serialized_protobuf);
tx.put("team/red/42", "");
auto result = tx.commit();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));

if (auto value = db.get("player/42")) {
    consume(value->view()); // 视图由 value 固定，string() 显式复制
}
```

键和值是带长度的二进制字节；空值有效，缺失返回 `std::nullopt`。Store、事务、快照、游标和值句柄不可复制、可以移动，析构释放句柄，未提交事务直接丢弃。`commit()` 返回状态和版本，正常冲突也通过该结果返回；不要套用旧 API 的 `try_commit()`。

事务失败、取消、期限及 COMMIT_UNKNOWN 的处理见 [Store API](STORE_API.md)。性能模式同步写及凭据完成均表示内存生效；可靠模式或单次 `commit(true)` 等待刷盘。`put/erase/patch` 也支持强制可靠参数。`submit_put`／事务 `submit` 返回凭据，提前析构未完成凭据会等待，详见 [写入策略](STORE_WRITE_POLICY.md)。全局ABI为2，须配套重编译。

## 快照与读取复用

```cpp
auto snapshot = db.snapshot();
auto cursor = snapshot.scan("player/", "player0");
while (auto row = cursor.next()) consume(row->key, row->value);

auto key = db.prepare_key("player/42");
auto value = key.get();
```

快照固定数据版本与 TTL 时间，游标行只在下一次 `next()` 或关闭前有效。值对象独立固定不可变数据。准备键复用编码，并可用 `get_into` 写入调用方缓冲，容量不足与生命周期见 [复用读取](STORE_BINDING_BUFFERS.md)。

重复访问同一个键时，在循环外准备句柄。发布未变化且缓存值仍存活时，`key.get()`和`get_into`复用已核对元数据与弱值；写入、TTL、时间回退和文件代变化仍会重新核对。准备键不会额外强持有业务载荷，其固定元数据计入pin预算。当前收益范围见[按键读写优化记录](STORE_KEY_CRUD_MERGE_20261110.md)。

保存点共享私有根，后续编辑按需隔离；等长补丁覆盖，删除裁剪，变长只移动必要尾部。公共参数与批量/原子调用见 [Store 操作](STORE_OPERATIONS.md)，完整示例见 `examples/basic.cpp`。

同一 Store 可由不同线程发起独立调用；同一事务、游标等可变对象由调用方串行访问。关闭宿主句柄之前须结束使用它的调用，子句柄可能继续固定租约和资源。
