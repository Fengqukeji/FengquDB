# Store 目录与 Unity 存档

Store 使用目录，库、头文件和绑定须配套使用。项目尚未发布，旧单文件引擎已删除。

每个 Store 单独指定容器根目录与库名，没有进程级可变根目录，不受其他实例设置影响。

`open_at(root, name)` 的实际目录严格为 `<root>/<name>`，不会再自动添加 `db` 或应用名。推荐宿主统一选择 `<持久化目录>/db` 作为 `root`，日志显式选择同一持久化目录下的 `log`，备份显式选择 `bak`。这三个短目录名是宿主约定，打开数据库不会自动启用日志或创建备份目录。根目录在打开实例时设置；切换位置须关闭相关实例及子句柄，再从新位置打开。

| 语言 | 打开 | 只计算绝对路径，不创建文件 |
| --- | --- | --- |
| C | `fqdb_store_open_at(root, name, options, &db)` | `fqdb_store_path_at(root, name, &buffer)` |
| C++ | `Store::open_at(root, name)` | `Store::path_at(root, name)` |
| C# | `Store.OpenAt(root, name)` | `Store.PathAt(root, name)` |
| Java | `Store.openAt(root, name)` | `Store.pathAt(root, name)` |
| Python | `Store.open_at(root, name, library=...)` | `Store.path_at(root, name, library=...)` |

路径为 UTF-8，根目录可含中文。库名限 1～32 个 ASCII 字母、数字、`_`、`-`；拒绝分隔符、`..`、冒号、空格和 Windows 保留设备名。建议固定小写名，如 `main`、`slot1`；大小写是否相同由文件系统决定。

CREATE 启用时允许创建根目录的父级；关闭时缺失返回 NOT_FOUND，不创建目录。参数无效时不创建目录。`open_at` 拒绝库名位置已有的符号链接和 Windows 重解析点；根目录可由平台映射。它不是用于对抗其他进程替换目录的安全沙箱。

打开时固定规范的绝对路径；之后改变当前工作目录不会改变写入位置。原来的 `Store(directory)` 同样固定打开时的绝对路径，但父目录须存在。业务应选用 `open_at`，不要依赖工作目录。

## 布局与路径长度

```text
<持久化根>/
  db/
    main/
      LOCK
      CURRENT
      WRITE                    # 修改过写入策略后保存的40字节配置
      g0000000000000001.fdb
      g0000000000000001.blk     # 新建非分段库的大记录通道
    slot1/
      LOCK
      CURRENT
      g0000000000000001.fdb
  log/                         # 可选诊断模块，显式指定
  bak/                         # 仅执行显式备份时使用
```

从传入的容器根目录到库目录只有一层，库目录内全部平铺。代际数据文件固定 21 字符；清单和恢复临时文件为 `m<16hex>.tmp`、`r<16hex>.tmp`，同样 21 字符，留在库目录内。整理期间可有多个代际；不要手工删除旧文件，旧快照可能仍在使用。

写入策略保存在同目录 `WRITE`，临时发布文件为 `w<16hex>.tmp`，没有额外子目录。未保存配置的库按默认50 ms／1 MiB打开；损坏配置返回 CORRUPT，不静默重置。完整备份和恢复链基底携带该配置，增量数据归档不用于回滚配置。详见 [写入策略](STORE_WRITE_POLICY.md)。

开发源码现已支持按需创建分段 Store。主段仍为 `g<16hex>.fdb`；后续段为 `s<16hex>_<8hex>.fdb`，固定 30 字符，全部放在同一库目录。每个物理文件最多 64 MiB 数据加 64 KiB 头区，即 67174400 字节；这是单文件长度上限，不是数据库总容量或内存上限。后台维护证明整段不可达且旧读者退出后才删除它；段号保持逻辑顺序，不为回收而反复拼接存活二进制。

| 语言 | 创建分段库的选项 |
| --- | --- |
| C | `options.flags |= FQDB_STORE_SEGMENTED` |
| C++ | `StoreOptions options; options.segmented = true;` |
| C# | `new StoreOptions { Segmented = true }` |
| Java | `Store.Options options = new Store.Options(); options.segmented = true;` |
| Python | `Store.open_at(root, "main", segmented=True, library=...)` |

默认新建非分段库采用同代际的 `.fdb` 和 `.blk` 两条数据通道，文件名均为 21 字符，按一次同步的实际字节数选择通道；`.blk` 不是日志诊断文件，不能单独删除或复制。每笔事务仍属于同一 Store 引擎。分段创建保留原有分段布局；打开现有库从文件头识别布局，不要求再次传创建选项。

备份、恢复及整理的批量构建输出使用直接记录布局，避免整库复制期间额外积累数据缓冲。对双通道代际执行带 I/O 计量的写入或原地稀疏回收时，会按预算逐步建立逻辑字节完全一致的直接镜像，发布后继续使用直接布局。旧快照的逻辑偏移不变；转换期间可有同目录的 `x<16hex>.tmp`（21 字符），未完成镜像不被 CURRENT 选中，进程关闭时清理本次拥有的临时文件。崩溃遗留的未发布文件不应由宿主按通配符任意删除。

该选项不会把已有非分段库直接转换成分段库，也不能让旧版原生库读取新格式。分段创建与后续扩段同样要求文件系统支持原子不覆盖发布，Windows 共享给 Linux 的挂载盘可能不满足；Linux 原生卷的分段检查与 Windows x64 的相关回归均已验证。

例如传入 `/persistent/db` 和 `main`，实际写入 `/persistent/db/main`。在数据库这一层之外，库不额外拼接用户 ID、集合名、键或日期目录；业务键再长也不会成为文件名。`bak` 下每份显式备份同样只占一个平铺的 Store 目录。

Windows 对完整内部路径采用小于 240 个 UTF-16 单元的保守限制；POSIX 使用小于 960 字节。普通目录检查预留 24 单元/字节用于内部名称；分段创建进一步预留 32，容纳最长段名。每个目录组件最多 255 个原生单元（Windows 为 UTF-16 单元，POSIX 为字节）。路径或组件超限先返回 LIMIT，避免写入数据后才遇到名称超限。平台自身更低的限制、权限和空间仍可能导致 IO_ERROR。此处限制内部增加的层数，不能缩短平台给出的根目录。

旧 Store 的 `gen-<32hex>-<16hex>.fdb`（57 字符）继续可读，打开不强行改名。下一次成功整理才写短名；旧快照释放后回收旧长名代际。较旧开发库不保证能打开新短名布局，升级时使用配套绑定并保留备份。

普通读写、检查点、恢复和整理不在系统临时目录或程序目录创建数据库文件。备份、恢复、迁移的输出由调用者显式指定；先在目标父目录创建短名 `t<16hex>.tmp`，成功后原子发布。目标不得存在，父目录须存在。崩溃可留下未发布临时目录，确认无任务运行后由宿主维护，库不扫描其他目录清理。

输出前同时检查最终目录和临时目录加内部文件的完整路径；即使最终目录名很短，临时目录仍须预留空间。路径预算不足时，在创建临时目录前返回 LIMIT。备份父目录建议同样选用持久化根下的短目录，如 `bak`。

备份、恢复、迁移的目标文件系统还须支持不覆盖现有目标的原子目录发布。Linux 使用 `renameat2(RENAME_NOREPLACE)`；不支持时返回 UNSUPPORTED，不退化为可能覆盖目标的重命名。在本次 WSL 验证中，`/mnt/d` 共享盘对此操作返回 EINVAL，而 Linux 原生 `/tmp` 目录支持；这只限定发布工具的目标位置，不表示所有 WSL 存储接口都不可用。移动平台须验证实际持久化目录及其文件系统。

诊断模块须显式传入日志目录，推荐 `<持久化根>/log`，不要与库文件混放。新日志名最多约 53 字符，日志目录额外预留 56 单元/字节；日志目录同样检查完整路径和每段最多 255 个原生单元，超限先于目录创建返回 LIMIT。日志是可选模块，不会因打开 Store 自动写入其他目录。

## C++ 设置根目录

```cpp
#include <fengqudb_store.hpp>

// 由宿主传入可写持久化目录；移动平台不要硬编码沙箱绝对路径。
auto db = fengqudb::Store::open_at(persistent_root + "/db", "main");
auto slot = fengqudb::Store::open_at(persistent_root + "/db", "slot1");
```

`persistent_root` 是宿主提供的 UTF-8 `std::string`。不同实例可使用不同根目录；实例打开后改变当前工作目录不会改变文件位置。

## Unity

```csharp
using System.IO;
using System.Text;
using FengquDB;
using UnityEngine;

var root = Path.Combine(Application.persistentDataPath, "db");
using (var db = Store.OpenAt(root, "main")) {
    var key = Encoding.UTF8.GetBytes("player/42");
    byte[] protobufBytes = LoadYourSerializedPlayer();
    using (var receipt = db.SubmitPut(key, protobufBytes)) {
        await receipt.WaitAsync(); // 包含本代码的方法须为 async；不要 .Wait()/.Result
    }
    byte[] saved = db.Get(key);
}
```

`LoadYourSerializedPlayer` 代表业务序列化逻辑。完整生命周期示例见 `examples/unity/FengquSave.cs`，不依赖 UnityEditor。不要把可写数据库放入 Assets、StreamingAssets、应用包或安装目录；`persistentDataPath` 由 Unity 按 Android/iOS 沙箱选择。卸载、清除应用数据和平台备份策略由宿主负责。

需要直接使用业务 PlayerSave 类时，可按 [Protobuf 业务示例](../examples/protobuf/README.md) 生成 DTO，并接入 `examples/unity/TypedFengquSave.cs` 与业务 PlayerSaves 封装，目录仍为 `<persistentDataPath>/db/main`。

引用 `bindings/csharp/FengquDB.csproj` 生成的 netstandard2.1 程序集，或将 `bindings/csharp` 下全部绑定 `.cs` 源文件一起纳入启用 unsafe 的程序集。Android/Windows 的 P/Invoke 名为 `fengqudb`；`UNITY_IOS && !UNITY_EDITOR` 下为 `__Internal`，配合静态库链接。移动平台与 IL2CPP 需要在目标设备验证。Android 共享库仅导出 C ABI，并启用链接裁剪及 16 KiB LOAD 段对齐；这不等于完成新 Android 版本的打包与真机兼容验收。

Android ARM64 构建示例（NDK 路径由宿主提供）：

```sh
cmake -S . -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DFQDB_SHARED=ON -DFQDB_BUILD_JNI=OFF \
  -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF -DBUILD_TESTING=OFF
cmake --build build/android-arm64 --parallel 2
cmake --install build/android-arm64 --strip --prefix build/android-sdk
```

将安装输出的 `libfengqudb.so` 配置为 Unity Android ARM64 原生插件。C# 直接调用 C ABI，无需 Java JNI。iOS 须在 macOS 用 Apple 工具链构建静态库，使用 `CMAKE_SYSTEM_NAME=iOS`、`FQDB_SHARED=OFF`，按设备/模拟器架构分别构建并配置链接。Windows/Android 编译不能证明 iOS 构建或真机运行通过。

普通存档默认采用性能模式，内存生效后确认；关键存档应传 force_reliable 或采用可靠模式，在成功返回后已请求系统可靠同步。显式 BUFFERED 须主动 `Sync()`；暂停回调可补充同步，突然终止仍可能丢失尚未同步的数据。设备可靠性须通过真实断电验证。

当前写入默认内存确认、后台50 ms／1 MiB刷盘；关键存档强制可靠并检查结果。全局ABI2绑定必须与当前原生库配套，见[写入策略](STORE_WRITE_POLICY.md)。
