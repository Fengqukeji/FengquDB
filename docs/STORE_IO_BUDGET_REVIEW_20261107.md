# 维护 I/O 预算审查与验收（2026-11-07）

开发版 Store 的按次维护读写额度、I/O 统计及后台整轮共享额度已接通 C/C++、C#、Java、Python。参数、示例和限制见 [维护 I/O 字节预算](STORE_IO_BUDGET.md)。本批保留完整私有二进制缓冲的覆盖与减法裁剪，既有发布 SDK 未替换。

## 审查边界

- 额度通过线程局部作用域按需启用，嵌套维护继承外层额度，不重新领取额度；作用域结束恢复原状态并回填统计。
- WAL、代际头、封存与双根指针、复制记录及 CURRENT 清单在开始写入前预留完整写额度。WAL 预检拒绝返回 LIMIT，文件长度、已发布版本不变，writer 可继续使用。
- 复制期间读取额度可能耗尽，未发布候选清理；CURRENT 额度差一字节时原清单完全不变，旧快照及后续提交可继续使用。索引已提交批次保留，可续建。
- 受计量维护使用当前线程的 writer gate 直接提交，避免其他线程的组写入借用其额度；SYNC 仍在同步成功后发布。真实同步故障保留 COMMIT_UNKNOWN、poison 和重开恢复语义。
- 恢复、恢复链与迁移覆盖源库打开/恢复、私有复制和旧格式恢复器的请求量；旧库源字节保持不变。元数据操作和设备物理流量不属于此字节额度。
- 维护 ABI 1 仅拷贝原 32 字节前缀，运行配置 ABI 1 仅拷贝原 56 字节前缀；ABI 2 检查完整结构大小。旧 Java 私有 JNI 入口继续保留，新维护包使用独立入口，避免参数错位。

本次收尾审查没有发现需要进一步修改引擎的持久化或作用域缺陷。初版开发时的测试状态辅助函数曾错误转换预期 COMMIT_UNKNOWN，测试已修正；该问题不属于生产引擎缺陷。本批最终验证使用冻结后的源码。

## 验证结果

证据根为 `build/store-io-budget-20261107/`：

| 范围 | 结果与证据 |
| --- | --- |
| Windows 全部目标、CTest | 19/19 通过，`windows-build-final.log`、`windows-ctest-final.log` |
| Linux ASan/UBSan、泄漏检查 | 12/12 通过，`asan-final.log`；复用 `build/store-layout-20261107/asan`，源码 SHA256 清单检查通过 |
| Linux TSan | 10 个程序通过，`tsan-final.log` 及 `tsan/*.log`；仅重试测试入口前的已知地址布局错误，原日志保留，源码清单检查通过 |
| Linux Release .so 与独立安装 SDK | C ABI、C++ API、Store SDK 三项和独立消费者通过，`linux-shared-final.log` |
| 三语言构建、独立运行及互通 | Python/C#/Java 9 步通过，当前源码复验见 `build/store-layout-20261107/bindings/report.json`，本批初版记录为 `bindings/report.json` |
| Linux Python .so 接入 | 原生 `/tmp` 流程通过，`linux-python.log` |
| 已编译旧 Java 类加载新 JNI | 通过，`old-java-final.log`；javap 确认旧类使用五参数 nFeatureAdmin |
| Android ARM64 | 编译通过，`build/store-layout-20261107/android-build.log`；裁剪后仍为 133 个 C API，`android-plugin.json`、`android-symbols.txt` |

定向用例覆盖旧 ABI 小结构、超额首字节拒绝、精确额度、晚期清单拒绝、候选清理、索引多次续建、后台限额及解除后继续工作、业务线程隔离、SYNC 发布前不可见和真实同步失败恢复。进程崩溃用例由 Windows/ASan 覆盖，TSan 跳过相关子进程。

## 默认路径性能

本批前 DLL 为 783872 字节、SHA256 `6029903e7f89d2366e68703d91f6d97fe2751af715824c303817a0bfe58e62d8`。新旧目录放置字节完全一致的 `fqdb_store_bench.exe`，各执行五轮并交替顺序；随后针对短耗时负载的波动，再固定到 CPU 0 执行五轮。每轮关闭后重开检查结果。此比较也包含上一轮目录组件校验补强。

下表为新/旧吞吐中位数，超过 1 表示新构建更高：

| 工作负载 | 默认 CPU 调度 | 固定 CPU 0 |
| --- | ---: | ---: |
| raw get hit | 0.9935 | 0.9964 |
| raw get miss | 0.9927 | 1.0068 |
| hash get hit | 1.0040 | 1.0119 |
| hash get miss | 1.0061 | 1.0154 |
| hash get missing scope | 0.9721 | 0.9954 |
| hash insert buffered | 0.9860 | 0.9805 |
| set add buffered | 1.0002 | 0.9716 |
| hash overwrite 256 KiB buffered | 1.0089 | 1.0454 |
| hash get command 256 KiB | 0.9714 | 1.0117 |
| hash scan 256 rows | 1.0015 | 1.0015 |
| hash scan 1 MiB | 1.0017 | 1.1096 |
| KV insert buffered | 1.0015 | 0.9948 |

多数读路径接近原水平；大值命令与扫描结果受调度和缓存影响，不能视作稳定增益。缓冲新增仍有约 1%～3% 的局部回退，部分 P99 也增加：例如固定 CPU 的 Set 新增 P99 为 16.1→20.9 微秒。该基准单项新增仅 1000 次，后续稳态性能定位应继续关注，不能以单轮吞吐或微秒级 P99 证明所有负载零损耗。

完整数据分别为 `performance/comparison.json` 和 `performance-affinity/comparison.json`，CPU 掩码为 `affinity.json`。基准使用热缓存和 BUFFERED 写入，最终 Sync 不在新增计时内；它不是持久性匹配的跨数据库比较，也不测启用维护统计自身的成本。

## 二进制体积与发布边界

| 构建 | 本批前 | 当前 | 增量 |
| --- | ---: | ---: | ---: |
| Windows Release DLL | 783872 B | 790016 B | 6144 B，约 0.78% |
| Android ARM64 裁剪 .so | 854072 B | 859112 B | 5040 B，约 0.59% |

Linux 裁剪共享库为 749088 B。实际文件大小和 SHA256 见 `artifact-hashes-final.json`。现有发布 DLL 的 SHA256 仍为 `3411f07db33140f7b7fc1a479d36c51b862460295680194462489a39d07ee9c9`。没有打包或替换发布 SDK。

字节预算不是实际磁盘满或物理内存限制验证。Docker 环境已确认可用，下一批将补充实际受限文件系统、进程内存压力与外部杀进程后的恢复证据；长期轮转、业务 Protobuf 生成示例、各语言稳态性能和最终 2 核/4 GiB Docker 跨数据库比较仍需继续完成。iOS 与 Unity IL2CPP 真机验收也不在本批已通过范围内。
