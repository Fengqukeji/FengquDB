# Docker Linux 2 核 / 4 GiB 比较驱动

这是当前 Store 开发版的专用基准，不复用旧 Database 的 SQLite 对比结果。工具、MySQL、Redis 都只用于验证，不成为数据库核心依赖，也不安装进 SDK。

每个容器包含 C++ 调用程序与该方案需要的全部服务，共用两个物理核心和 4 GiB，禁止额外 swap。当前 Windows Docker 主机的 CPU 0、2 分属 Ryzen 9 9950X3D 的两个物理核心；其他机器须核对拓扑并调整 cpuset。数据库数据写入新的 Linux 命名卷，结果写入显式选择的输出目录；不打开生产库，不发布端口。组合方案也共享同一额度。

固定版本为 Ubuntu 26.04 镜像摘要、MySQL 8.4.11、官方 SQLite 3.54.1 amalgamation、官方 Redis 8.10.3 键值内核与 jemalloc。Redis 不编译可选搜索/JSON/时间序列模块，不启用 TLS；客户端在同一容器走 TCP loopback。MySQL 用发行版库的原生 prepared statement；SQLite 为预编译语句、WITHOUT ROWID、WAL、NOMUTEX 独立连接及 mmap。完整包版本、源码哈希、镜像 ID 与实际加载库另存到结果目录。

## 持久性与操作

| 模式 | Store | SQLite | MySQL | Redis | MySQL+Redis |
| --- | --- | --- | --- | --- | --- |
| sync | SYNC | WAL + FULL | InnoDB flush_log_at_trx_commit=1，doublewrite 开启，binlog 关闭 | AOF + appendfsync=always | 同 MySQL；Redis 为不持久化的派生缓存 |
| buffered | BUFFERED | WAL + NORMAL | InnoDB flush_log_at_trx_commit=2 | AOF + appendfsync=no | 同 MySQL，缓存同上 |

sync 单条调用等待其可靠提交语义；并发时允许引擎合组。buffered 不承诺逐条可靠同步，计时结束后显式同步再校验；该最终同步不计入 buffered 吞吐。操作系统/设备仍须兑现 fsync，杀进程不等于真实掉电。

- `get_copy`：独立字节副本。Store 返回 C-owned buffer；SQLite 拷贝 column blob；MySQL 绑定新目标字符串；Redis 复用其已拥有网络字节的 reply。
- `get_borrow`：在调用范围内消费可用视图或复用缓冲；不与普通复制读取混成一项。
- `get_miss`：固定缺失键域，组合缓存支持负缓存。
- `get_many`：一次 32 条。Store/SQLite/MySQL 保留 32 个独立副本后消费，Redis 使用 MGET；P99 表示整批调用，吞吐按记录数归一。
- `insert`、`delete`：固定调用数，各次一条。新增使用全新键；删除使用预填充键。
- `update`：覆盖已有记录；各线程拥有不相交的写键分区。
- `patch8`：仅修改已知 schema 的 fixed64 revision 字节。Store transaction patch、SQLite substr/拼接、MySQL INSERT 二进制函数、Redis SETRANGE，结果字节相同。
- `put_many`：一笔原子操作 32 条。Store 单事务 put_many；SQLite/MySQL 单条多行 UPSERT 的隐式事务；Redis 使用一次 MSET，键与值参数在栈上组装。
- `mixed95`：95% 点读、5% 覆盖写，读取可访问其他线程负责的键。

值是合法 Protobuf，schema 为 value.proto。默认 256 B，也可选择其他总长度；首尾消费在计时内，完整字节与最终键集由独立模型在计时外验证。每个用例还在新进程中复核；服务重启校验的范围由结果里的 server_restart 字段明确标出。

组合方案采用 MySQL 权威数据、Redis 正/负缓存和写后失效。一个基准进程内按键分片的读写锁协调缓存与写入，批量按固定顺序锁定分片。它不是通用跨进程缓存一致性协议，也不是 MySQL/Redis 分布式事务。服务重启会丢弃派生缓存。

Store 缓存额度 2 GiB、delta 16 MiB、writer 32 MiB、事务 64/256 MiB；固定对象计费额度 2 GiB 用于高并发下共享根的保守重复计费，不代表预分配内存。启用自动维护，记录合组、文件、缓存及后台整理计数。全部实际内存仍受 4 GiB cgroup 限制。

## 构建与运行

```text
docker build -f tools/final_benchmark/Dockerfile.base -t fqdb-final-bench-base:20261107 .
docker build -f tools/final_benchmark/Dockerfile -t fqdb-final-bench:20261107 .
python scripts/run-final-comparison.py --suite smoke --group --output NEW_OUTPUT
python scripts/run-final-comparison.py --suite matrix --group --output NEW_OUTPUT
```

Dockerfile 各自的 dockerignore 只发送需要的文件，不把整个构建目录、数据或发布包打入镜像。官方参考源位于 artifacts/reference，Redis 和 SQLite 的固定哈希在构建时验证。核心仍按原仓库构建，未加入 Google Protobuf 或服务端依赖。

先用 smoke 的两种持久性模式检查五种方案的 CRUD、空缺、局部修改、32 条批量和重启；短样本吞吐不作最终结论。再对 Redis 的 1/2 个 I/O 线程进行对照，使用适合当前 2 核总预算的配置。正式扫描应覆盖并发 1～128，较大记录集、256 B/4 KiB 载荷及不同写入持久性；对候选峰值增加多轮测量，并保留回退和尾延迟。

group 仅复用 MySQL 服务启动成本；每个用例先清理专属客户端夹具、重建表和 Redis 数据，重新填充及完整预热。每次操作校验、最终模型、进程重开和资源限制都保留。默认 grouped matrix 在最后一个用例重启服务；smoke 每个用例都重启。命名卷和容器带本次所有权标记，通过后只删除对应资源，失败则停止并保留诊断数据。

延迟按每 64 次调用抽样，报告抽样 P50/P95/P99；小样本的 P99 不可当作生产 SLA。cgroup CPU 包括调用程序与服务，内存同时记录计时区间采样峰值和容器生命周期累计峰值。结果只适用于记录的硬件、版本、配置、API 与负载，不是数据库普遍或理论吞吐上限。
