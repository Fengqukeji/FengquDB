# FengquDB 0.2.2 与 SQLite：实际调用对比

实测于2026-11-05/06，同一台 Windows 11 x64（build26200）、AMD Ryzen 9 9950X3D、D盘NTFS。原生代码由 MSVC19.51 Release编译；使用已交付的0.2.2核心DLL。本轮没有修改引擎或语言封装，没有重新发布0.2.2运行库。

结论：在本轮能全部装入内存的KV样本中，原生随机主键查询约3倍、缓冲更新约2.5倍；逐笔可靠提交通常仅略快，100条批量插入由SQLite领先。Python普通查询几乎持平，不能把原生内核优势套到所有语言接口。单线程下FengquDB的内存使用更高。

## 数据与配置

* 每个样本新建独立文件，预填100000条，固定宽度14字节二进制键、256字节二进制值；预填、生成键、预热和验证不计入计时。应用层Protobuf编解码与业务处理未计入。
* 原生对手是从 [SQLite官方网站](https://www.sqlite.org/download.html) 下载并校验SHA3-256的3.54.1 amalgamation。与核心同为共享库、Release优化；SQLite采用THREADSAFE=1并启用受支持的Release IPO，没有关闭线程安全来求速度。来源和文件哈希见 artifacts/reference/sqlite-amalgamation-3540100/origin.json。
* SQLite使用 `CREATE TABLE kv(k BLOB PRIMARY KEY,v BLOB NOT NULL) WITHOUT ROWID`，复用预编译的查询/写入/开始/提交语句，绑定字节，不在每次操作重新解析SQL。[WITHOUT ROWID](https://www.sqlite.org/withoutrowid.html) 适合这种非整数主键表。
* 两边先完整扫描预热。SQLite每个连接设置64 MiB页缓存、mmap=0、4 KiB页、WAL、wal_autocheckpoint=0，构造时查询并校验实际PRAGMA；没有拿默认小缓存强迫SQLite反复读盘。
* 可靠提交：FengquDB同步模式对 SQLite WAL+synchronous=FULL，计时包括事务开始、操作、提交及资源清理。提交已逐次同步，未把额外的SQLite检查点强加到这类吞吐计时。
* 缓冲提交：FengquDB buffered后显式sync，对 SQLite WAL+NORMAL后显式TRUNCATE检查点；最终同步包含在总耗时。按 [SQLite同步文档](https://www.sqlite.org/pragma.html#pragma_synchronous)，NORMAL的WAL检查点负责同步WAL和数据库；这两种收尾操作都计费，但其内部工作并不相同。没有使用synchronous=OFF。
* FQDB保留默认历史限额（1024提交/16 MiB编码载荷），未挂诊断接收器。SQLite未额外模拟增量历史，这项功能和开销不完全相同。
* 原生每个场景交替执行两个引擎，三轮取中位数；除读改写场景需要根据读值计算外，每次操作的键和新值预先生成。每个样本校验初始/最终全部键与全部256字节，并关闭重开再验证，读取校验和跨引擎一致。

这些是匹配当前KV任务的合理配置，不是穷举SQLite所有极限调优；未测试mmap、exclusive locking、不同页大小/缓存、共享缓存或其他表结构。缓冲模式的每次提交没有抗断电保证，最终显式同步才提高持久化水位。

## 原生 C/C++ 边界结果

直接调用两个库的C API，不含Python、JNI或P/Invoke；C++RAII接口的完整使用例子另有编译执行。倍数为FengquDB吞吐÷SQLite吞吐，小于1代表FengquDB更慢。

| 场景 | 单位 | FengquDB | SQLite3.54.1 | 倍数 |
| --- | --- | ---: | ---: | ---: |
| 随机主键查询，分配并复制结果 | 次/秒 | 1,633,004 | 536,013 | 3.05× |
| 随机主键查询，复用256字节缓冲区 | 次/秒 | 1,764,561 | 544,444 | 3.24× |
| 固定快照中查询，借用结果字节 | 次/秒 | 2,035,372 | 1,349,032 | 1.51× |
| 反复查询同一个不存在的键 | 次/秒 | 12,300,184 | 875,984 | 14.04× |
| 范围扫描，每次返回100条 | 次扫描/秒 | 303,970 | 92,134 | 3.30× |
| 4线程随机查询，复用缓冲区 | 次/秒，合计 | 4,329,328 | 1,586,132 | 2.73× |
| 新增1条，逐事务刷盘 | 事务/秒 | 4,118 | 3,730 | 1.10× |
| 更新1条，逐事务刷盘 | 事务/秒 | 4,562 | 4,307 | 1.06× |
| 删除1条，逐事务刷盘 | 事务/秒 | 4,604 | 4,346 | 1.06× |
| 新增100条后一起刷盘 | 事务/秒 | 2,110 | 2,392 | 0.88× |
| 单条缓冲更新，包含末尾同步 | 事务/秒 | 170,532 | 67,783 | 2.52× |
| 读两条、改两条、原子提交并刷盘 | 事务/秒 | 4,642 | 3,661 | 1.27× |

数量：普通/缓冲区/快照/缺失各250000次；范围2000次（每次100条）；并发每线程100000次，共400000次；单条可靠增/改/删及两条读改写各1000事务；100条批量500事务；缓冲更新20000事务。

复制结果场景两边都安排一次调用方拥有的分配/复制/释放；复用缓冲场景两边复制到同样的256字节缓冲；借用场景两边都保留稳定读事务/快照，只消费本次有效字节。定时阶段读取值的标识、版本、长度和末尾字节，完整逐字节验证在计时外执行。

范围行的303970等数值是“每秒返回100条的扫描次数”，不是全库扫描次数。批量插入约2110/2392是事务/秒，换成记录约210952/239187条/秒；不能将其与单条事务的数字混淆。同一不存在键的14倍属于高度局部的热未命中测试，不代表随机不存在键或综合应用都有14倍。

四线程使用共享FQDB数据库句柄，SQLite为四个工作线程各建一个连接/语句；两边均预热后一起开始，排除建连、预热和关闭工作连接的时间。SQLite准备数据的主连接仍保留，内存包括该连接。没有测并发写入、读写混合或冲突重试成本。

## 原生写入延迟

每个单元格依次为三轮中位数的P50/P95/P99，单位μs；批量是整个100条事务的时间。缓冲事务分位数不含末尾统一同步，吞吐总耗时包含。

| 场景 | FengquDB | SQLite |
| --- | ---: | ---: |
| 新增1条，逐事务刷盘 | 204.9 / 318.9 / 426.4 | 255.7 / 343.4 / 394.4 |
| 更新1条，逐事务刷盘 | 205.5 / 283.0 / 348.2 | 220.9 / 301.6 / 368.8 |
| 删除1条，逐事务刷盘 | 204.2 / 285.8 / 362.8 | 209.8 / 305.8 / 377.1 |
| 新增100条后一起刷盘 | 456.5 / 581.1 / 693.6 | 402.7 / 511.4 / 643.1 |
| 单条缓冲更新，包含末尾同步 | 4.9 / 6.1 / 36.5 | 8.2 / 21.4 / 43.8 |
| 读两条、改两条、原子提交并刷盘 | 203.0 / 286.4 / 343.4 | 256.0 / 329.4 / 431.3 |

## Python 实际接入结果

Python3.12.10：本项目当前ctypes便利封装，对Python标准库sqlite3（其自带SQLite3.49.1，不是上面的3.54.1）。这是各自现成Python接口的端到端比较，版本、编译工具链和语言封装不同，不与原生表混算。

同样10万条/256字节/预热/WAL持久化配置；普通查询100000次、缓冲更新10000事务、100条批量插入100事务，三轮交替取中位数。SQLite使用语句缓存128、批量使用executemany；FQDB使用get/put/事务with接口。调用方Python循环、FFI、返回bytes和事务封装均计时。两边均逐字节核对最终数据并重开验证。

| 场景 | FengquDB 次或事务/秒 | SQLite 次或事务/秒 | 倍数 |
| --- | ---: | ---: | ---: |
| 普通 get / SELECT 返回 bytes | 317,003 | 307,877 | 1.03× |
| 单条缓冲更新，含最终同步 | 68,979 | 40,137 | 1.72× |
| 100条批量插入并可靠提交 | 1,317 | 1,775 | 0.74× |

普通查询约1.03倍，实质上接近。ctypes参数/句柄包装、锁和结果复制，会吃掉相当一部分原生查询优势；SQLite的Python扩展在C层直接处理部分工作。批量executemany也减少Python到C的往返，所以Python接口必须单独测，不能从C++结果外推。C#/Java接口本轮未做性能对照。

## 内存和文件

原生普通读场景，预热后的进程私有内存增量约FQDB54.46 MiB、SQLite35.85 MiB；FQDB约多52%。这是预先创建调用方键/期望值后，打开前到预热后取样的PrivateUsage差值，含分配器/历史/索引开销，不是全进程峰值、总内存上限或精确对象计量。

四线程取样约FQDB54.69 MiB、SQLite177.39 MiB；SQLite四个工作连接加保留的准备连接各有独立预热页缓存，不能当作SQLite最低可能内存，mmap/不同连接管理会改变结果。100条批量场景新增50000条，最终15万条，取样约77.63/53.42 MiB。

FQDB全量值/树索引驻内存，SQLite按页缓存；指针节点和逐条分配与页内紧凑存放存在空间取舍。此次数据可装入内存，不能证明超内存场景。原始JSON另有主文件+WAL+SHM逻辑字节之和；FQDB不断覆盖会持续追加日志，未自动在线整理，短样本文件大小不能推出长期更省磁盘。

## 复现与核对

构建独立对比项目，不修改核心依赖：

```powershell
cmake -S tools/sqlite_benchmark -B build/sqlite-comparison -A x64 -DCMAKE_PREFIX_PATH=D:/study/FengquDB/artifacts/release-0.2.2/windows-x64 -DSQLITE_SOURCE=D:/study/FengquDB/artifacts/reference/sqlite-amalgamation-3540100
cmake --build build/sqlite-comparison --config Release --parallel
python tools/run_sqlite_comparison.py --executable build/sqlite-comparison/Release/fqdb_sqlite_bench.exe --output artifacts/new-native-result --runs 3
python tools/run_sqlite_comparison.py --python --library artifacts/release-0.2.2/windows-x64/bin/fengqudb.dll --output artifacts/new-python-result --runs 3
```

输出目录必须新建。只有成功样本本次创建的确切数据库文件及-wal/-shm会清理，原始采样和JSON保留。没有添加SQLite运行依赖到FengquDB核心或SDK。

原生原始72组：artifacts/sqlite-comparison-20261105/comparison.json、samples.jsonl。Python原始18组：artifacts/python-sqlite-comparison-20261106/comparison.json、samples.jsonl。保留二进制/源码指纹和原始运行日志。本轮能力示例 examples/capabilities.cpp 编译执行通过。生产引擎二进制SHA256与0.2.2交付清单一致，因此未为新增基准/文档重复运行存储全量回归。

本轮只在Windows本机测量，未测Linux性能、冷缓存、长时间压力、真实断电、大值、超内存、RAG检索或关系查询。三次短样本中位数不是统计置信区间，也不证明“所有场景强多少”。从结果看，原生热点键值读取是优势，可靠刷盘更受设备影响，批量写入与语言封装仍有明确改进空间。
