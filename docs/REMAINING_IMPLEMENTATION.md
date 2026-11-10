# Store 集中批次验收记录

本文是早期集中开发批次的历史验收记录；下文的“最终”、库路径和发布状态只代表该阶段，不能用于判断当前源码产物。当前接入从 [文档入口](README.md) 开始，库按 [构建说明](../README.md#构建) 重新构建。下面的旧 SDK 和验收记录保留供追溯。

本批集中实现此前剩余的软件能力。公共接口及限制见 [Store 扩展能力](STORE_FEATURES.md)。实现保留二进制完整缓冲的减法裁剪、按需页读取和有界缓存，没有引入 SQL 服务、模型运行库或 Google Protobuf 运行时。

- [x] 自动 TTL/整理调度、索引后台续建、取消及线程退出。
- [x] 顶层标量字段索引、唯一/必填约束、在线分批构建和持久化续扫。
- [x] Hash、Set、List、SortedSet 的独立元素操作和事务/保存点接入。
- [x] SYNC 合组刷盘及发布隔离；BUFFERED 复用直接追加路径。
- [x] 增量备份身份/版本链、版本及时间点恢复、新目录原子发布。
- [x] 旧 Database 的隔离副本读取、有界目标事务和源文件保护。
- [x] C/C++、C#、Java、Python 及维护 CLI 接入。
- [x] 最终跨平台验证收尾，结果及证据见下文。

最终验证结果：

- Windows 阶段全套 [14/14 项 CTest](../artifacts/complete-windows-tests.log) 通过；收尾修改后，Store/维护/扩展功能 [3/3 项](../artifacts/complete-store-watchdog-tests.log) 和日志/诊断/扩展功能 [3/3 项](../artifacts/complete-logging-final-tests.log) 定向复验通过。这里不将阶段全套结果表述为最终源码的重新全量执行。
- Linux 最终 ASan/UBSan（含泄漏检查）[7/7 项](../build/paged-storage-sanitizers/Testing/Temporary/LastTest.log) 通过：分页基础、存储、Store、共享 SDK、维护、TTL、扩展功能。
- Linux 最终 TSan 的 [I/O/缓存](../build/store-tsan/paged_io.log)、[Store](../build/store-tsan/store.log)、[维护](../build/store-tsan/store_maintenance.log)、[TTL](../build/store-tsan/store_expiry.log)、[扩展功能](../build/store-tsan/store_features.log) 共 5 项通过。仅对已知的测试进入前地址布局失败重试；进程崩溃子测试在 TSan 下跳过，由 Windows 和 ASan 覆盖。
- Linux Release 共享库的 [C ABI、C++ API、Store SDK 共 3/3 项](../build/store-linux-shared/Testing/Temporary/LastTest.log) 通过；安装后另建 C/C++ 消费者也通过。安装后的 CLI 在原生 `/tmp` 完成维护及扩展两组测试，覆盖中文路径、索引、增量链、版本恢复、历史/目标保护及旧库源文件字节保持。
- C#、Java、Python 的构建、独立功能与跨语言共库 [9/9 条命令](../build/store-bindings-all-features/report.json) 通过；共库流程包含 Hash、字段索引及 Java 整理后继续读写。Windows 扩展 CLI 同样通过。

测试覆盖随机集合模型、分页、事务保存点、唯一值交换、索引约束与持久化续建、系统命名空间碰撞、业务范围隔离、SYNC 发布前读隔离、注入同步失败、并发追加、后台停止和错误定义轮转。进程崩溃测试覆盖扩展启用、索引进度、增量包及迁移发布；过期索引清理还验证了不读取 8 MiB 的旧业务载荷。

消毒器首轮识别出旧 TTL 测试对短墙钟窗口的假定：在快照创建前已到期时，测试仍解引用空 optional。现已改用仅测试构建存在的确定性时钟，并显式检查空结果；实际事务期限仍使用单调时钟。初始诊断保留，没有当作测试进入前布局失败而重试。高并发验证期间的部分 Windows 测试曾触发 120 秒进程看门狗；相关上限改为 600 秒后定向复验通过，这不改变引擎的事务期限。

常规 API 的数值日志已补齐索引、增量备份、恢复链、迁移与配置操作。后台线程通过运行状态和计数器暴露错误，不调用用户日志回调；接入程序应检查 `runtime_info`。

开发版 Windows 库位于 `build/Release/fengqudb.dll`；Linux 安装验证库位于 `build/store-linux-shared/sdk/lib/libfengqudb.so`。已发布 0.3.2 DLL 的 SHA-256 仍为 `3411f07db33140f7b7fc1a479d36c51b862460295680194462489a39d07ee9c9`。

真实断电、长期生产负载、硬件故障及新能力的同条件性能基准尚未执行，不能把模拟故障或小规模并发测试当作生产验收。已发布的 0.3.2 SDK 不替换，也没有在本批打包新版本。
