# 分段 SDK 三语言接入审查（隔离候选）

证据目录为 `build/segment-sdk-lab-20261107/`。本批沿用已通过数据库级验证的分段核心，54 个原生核心、公共头和 CMake 文件字节保持一致；增加 Python、C#、Java 的创建选项、测试清理器和互通验证。**没有合入主工程，没有覆盖旧发布包，没有正式性能结论。**

## 接口与兼容修复

隔离 SDK 提供 Python `segmented=True`、C# `StoreOptions.Segmented = true`、Java `Store.Options.segmented = true`。默认均关闭，打开已有容器仍由核心自动识别。创建标志使用原 C 结构中的 flags，不改变结构大小或版本；新选项目前只能配套隔离核心，主工程旧 SDK 未公开此能力。

JNI 保持现有方法签名，接受原来的 7 项 limits 和显式分段时的 8 项 limits；新增项只能为 0/1。旧 Java 类不需要重新编译即可配新 JNI。最初新版 Java 类无论选项都传 8 项，实际复现其默认模式配旧 JNI 返回状态 3，见 `compat-before.log`。修复为默认模式保持 7 项、仅显式分段使用第 8 项，两处打开路径共用参数构造函数。

修复后 Windows 验证了新 Java 类配旧 JNI 的普通读写、新类配新 JNI 的普通/分段读写，以及向旧 JNI 请求新选项时返回错误且不创建目录。Linux 复验了修正后类配新 JNI 的两种模式、完整 smoke 和大值流程。旧类配新 JNI 的共库接力另在两个平台均实际通过；没有把 Windows 的反向兼容结果称作旧 Linux JNI 已重新运行。

## 测试目录清理

三个语言原有 smoke 固定要求数据库只有 CURRENT、LOCK 和一个代际文件。现在共用测试专用 Python 清理器，C#/Java 用独立参数启动它；这个依赖仅用于测试，不进入 SDK 运行接口。

清理器要求路径位于本轮独占输出根中，拒绝重解析路径、非普通文件和未知条目；验证 CURRENT 的 CRC、UUID、代际、规范名称，再核对数据库头及每个段的身份镜像、CRC、段号和容量。全部校验完成后才删除文件，不放宽为匹配后缀便删除。大值 Python 用例实际插入一个外来文件，验证清理被拒绝且原有文件未删除，再移除这个测试文件继续清理。

准备脚本首次遇到已有测试目录及 Windows 默认 GBK 解码问题，已改为核对现有拷贝后增量复制和显式 UTF-8。它们发生在构建前的隔离准备阶段，不是数据库运行失败。第一轮互通预填夹具使用 40 MiB 单值，被既有 16 MiB 上限以 INVALID_ARGUMENT 拒绝；改为多次 8 MiB 写入，保留 `windows-test-report.json` 和原始日志，没有提高生产常量来绕过检查。

## 实际验证

| 阶段 | Windows | Linux |
| --- | --- | --- |
| 分段常规 smoke | Python、C#、Java 均通过 | Python、C#、Java 均通过 |
| 跨段共库接力 | 新/旧 Java 两条 Python→Java→C#→Python 链均通过 | 同样两条链均通过，Java 启用 `-Xcheck:jni` |
| 大值、旧快照与备份链 | 三语言均通过 | 三语言均通过 |
| 修正后的 Java 参数契约 | 新旧原生组合 4 种情况及 smoke/大值通过 | 新 JNI 的默认/分段、smoke/大值通过 |
| 固定输出缓冲生命周期 | Python CPython/回退路径、Java direct buffer 在分段模式通过；旧布局也有保留记录 | 相同三组分段模式通过 |

主要运行阶段各有 16 条成功命令：三个常规 smoke、两条含预填与四次语言接力的流程、三个大值程序。Windows 的三个常规成功项来自首次报告，其后 13 条来自 `windows-test-report-02.json`，只复用同一二进制上明确成功的行；首次报告整体失败仍保留。Linux 为 `linux-test-report.json`；兼容修复和缓冲复验分别为 `java-compat-report.json`、`windows-buffer-segmented-report.json`、`linux-compatible-report.json`，不与主阶段重复合计为性能样本。

使用非测试构建的真实 **64 MiB 段容量**。共库预填后核对至少两个数据段、最长名称 30 字符，再执行 Protobuf 字节、字段索引、原子计数和批量值的跨语言接力。旧 Java 类在新 JNI 下实际读写并整理这个多段容器。

每种语言的大值程序反复写入 **8 MiB 值**，实际跨到两个数据段；4 MiB 缓存小于该值，验证冷读取、旧快照跨整理仍可读、复合索引/投影及原子操作、持久历史保留、全备加增量链恢复、释放历史后的旧代际清理和重开后的完整字节一致。这里的有限模型不证明长期文件数量或 RSS 有界。

Linux 的服务进程和测试受限于两核、4 GiB、无额外 swap，数据在 Linux 命名卷；代码与已编译的 portable C# 程序从 D 盘只读挂载，C# 实际由 Linux .NET 10.0.14 host 执行，Java 为 Linux OpenJDK 11.0.33。该组 .NET 使用 invariant globalization；UTF-8 路径与字节互通已验证，不代表应用的全部本地化文化行为已验收。日志未出现 JNI 检查警告；本轮自有容器和卷核对所有权后清理，证据留在 D 盘。

## 源码、包体与边界

最终 137 文件由 `source-manifest.json` 固定；兼容修复前的清单、源码压缩包和审计分别保留为 `v1-*` 及 `validated-sdk-source.zip`。最终 `validated-sdk-compatible-source.zip` 完整性检查通过，SHA256 为 `c0dfefa56b76e685bbfb932c748deb7690400967cbc9c9a12951680fa36927db`。最终 Java 类及 portable C# 程序的哈希、复用的原生二进制和资源配置见 `final-audit.json`。代码和测试没有追加每次读写的语言侧步骤，新选项只参与打开，但其调用成本仍须实测，不能从源码推断零回退。

| 二进制 | 实际字节数 |
| --- | ---: |
| 当前主开发 Windows 核心，保持不变 | 790016 |
| 隔离 Windows 核心 | 863232 |
| 隔离 Windows JNI | 189440 |
| 隔离 Linux 核心，未作发布 strip | 994656 |
| 隔离 Linux JNI，未作发布 strip | 101624 |

Windows 核心相对当前主基线增加 73216 字节，约 9.3%，包含 Root5 回收与分段门面的累计变化，不能全部归因于语言选项。Linux 未裁剪产物不与已裁剪或不同编译配置的历史包混算。正式包体优化、导出与依赖复核仍未完成。

主工程 114 文件基线和两个既有 DLL 哈希保持不变。本批结束时，语言实际调用成本、原生吞吐和包体仍待实测；随后完成的 72 项原生、126 组语言测量及去符号副本验证见 [调用成本与包体审查](STORE_SEGMENT_COST_REVIEW_20261107.md)，其中写入和 C# 复用仍有回退，候选未采用。

当前格式的磁盘/内存故障和长期资源验证、写入回退定位及最终五方案比较仍待完成。Unity IL2CPP、Android/iOS 真机、所有字段约束/集合/TTL/订阅的跨段组合也不能由本批测试直接推定，整体目标继续按 [长期验收记录](GOAL_ACCEPTANCE_20261107.md) 推进。
