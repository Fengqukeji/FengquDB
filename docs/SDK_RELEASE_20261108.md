# 历史开发包：FengquDB 0.3.2 Windows/Linux x64 SDK

本文冻结记录此前双引擎开发包，项目尚未正式发布。当前源码已移除旧 Database 引擎与迁移器；本文的包体、133 项导出和性能身份不能用于当前库，最新状态见 [核心审查](STORE_CORE_REVIEW_20261108.md)。

本轮交付目录为 `artifacts/sdk-0.3.2-20261108/`，分别提供 Windows ZIP 和 Linux tar.gz。沿用已经完成原生、语言接入、资源和正式性能验证的核心，不因打包重新编译；全部文档刷新为当前工作区版本并调整包内相对链接，未附带的历史构建日志明确标为源码工作区路径。旧发布包和预览归档保持原样。

## 使用入口

新项目使用目录型 `Store`。C/C++ 头文件在 `include/`，Windows 核心在 `bin/fengqudb.dll`，Linux 核心在 `lib/libfengqudb.so.1`，后者为指向版本文件的相对链接。CMake 设置解压目录为 `CMAKE_PREFIX_PATH`，使用 `find_package(FengquDB CONFIG REQUIRED)` 和 `FengquDB::Cpp`；公共 C++ 接口要求 C++17。

```cpp
#include <fengqudb_store.hpp>

auto db = fengqudb::Store::open_at(persistent_root + "/db", "main");
db.put_if_changed("player/42", serialized_protobuf_bytes);
auto value = db.get("player/42");
```

`share/fengqudb/bindings/` 提供 C#、Java、Python 源码，`share/fengqudb/examples/` 提供 C++、业务 `.proto`、三语言 DTO 及 Unity 存档示例。宿主可传入 `Application.persistentDataPath` 作为根目录。业务结构使用自己的 `.proto` 和各语言官方生成代码；数据库接收序列化字节，不要求复用内部日志 schema。

诊断库和 Java JNI 是独立可选库，C#/Java 运行时及业务 Protobuf 运行时由宿主提供。核心不依赖 SQLite、MySQL、Redis、Google Protobuf 或 Abseil。备份、恢复、校验、PITR 等通过 Store API 调用；命令行工具可从源码启用 `FQDB_BUILD_TOOLS` 构建。

## 核心身份与包体

| 平台 | 核心字节数 | SHA256 |
| --- | ---: | --- |
| Windows x64 | 865792 | `e57953d80fcbd9ef6e28976db39664b44a7fcb907517afa56fbe40ec488c32d7` |
| Linux x64，已裁剪符号 | 847400 | `7b4404adfa4c770cd4548abc67fdeda3d464aeea1c04f956f9daf25b7c172a0b` |

两平台保留相同的 133 项 C ABI，无测试导出。完整归档成员、字节数和 SHA256 见交付目录的 `file-manifest.json` 与 `SHA256.json`；`source-manifest.json` 和包内 `SOURCE_MANIFEST.json` 记录源码与文档身份。Windows 为共享 DLL 加导入 LIB；静态库可用 `FQDB_SHARED=OFF` 从源码构建。

Linux 核心直接引用的最高运行库版本节点为 GLIBC 2.34、GLIBCXX 3.4.30、CXXABI 1.3.15；可选诊断另需 GLIBC 2.38、GLIBCXX 3.4.31。目标系统必须提供完整所需符号；本轮实际运行环境为 Ubuntu 26.04，不能据此宣称已验证更旧发行版。详细符号记录见 [SDK 预览身份](SDK_PREVIEW_20261107.md)。

## 验证与性能

当前核心已经过两平台安装后 C++17 消费者、两平台三语言两布局的十二组批量接入验证；Linux C# 实际执行，Java 实际加载 JNI。Windows 业务载荷成本测量为 25 条命令、69 个样本，缓冲复用建议见 [语言调用成本](SDK_CALL_COST_20261108.md)。

功能及故障证据见 [验收记录](GOAL_ACCEPTANCE_20261107.md)。其中包括实际 ENOSPC、分配失败、OOM、外部 SIGKILL、有限持续回收及默认日志轮转/过期；当前核心另完成超过 2 GiB 活跃数据在 512 MiB、无 swap 下的旧快照、批量读取、自动维护和独立重开组合验证。缓存额度不等于 RSS 或容器总内存硬上限；记录的 cgroup 峰值约 512.23 MiB，无 OOM。

Linux 2 CPU/4 GiB 的五方案正式比较完成 1280 项扫描、960 项三轮峰值复测，见 [完整结果](DOCKER_FINAL_COMPARISON_20261108.md)。本库在本次配置的 4 KiB 复制读取和批量读取领先；部分小值读取、批量写和可靠写仍有 SQLite 或 Redis 领先。数字是测试并发点中的峰值中位数，不是理论上限。

聚合位置写候选通过相关 Release 六项与 ASan/UBSan 六项、96 次模型/独立重开校验，但收益与回退混合，未采用，见 [I/O 评估](STORE_SYNC_IO_REVIEW_20261108.md)。没有改变文件格式、同步屏障或 0.3.1 完整私有缓冲的减法裁剪。

最终归档逐成员校验、刷新全部文档及解包后独立 CMake 消费者的结果保存在 `build/final-sdk-20261108/`。旧阶段报告只对应各自当时的源码；最终交付审计负责串联其适用范围。

本轮交付 Windows/Linux x64。Android/iOS、Unity IL2CPP 真机、硬件掉电和多年生产运行不属于已验证范围。向量检索、全文检索、RAG/Wiki 按用户要求留待以后设计；完整 SQL、远程服务、权限、复制和跨进程协作不在本轮范围内。
