# 验证与发布门槛

本文件描述测试入口与范围；0.1 结果见 `artifacts/validation-report.md`，0.2 结果见 `artifacts/validation-0.2.md`，C++ SDK 结果见 `artifacts/validation-0.2.1.md`，0.2.2见 `artifacts/validation-0.2.2.md`，本轮见 `artifacts/validation-0.2.3.md`。未执行的平台/测试不视为通过。

* `ctest`：C 共享 ABI、wire 极值与畸形输入、固定种子随机输入、事务冲突/回滚/读己之写、快照/零拷贝视图、范围与增量、逐字节截断恢复、完整数据损坏拒绝、子进程在 5 个提交阶段退出、2 种模拟 I/O/刷盘错误、缓冲水位、随机模型与多线程冲突重试。
* `ctest -R cpp_api`：C++ 两个翻译单元链接、RAII/移动、异常回滚、缺失与空值、固定视图与行生命周期、范围迭代、冲突状态、历史与检查点、多线程调用。配置共享和静态构建分别运行。
* `tests/cmake-consumer`：从独立 CMake 项目使用已安装的 `FengquDB::Cpp`，验证包查找、头文件、链接、运行与搬迁后的安装前缀。
* `sh scripts/build-linux-minimal.sh <output>`：没有 CMake 时以 GCC/G++ 构建 Linux 共享/静态库，并运行核心、C ABI 与 Python 接入测试。
* `python tools/verify_protobuf.py --protoc <official-protoc> --tester <fqdb_tests>`：官方 wire 互操作。
* 加 `--library <native-library>` 同时验证真实事务载荷和检查点分块/完结消息与官方 protoc 互操作。
* `tools/verify_compatibility.py --previous <0.1-dll> --current <0.2-dll>`：原有 C ABI、格式 1 双向读写及旧库拒绝格式 2。
* `python tests/python_smoke.py <native-library>`：Python ctypes 接入。
* `dotnet run --project tests/csharp/Smoke.csproj -c Release`：C# P/Invoke 接入；将核心原生库加入进程加载路径。
* `javac -encoding UTF-8 -d build/java bindings/java/io/fengqudb/Database.java tests/java/Smoke.java` 后运行 `java -Djava.library.path=<native-directory> -cp build/java Smoke`：Java JNI 接入。
* `fqdb_bench NEW_FILE sync|buffered TRANSACTIONS BATCH insert|update PREFILL_KEYS`：插入或预填充后更新，以及复制读取、零拷贝视图读取、全范围扫描和热恢复。值 256 字节；事务 P99 包含准备/索引处理/提交；缓冲吞吐计时包含末尾同步，但提交 P99 不包含。运行后不自动删除数据库。
* `tools/compare_baseline.py`：同一可执行文件、不同 DLL 目录，交替次序多次采样并报告各项中位数。

0.2 核心新增按键读写冲突/缺失键 ABA、无关键多线程写入、索引平衡及计数、短键/大键边界、单键更新节点分配、恢复期历史限额、游标过期与固定、检查点版本/逐字节截断/块缺失/失败隔离测试。

模拟退出使用测试专用静态构建；发布库不包含故障注入入口。确认测试调用的不是发布库，不能据此推断发布 ABI 含调试接口。

尚需生产门槛：独立 Linux 主机与 Windows/Linux 持续构建矩阵、更多编译器与架构、三语言双平台完整矩阵、扩大消毒器/模糊测试覆盖、OOM/磁盘满/真实系统 I/O 故障、真实断电、长时间并发压力、恢复大文件、备份/迁移、与 Redis/SQLite/LMDB/LevelDB 的同保证比较。WSL Linux 原生二进制验证不等于完整发行版与硬件矩阵验证。任何尚未运行的项目都须在交付说明中保留。

0.2.2 新增 logging/diagnostics 测试：回调锁外执行、重入、异常隔离、注销等待、冲突/刷盘故障元数据、并发队列计数和上限、业务字节不入日志、恢复/I/O 定位、清理隔离、只读查询及日志磁盘错误。复用缓冲区覆盖容量不足/空值/缺失/快照/事务；检查点覆盖临时阶段与发布后的退出。恢复测试验证每个不同键仅分配一次索引节点。

`fqdb_read_bench NEW_FILE` 对复制、复用、快照借用读取各取 5 次中位数，另抽样过滤接收器开销。共同基准增加内存采样：Windows PrivateUsage、Linux RssAnon；含义不同，不跨 OS 比较。采样包含分配器和其他进程内开销。

0.2.3新增 `ctest -R grouped_commit`：暂停设备同步，验证16个独立事务与sync屏障共用两次write/sync、刷盘期间普通读/事务开始/状态读取继续、同组冲突、版本和历史、旧快照稳定、整组部分写入/同步错误的不确定状态与恢复、私有批量树节点分配。ASan/UBSan脚本也覆盖该测试。

`tools/run_concurrency_comparison.py` 使用同一可执行文件加载旧/新DLL，与SQLite WAL FULL对照1/4/16个可靠写线程及4读1写，保留三轮原始样本、尾延迟、write/sync区间计数及二进制哈希，关闭重开后核对全部值。`tools/run_sqlite_comparison.py --label VERSION` 记录12个原生场景或3个Python场景；Python批量入口对旧库回退逐条调用。

0.3新增 `ctest -R optimizations`：小映射随机模型、CRC组合、准备对象/缓存生命周期、并发原生命令、增量链及回退、逐字节截断、部分写/同步失败、缺失/删除依赖失效、最大版本哨兵及proto3零字段省略。三语言advanced覆盖接口和关闭竞态。`tools/verify_delta_log.py` 对照官方protoc验证格式3增量与旧库拒绝、旧库打开物化检查点。最终结果见 `artifacts/validation-0.3.0.md`。
