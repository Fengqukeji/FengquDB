# Store 目录与操作接口审查

本批为开发源码，未覆盖 release-0.3.2，未发布 SDK 压缩包。完成目录规范、原子条件操作、批量读写、准备对象及三语言接入；整体持续开发目标尚未完成。

## 目录

根目录接口采用每实例 root/name，支持 Unity persistentDataPath。库名限制为 1～32 个便携 ASCII 字符，内部文件平铺，新增代际和临时文件名均为 21 字符。路径绝对化并预留内部名称空间，拒绝超长完整路径；关闭 CREATE 时不创建缺失父目录，无效选项不落盘。

旧 57 字符代际名继续兼容。除了人工格式回归，还用本批前真实 DLL 创建旧库，新 DLL 打开、整理为短名、关闭重开并读回原值。记录位于 `build/store-operations-20261106/legacy-real/`。发布库原 SHA256 保持 `3411f07db33140f7b7fc1a479d36c51b862460295680194462489a39d07ee9c9`。

诊断文件改为固定 16hex writer ID。首次验证发现原日志识别器只识别四段十进制名称，新文件无法查询/清理；已补上新格式精确识别并保留新旧两种历史命名。测试覆盖查询、清理旧名、保留无关文件、短名和超长目录拒绝。

## 一致性与生命周期

原子操作在 writer 内检查最新候选视图，复用 SYNC 合组刷盘，无隐藏 OCC 重试。并发同键计数、TTL、数值溢出、失败条件不生成版本、排队超时、I/O 不确定提交和重启恢复均覆盖。

准备键仅缓存精确不可变 View 的弱引用；检查点、整理及旧快照均重新按视图身份解析。批量读取元数据与值来自同一视图；事务私有结果在后续 patch 后仍保持不变，未命中读取照常登记依赖。

准备批次共享不可变 payload，事务保留拥有者并保守计预算，保存点回滚恢复保留链。修改共享值先隔离完整私有缓冲，再沿用减法裁剪。审查中把批次接入和其他相关事务入口的 ready 检查放在访问事务状态前；已结束事务返回 INVALID_ARGUMENT。

批量输入尾项无效时验证整批而不暂存前缀；内存额度不足时结果/准备批次释放预算；准备数据不能绕过事务额度。资源暂存失败仍要求 rollback/close。

## 验证

| 范围 | 结果 |
| --- | --- |
| Windows Release | 全构建完成；15 项 CTest 均通过。首次 diagnostics 失败后修正并定向复验；后续仅修改的 features 测试也定向复验 |
| C#/Java/Python | 各语言操作、root/name、Unicode 根路径、批次生命周期与共享库互通通过；9 个构建/运行步骤结果见 bindings/report.json |
| Linux ASan/UBSan | 8 项分页/存储/维护/扩展/新增操作通过，无检测报告 |
| Linux TSan | 6 个测试程序通过，无数据竞争报告；保留 WSL 进 main 前的地址布局失败重试记录，不重试检测器已进入后的失败 |
| Linux Release .so | C ABI、C++ API、Store SDK 三项通过；安装到本批隔离 sdk 后独立 CMake 消费者通过 |
| Android ARM64 | Unity 所带 NDK、API24、静态 libc++ 编译及链接通过；122 个公开 C API 导出齐全，仅导出 C ABI，LOAD 对齐为 0x4000 |
| C# iOS 条件分支 | 定义 UNITY_IOS 的 netstandard2.1 构建通过，0 warning；仅证明 __Internal 分支可编译 |

TSan 首次扩展测试曾出现 HISTORY_LOST，带阶段记录重复运行未复现；时间点恢复测试原来依赖真实时钟和 sleep，现改成受控测试时钟明确推进提交边界，并通过 Windows、ASan/UBSan、TSan 定向复验。这里没有据此声称修复了引擎时间点恢复缺陷。

证据目录为 `build/store-operations-20261106/`，含 bindings、asan、tsan、linux-shared、android-arm64、android-plugin、binaries.json。没有 Android/iOS 真机、Unity IL2CPP 发布或 iOS 原生链接证明。

## 调用微基准与体积

同机 Windows Release，热缓存，5 组中位数；每值耗时如下。准备键 GetInto 会复制 256 字节，普通 C++ get 返回借用视图。批量/逐条都读取相同 32 个键。64 KiB 写入为 BUFFERED，最终 sync 在计时外，不代表逐次刷盘吞吐。

| 场景 | 普通接口 | 复用/批量 | 吞吐比 |
| --- | ---: | ---: | ---: |
| C++ 256B 读取 | 151.0 ns | 准备键 GetInto 79.2 ns | 1.91x |
| C++ 32 条读取，每值 | 163.7 ns | 批量 86.8 ns | 1.89x |
| C++ 64KiB 写入 | 78.0 us | 准备批次 43.6 us | 1.79x |
| Python 256B 读取 | 2104 ns | 准备键 1371 ns | 1.54x |
| Python 256B 读取 | 2104 ns | 准备键 GetInto 1233 ns | 1.71x |
| Python 32 条读取，每值 | 2099 ns | 批量 1290 ns | 1.63x |

原始结果为 native-benchmark.jsonl、python-benchmark/report.json；复现工具为 `fqdb_store_operations_bench NEW_DIRECTORY` 和 `tools/benchmark_store_calls.py`。上述数据不证明超越 Redis/SQLite，也不表示 C#/Java 已完成同等性能测量。

Windows 核心 DLL 从本批前 664064 增至 717824 字节（约 +8.1%），这是新增能力成本，不能声称整体包体缩小。Android 开启导出收敛/链接裁剪后，同样 strip 的库从 957728 降至 783528 字节（约 -18.2%），静态包含 libc++，运行时依赖 libc/libm/libdl。部署候选仅放本批 android-plugin 目录；最终移动端发布仍需真机验收与许可打包检查。

## 后续持续目标

仍需推进查询与复合索引/投影、备份保留水位与维护预算、进程内变更订阅、字段语义操作、按需检索扩展；完善业务 Protobuf 定义/代码生成示例；补齐真实磁盘/内存限额、长时间计数/日志轮转压力及三语言性能对照。最终定版后再运行 Docker Linux 2 CPU/4 GiB 下 SQLite/MySQL/Redis/MySQL+Redis 持久性匹配的对比。跨进程服务、复制、权限与完整 SQL 不在当前范围。
