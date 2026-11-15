# 业务性能报告

直接打开 [index.html](index.html)，或把此目录发布到 GitHub Pages。页面没有 CDN、字体下载、框架或后台服务依赖。数据通过同目录 `data.js` 加载，支持直接打开本地 HTML。

页面默认展示单调用线程，常用 CRUD 优先；每项图下列出 FengquDB 与四种方案的中位数比值。GitHub 首页使用 [静态图](../assets/benchmarks/)，由 `python scripts/prepare-github-home.py` 从同一份 `results.json` 导出；该生产方脚本使用 Matplotlib，网页与 SDK 不依赖它。`comparisons.json` 记录全部 44 组和 176 个逐库倍数。

## 测了什么

五种实现完成同一个十字段档案业务：按键读取、整存、新增、删除、小字段修改、两种读写混合、一次读 32 份、字段投影、热点计数和双键转账。

客户端为同一个 C++ 业务驱动，FengquDB 通过 C ABI 调用。此处不是 C#、Java 或 Python 绑定吞吐的测量。

- FengquDB：共享 C ABI 读取、有限 Protobuf wire 编码、原子字段操作；修订号和余额使用固定宽度布局，按已知字节位置更新。性能模式 50 ms / 1 MiB。
- MySQL：InnoDB 类型列、预编译 SQL、本机 TCP、flush=2、binlog 关闭。
- Redis：Hash、HMGET、HINCRBY、Lua 双键事务、本机 TCP；AOF everysec。
- MySQL+Redis：同 MySQL 建模，JSON cache-aside、读后填充、更新后失效；基准进程内按键协调缓存一致性。不是分布式缓存一致性方案。
- SQLite：普通类型列表、预编译 SQL、WAL、NORMAL，按 SQLite 写锁机制重试。

每场景运行 3 轮，交替产品顺序；客户端与相应服务合计限制 2 逻辑 CPU、4 GiB、禁用 swap。按虚拟机报告的拓扑，CPU 0/2 属于不同核心；未独占宿主物理核。主机为 Ryzen 9 9950X3D、约 94 GiB RAM、Predator GM9000 4 TB NVMe SSD；测试在 Docker / WSL2 Linux 中运行。共享主机未做独占裸机隔离。

每份业务对象的简介为 128 B 或 4 KiB，实际对象包括其他字段。热场景 4,096 条、Zipf 0.99、预热后 1.5 秒计时；新增/删除分别处理 65,536 个不同键，只访问一次。写线程修改互不重叠的 ID；热点计数/转账主动争用 128 个键。整存更改修订号和简介中的一个字符，依然向数据库传入整份 DTO。

所有计时包含 DTO 编解码与调用。每个样本核对全部字段或删除后的空库，更新/转账另按每键预期值检查；再由独立进程重新连接并校验。逐条时延采用确定间隔抽样，每线程最多 100,000 次；FengquDB 性能模式写场景每 8 次抽一次，其余场景每 64 次抽一次。采样开销包含在吞吐中，P99 不是全量时延直方图，也不是刷盘周期。

“确认吞吐”计至所有调用结束；“含最终同步吞吐”包含末尾屏障：FengquDB sync、Redis WAITAOF、MySQL FLUSH ENGINE LOGS、SQLite FULL + WAL checkpoint。它们的工作量不同，单独显示。指标不能代表每次调用已在确认前持久化。CPU 与内存为整个容器的采样，内存含文件缓存；短 CRUD 采样较少。

本报告不是同等丢失窗口测试，也不是可靠模式、断电、超内存、长时间稳定性或远程网络测试。不把客户端与服务架构差异全部归因于编码。不要将此处胜出解读为任何机器和负载均领先。

## 文件与版本

- [results.json](results.json)：所有样本、版本信息、镜像标识、Core 哈希、驱动输入哈希和实际设置。
- [results.csv](results.csv)：每轮吞吐、延迟及资源采样。
- [raw-evidence.zip](raw-evidence.zip)：原始 stdout/stderr、试验设置、oracle 与重开验证。
- [reproduction-inputs.zip](reproduction-inputs.zip)：冻结基准驱动、最新已验证 Index Core 源码及当次 Linux 库。主工作树当时有并行 SDK 修改，所以测试库和源码在此明确冻结；不能用其他构建代替后宣称复现。

当次 Core SHA256：`b8f07ad194abbec18d02a5e73494455701410ed5f93544ab5bdea35041100d3b`。MySQL 8.4.11、Redis 8.10.3、SQLite 3.54.1、yyjson 0.12.0；详细编译器、内核、镜像哈希见 JSON。版本仅表示测试环境，不表示 SDK 最低要求。

## 复测

需要可运行 Linux 容器的 Docker、Python，以及本项目原生构建工具。以下为仓库根目录中的生产方命令，不是用户 SDK 安装步骤。

```text
python scripts/fetch-benchmark-references.py
docker build -f tools/final_benchmark/Dockerfile.base -t fqdb-final-bench-base:20261107 .
python scripts/run-public-benchmark.py --build --core /absolute/path/libfengqudb.so.2 --core-root /absolute/path/frozen-core --output artifacts/my-public-run
python scripts/measure-benchmark-codec.py --input artifacts/my-public-run
python scripts/publish-benchmark-report.py --input artifacts/my-public-run
```

`--core-root` 应指向与待测库对应的源码树。可解压复现 ZIP 使用 `core-source`，按 Release 构建共享库，或使用当次的 `image-context/native/libfengqudb.so.2`。先运行 `--pilot --bodies 128 --threads 8` 可验证驱动；导出器拒绝把 pilot 当作正式报告。

基础镜像定义固定 Ubuntu digest、MySQL/hiredis 包版本及 Redis/SQLite 哈希；旧包若从上游仓库移除，需使用对应官方快照，不能静默升级后仍引用原环境。原始库与复建库的哈希可能因工具链或构建选项不同而不同，应记录实际哈希。

报告文件采用项目 Apache-2.0；包含第三方参考内容的复现输入保留各自许可证。MySQL、Redis、SQLite 名称归各项目所有。
