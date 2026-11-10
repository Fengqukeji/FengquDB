# 当前文档入口

本目录区分当前接口契约和历史实现/测量证据。当前源码为 Store 0.3.2 / ABI 2，默认**性能模式 50 ms / 1 MiB**。已在内存确认但未同步的后缀可能在崩溃时丢失，关键写使用 force_reliable 或可靠模式。

| 阅读目的 | 当前文档 |
| --- | --- |
| 了解产品范围与当前能力 | [产品规格](PRODUCT_SPEC.md)、[能力与用途](CAPABILITIES.md) |
| 接入、所有权、错误与线程契约 | [Store API](STORE_API.md)、[目录布局](STORE_LAYOUT.md) |
| 写成功、刷盘、故障及在线配置 | [写入策略](STORE_WRITE_POLICY.md) |
| CRUD、批量、原子字段修改 | [常用操作](STORE_OPERATIONS.md)、[字段命令](STORE_FIELD_COMMANDS.md) |
| 字段投影与显式索引 | [索引和投影](STORE_QUERY.md) |
| 条件、排序、聚合、关联与计划 | [复杂查询](STORE_SELECT.md) |
| 共享读取与语言缓冲复用 | [共享读取](STORE_SHARED_READ.md)、[绑定缓冲](STORE_BINDING_BUFFERS.md) |
| 集合、TTL、索引约束与维护 | [扩展能力](STORE_FEATURES.md)、[维护 I/O 预算](STORE_IO_BUDGET.md) |
| 完整/增量备份、PITR 与消费变更 | [历史保留](STORE_HISTORY.md)、[变更订阅](STORE_WATCH.md) |
| 编码和业务 Protobuf | [wire 说明](PROTOBUF.md)、[业务模型示例](../examples/protobuf/README.md) |
| 构建、并行开发、消毒器与产物 | [开发规范](DEVELOPMENT.md) |
| 查证某次测试或实验 | [历史证据索引](history/README.md) |

带日期的报告描述相应冻结版本，报告中的“当前”、产物路径和吞吐不自动适用于后续源码。`STORAGE.md`、`ITERATION_0_2.md`、旧 `OPTIMIZATIONS_0_3*` 描述已移除引擎，保留供追溯；它们不定义当前 Store 的 ABI 或默认持久性。

项目索引的 stale/unverified 是文件用途或指纹尚未复核的状态，不单独证明正文语义错误。需要同时核对源码、当前契约和测量所绑定的源码清单。
