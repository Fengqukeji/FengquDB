# 业务 Protobuf 生成、升级与调用成本（2026-11-07）

已补齐独立业务 schema、C#/Java/Python 官方生成代码的接入程序、C# PlayerSaves 封装与 Unity 源码示例，以及生成、编译、互通和成本测量驱动器。使用方法和字段规则见 [业务 Protobuf 示例](../examples/protobuf/README.md)。生成文件和第三方运行库位于业务构建目录，数据库核心、现有绑定程序集依赖及减法裁剪实现保持不变。

## 生成与互操作证据

正式运行根为 `build/store-protobuf-20261107/run-unity-final/`，report.json 的 ok=true，全部 25 条命令成功，测量期间源码清单未变。

- 官方 protoc 36.2 生成两版 C#/Java/Python DTO 和 descriptor，生成文件 SHA256 清单保留。
- C# Google.Protobuf 3.36.2、Java protobuf-java 4.36.2、Python protobuf 7.36.2；Python 使用独立环境中的 upb 后端，原全局 protobuf 4.25.8 未替换。
- Python v2 写入含嵌套背包、map、repeated、UTF-8、负数 ZigZag 和二进制令牌的记录；C# v1 与 Java v1 分别修改后，C#/Python v2 仍读到新增的 realm 和 packed milestones。
- 显式 player_id=0、display_name=""、coins=0 的 optional 存在性在三语言中保留；required 字段缺失与唯一 ID 冲突返回 CONSTRAINT，未发布无效记录；索引能找到零值、空字符串及更新后的负数金币。
- 在线整理前后业务字节完全一致，关闭重开继续可读。PlayerSaves 的类型化读取和写入也已实际验证。
- 顺序将同一测试目录从 Windows 交给 Linux .so 及官方纯 Python 7.36.2 编解码器，再交回 Windows，字段、约束、整理和重启检查均通过，见该运行目录的 linux-handoff.log、windows-return.log。
- 原生对照工具的 Linux 编译、同载荷三项读取及数据校验通过，见父目录 linux-build.log、linux-native.jsonl。WSL 文件时间戳有小幅偏差警告，构建与执行均成功。

共享目录每次先由当前程序关闭，再由后续程序打开。Linux 功能交接使用纯 Python 实现，不纳入下方 Windows upb 性能表。Unity 组件本身已纳入条件编译：使用桌面 UnityEngine 桩编译并调用 Awake、LoadPlayer、SavePlayer、OnDestroy，验证记录 101 写入与关闭后拒读。这只能证明源码编译和普通生命周期封装，Unity IL2CPP/Android/iOS 工具链和真机仍需单独验收。

## 同载荷成本测量

Windows 同机、同一个 Release DLL，载荷为 4176 B，其中二进制字段为 4096 B。三轮按正反顺序交替执行原生、Python、C#、Java。单条工作负载每轮 1000000 次；每批 32 条读取每轮 62500 批，按记录数归一。各工作负载先预热，启动、生成/编译、32 条样本写入与验证不在计时内。各语言相同工作负载的调用数、单位及校验和一致，共 63 组结果。

环境为 .NET SDK 10.0.402、OpenJDK 11.0.14.1、CPython 3.12.10/upb 7.36.2；Java 使用 -Xms64m/-Xmx256m。原生基线调用公开 C ABI，返回独立字节副本或写入复用缓冲；批量场景同时保留 32 份独立副本后再释放。

吞吐中位数，单位为百万记录/秒（序列化/解析为百万消息/秒）：

| 工作负载 | 原生 | C# | Java | Python |
| --- | ---: | ---: | ---: | ---: |
| 序列化 DTO | — | 1.676 | 2.616 | 2.406 |
| 解析 DTO | — | 1.378 | 1.860 | 1.407 |
| 普通读取独立字节副本 | 9.256 | 2.942 | 2.622 | 0.437 |
| 准备键 + 复用目标缓冲 | 11.374 | 9.146 | 5.288 | 0.641 |
| 读取并解析 DTO | — | 1.125 | 0.998 | 0.302 |
| 每批 32 条独立副本 | 8.132 | 4.737 | 2.465 | 0.646 |

抽样 P99 中位数，单位微秒；批量行仍表示整批调用：

| 工作负载 | 原生 | C# | Java | Python |
| --- | ---: | ---: | ---: | ---: |
| 普通复制读取 | 0.2 | 0.8 | 0.7 | 3.7 |
| 准备键 + 复用缓冲 | 0.2 | 0.2 | 0.3 | 2.6 |
| 每批 32 条 | 4.1 | 13.1 | 24.0 | 98.5 |

对这个固定热缓存场景，复用方式相对普通复制读取提升为 C#3.109x、Java2.017x、Python1.468x；批量相对普通单条读取为 C#1.610x、Java0.940x、Python1.479x、原生0.879x，并非所有路径都因批量而更快。

C# 准备键读取约达到同语义原生对照的 80%；Java/Python 仍存在较大差距。差距包含语言运行时、数组处理、锁、参数封送及分配，不可把全部差异归因于数据库引擎。DTO 解析也明显影响读取后得到业务对象的实际成本。当前数据不能推出冷数据、变更频繁时、并发、SYNC 写入或其他数据库的性能结论。

后续优化重点为按需固定 Python 输出缓冲及参数对象、Java 可选 direct buffer 路径，同时保留原接口行为，并验证关闭/生命周期、容量不足和并发边界。普通 Get 的资源与所有权契约不应仅为基准更快而改变。

## 包体与剩余范围

本批只增加示例、构建驱动和不安装的基准工具。隔离 SDK 安装到 `build/store-protobuf-20261107/sdk-unity-check`，确认包含业务 schema、三语言源码和 Unity 组件，不含示例 bin/obj、Python 缓存和 Java jar。开发核心 DLL SHA256 仍为 `1ec3cbd02a709e983b4cc00a55a280a27c8e6a8f1b7103ba2616c201b245e44e`，原发布 DLL 仍为 `3411f07db33140f7b7fc1a479d36c51b862460295680194462489a39d07ee9c9`；业务官方运行库不进入数据库核心，不替换已发布 SDK。

业务 Protobuf 定义/生成/升级路径已经有实际证据；跨语言成本优化仍需继续，长期数值边界、必要能力补强、最终优化审查及 Docker Linux 2 核/4 GiB 的 SQLite/MySQL/Redis/MySQL+Redis 持久性匹配与吞吐比较也未完成。
