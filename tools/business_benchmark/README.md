# 同业务、多数据库基准

这是 YCSB 思路衍生的业务比较工具，不是官方 YCSB/TPC-C 合规成绩。核心数据库不增加依赖；yyjson、SQLite、MySQL 和 hiredis 仅用于此独立驱动。

所有方案处理相同十字段 Profile：用户编号、修订号、余额、等级、状态、UTF-8 名称、邮件、简介、区域和登录时间。简介为 128/4096 字节，实际编码大小另报。完整读取返回同样的拥有字符串的 C++ 业务对象，包含解码、数值转换和字段复制；更新允许用各引擎更合适的字段操作，不强迫 SQL/Redis 重写 Protobuf。

| 方案 | 常用建模和操作 |
| --- | --- |
| FengquDB | Protobuf wire、共享只读 get、增量 patch、OCC 事务 |
| SQLite rows | INTEGER PRIMARY KEY、普通类型列、预编译 SQL、WAL |
| SQLite JSON | TEXT 文档、yyjson 解析、json_set/json_extract |
| SQLite JSONB | SQLite 原生 JSONB、jsonb_set/json_extract；完整 DTO 通过 json() 返回文本解析 |
| SQLite Protobuf | BLOB 控制组，借用列字节解析、sqlite3_blob_write 修改固定字段 |
| MySQL | InnoDB、BIGINT 主键、类型列、预编译语句与显式事务 |
| Redis | Hash、HMGET/HSET/HINCRBY、批量流水线、Lua 原子转账 |
| MySQL+Redis | MySQL 权威表、Redis JSON DTO 缓存、300 秒 TTL、提交后失效；批量 MGET 和 SQL IN |

组合缓存使用基准进程内分片读写锁解决回填与更新的竞态，不是跨进程缓存一致性协议。它与数据库和客户端共用资源额度，缓存命中率实测记录。

| 业务用例 | 操作与实际意义 |
| --- | --- |
| C | 100% 读取完整用户档案，参考 YCSB C |
| B | 约 95% 读取、5% 修改 revision，参考 YCSB B |
| A | 约 50% 读取、50% 修改 revision，参考 YCSB A |
| F | 约 50% 读取、50% 原子 credit++，参考 YCSB F 的读改写思想；允许原生原子表达式，不是官方 F 操作序列 |
| projection | 只需 revision 和 credit，允许 SQL 列投影、Redis HMGET、本库延迟字段投影 |
| batch32 | 一次取 32 个用户档案，SQL IN、Redis pipeline/MGET、本库 get_many；不是 32 条原子写入 |
| save | 从业务 DTO 完整编码并覆盖保存，修改修订戳及简介末字节；SQL 更新全部常规列、Redis HSET 完整 Hash |
| transfer | 两账户原子转账 1 单位；SQL 事务、Redis Lua、本库事务，验证每户余额及总额 |

A/B 的写键分区避免非业务必需的同键写竞争，读可访问其他线程写过的键；F/transfer 在共享的 128 个热点账户上竞争。随机操作选择和 Zipf(0.99) 键轨迹预生成，各数据库使用相同种子与轨迹。计时内只校验返回编号并消费 DTO；计时后按独立操作增量核对每个字段，随后由新进程再次读取核对。报告保留实际读写比例、重试、命中率和延迟样本数。

每方案的客户端及所有服务共同受限于 2 个指定 vCPU（cpuset 0,2）、4 GiB、无额外 swap；数据写入 Linux 命名卷。服务走同容器 loopback TCP，内嵌数据库直接调用，因此端到端差异包括调用架构，不能全部归因为 Protobuf。工作集为热数据，本轮不测超内存或冷启动。

SYNC：本库 SYNC、SQLite WAL/FULL、MySQL flush_log_at_trx_commit=1、Redis AOF/always。BUFFERED：本库 BUFFERED、SQLite WAL/NORMAL、MySQL flush=2、Redis AOF/everysec；它们并非相同的丢失窗口。组合缓存不持久化，持久性取决于 MySQL。装载不计时；BUFFERED 计时结束后额外同步，再校验和重开；该最终同步不计入吞吐。杀服务进程不是硬件掉电。

先运行小夹具确认所有适配器正确，再固定二进制运行多轮；不边编译边测性能。单线程反映同步调用延迟，8 并发反映有限资源内的吞吐，不代表各产品全局峰值。延迟为抽样 P50/P95/P99，每 64 调用取一次，SYNC 每 8 次，单线程最多 100000 个样本；不是生产 SLA 或开环排队测试。

```text
python scripts/run-business-comparison.py --phase build --output NEW_BUILD_DIR
python scripts/run-business-comparison.py --phase pilot --output NEW_PILOT_DIR
python scripts/run-business-comparison.py --phase codec --output NEW_CODEC_DIR
python scripts/run-business-comparison.py --phase matrix --representative --threads 1,8 --runs 3 --records 4096 --seconds 1 --output NEW_RESULT_DIR
python scripts/run-business-comparison.py --phase matrix --representative --threads 1,8 --runs 3 --records 4096 --seconds 1 --workloads save --output NEW_SAVE_DIR
```

每次输出目录必须全新。所有样本、失败日志、配置、版本及校验摘要保留。只有带本次所有权标签、确认停止且成功的容器和命名卷才自动移除。正式代表矩阵：4 KiB 简介用 1/8 并发，小简介用 8 并发；只读用例无需重复测 SYNC。

参考来源：

- [YCSB core workloads](https://github.com/brianfrankcooper/YCSB/wiki/Core-Workloads)
- [SQLite JSON/JSONB](https://www.sqlite.org/json1.html)、[WAL](https://www.sqlite.org/wal.html)
- [MySQL InnoDB 参数](https://dev.mysql.com/doc/refman/8.4/en/innodb-parameters.html)
- [Redis Hash](https://redis.io/docs/latest/develop/data-types/hashes/)、[流水线](https://redis.io/docs/latest/develop/using-commands/pipelining/)、[基准注意事项](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/)
- [yyjson 0.12.0](https://github.com/ibireme/yyjson/releases/tag/0.12.0)，来源、许可证和 SHA256 在 artifacts/reference/yyjson-0.12.0/origin.json

整份保存是单独补充的业务负载，业务 DTO 在计时前建立，计时包含完整编码、驱动和提交；不包含生成合成数据的循环。修订值是 64 位变更戳，每轮重装载零修订数据，包含首次更新时格式记录长度的增长。初始化样本字节数不是所有编号或修订后的固定长度。
