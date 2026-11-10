# Windows / Linux x64 SDK 预览包

本包来自已采用的开发源码，包含分页 Store、可选分段文件、事务额度修复及请求内批量索引页复用。核心版本号为 0.3.2；这是保留的历史预览包，不覆盖旧 `artifacts/release-0.3.2/`。五方案比较和兼容 I/O 候选评估均已完成，候选未采用，原核心不变；新的独立 SDK 交付见 [交付说明](SDK_RELEASE_20261108.md)。下文保留预览包当时的身份与验证范围，原预览归档不改写。

## 位置与接入

产物位于 `artifacts/sdk-preview-20261107/`：Windows ZIP、Linux tar.gz、文件清单、校验值和本说明。每个平台包提供原生共享库、C/C++ 头文件、CMake 配置、Python/C#/Java 绑定源码、业务 Protobuf 与 Unity 接入示例。诊断和 JNI 为独立可选库，C#/Java 的运行时由宿主提供。

Windows 使用 `bin/fengqudb.dll`；Linux 使用 `lib/libfengqudb.so.1`，保留相对符号链接及实际版本文件。CMake 消费项目设置解压目录为 `CMAKE_PREFIX_PATH`，再用 `find_package(FengquDB CONFIG REQUIRED)` 和 `FengquDB::Cpp`。C++ 样例见包内 `share/fengqudb/examples`，各语言绑定位于 `share/fengqudb/bindings`。

数据库根目录由调用接口设置，Unity 可传入 `Application.persistentDataPath`。本包只交付 Windows/Linux x64；移动端工具链和真机验收边界见主文档，不能由桌面验证替代。

## 验证与身份

当前源码合入前归档、完整 200 项构建输入清单、接入程序、逐命令日志及二进制哈希保存在 `build/bulk-adoption-20261107/`。与隔离候选相比，五个采用文件逐字节一致；另保留 SQLite 基准客户端修正。构建输入清单不宣称涵盖 SDK 全部文档、第三方工具链和依赖；交付包另有完整成员清单。

Windows 与 Linux 均从主工程重新构建，安装后的 C++17 普通库/Store/诊断消费者通过；两平台各三语言、两种文件布局，共十二组批量 32 键验证通过，覆盖重复/缺失键、检查点前后、旧快照、整理、返回值独立所有权和重开。Linux C# 在 Linux .NET 运行时实际执行，Java 实际加载配套 JNI。Linux 先裁剪符号再执行这些接入检查。

| 核心 | 字节数 | SHA256 |
| --- | ---: | --- |
| Windows x64 | 865792 | `e57953d80fcbd9ef6e28976db39664b44a7fcb907517afa56fbe40ec488c32d7` |
| Linux x64（裁剪） | 847400 | `7b4404adfa4c770cd4548abc67fdeda3d464aeea1c04f956f9daf25b7c172a0b` |

两平台 C ABI 导出保持相同的 133 项，无测试导出。Windows 核心仅依赖 KERNEL32；Linux 核心依赖系统 C/C++ 运行库。两者均不依赖 SQLite。

### Linux 运行库符号要求

交付后已对原 Linux 归档做离线 ELF 核对，原归档和库文件未改写。先验证归档 SHA256，再逐库匹配已验收的字节数和 SHA256，读取 GNU 版本需求节；完整节点保存在 `build/final-comparison-ready-20261108/linux-runtime-abi.json`。各文件直接引用的最高版本节点如下：

| 库 | GLIBC | GLIBCXX | CXXABI |
| --- | --- | --- | --- |
| 核心 | GLIBC_2.34 | GLIBCXX_3.4.30 | CXXABI_1.3.15 |
| 可选诊断 | GLIBC_2.38 | GLIBCXX_3.4.31 | CXXABI_1.3.9 |
| JNI | GLIBC_2.14 | GLIBCXX_3.4.21 | CXXABI_1.3.9 |

目标系统的运行库须提供完整所需符号；JNI 和诊断还依赖核心，不能单看各自一行判断兼容性。相同核心正在 Ubuntu 26.04 镜像中执行原生比较，镜像标签与源码、二进制身份均有记录。上述离线核对不等于旧发行版运行验证；需要支持更旧运行库时，应使用目标基线工具链重新构建源码。

首次新接入探针遗漏 C# unsafe 编译选项，随后续跑日志读取又遇到默认 GBK 解码问题；两次失败均属于探针配置，原始程序、日志和失败报告保留，修正后使用 v3 报告完成剩余检查。没有为修正探针改变数据库代码，也没有重写失败报告为成功。

## 优化范围与尚未完成项

批量路径只在单次至少八个键、且缓存启用时复用最多三层不可变索引页；按代际、偏移、读取上界核对，不跨请求保留，不改变格式、ABI、写入调度和 0.3.1 完整二进制缓冲的减法裁剪。隔离候选已经通过 Windows 七项、Linux ASan/UBSan 七项、四次生产共享库并发探针，以及 108 项交替 A/B 和独立重开；源码相同的验证不再机械重跑。

三轮短测中批量吞吐中位数提高约 6%～20%；4 KiB 的 8/32 线程批量 P99 分别增加 2.79%/8.16%，不能概括为全部场景或尾延迟都改善。详细配置和点读/混合控制组见 `DOCKER_CURRENT_COMPARISON_20261107.md`。

当前 Windows SDK 的完整成本和缓冲复用建议见 [调用成本](SDK_CALL_COST_20261108.md)；受测核心的单线程 I/O 观察、文件层对照及否决直接填零的兼容性证据见 [I/O 诊断](STORE_SYNC_IO_REVIEW_20261108.md)。这些诊断没有修改核心、格式或 0.3.1 减法裁剪。

SQLite 锁等待处理只在性能对照程序内，为完成高并发固定工作量而进行有界重试，等待仍计入测量；它不是本库的功能依赖或核心修复。同一冻结核心的 1280 项扫描和 960 项峰值复测已完成；当前 Windows SDK 的 25 条命令、69 组成本测量通过，Linux 超过 2 GiB 活跃值在 512 MiB 下的批量读取、旧快照、自动维护与独立重开组合验证也通过。后者首次探针字段名编译失败保留，修正后的独立记录位于 `build/large-maintenance-retry-20261108/`，未改变核心和 SDK。向量、全文、RAG/Wiki 本轮不做，后续单独设计；跨进程协作等仍在范围之外。
