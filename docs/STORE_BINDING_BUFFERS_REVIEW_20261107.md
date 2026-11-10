# 可选读取缓冲复用审查（2026-11-07）

Python 为 Store 准备键新增 bind_buffer，固定 bytearray 导出并复用 ctypes 输出参数；Java 新增 getIntoDirect，直接向可写 direct ByteBuffer 的 position/remaining 区间复制。两条路径按需创建，原来的 Get/get_into/getInto 行为与包依赖保持兼容。用法与所有权见 [复用读取](STORE_BINDING_BUFFERS.md)。

## 审查与稳定性

- Python 不在读取器中长期保存 `_NativeOwner`，避免准备键 close 等待闲置读取器释放而死锁。每次调用临时获取原生参数所有权，读取器 close 与 read 串行化。bytearray 的活动导出阻止扩容导致指针失效，close 后释放导出。
- Java 使用独立方法名，避免给原来的 null 调用引入重载歧义。公共入口拒绝只读/heap/null；JNI 检查 direct 容量、指针和偏移。JNI 局部引用保证调用期间目标有效，保留原有 Resource 读写租约；不使用 GetPrimitiveArrayCritical 包围可能阻塞的读取。
- Linux 编译发现旧维护选项聚合初始化没有显式填写新 ABI 尾部；其隐式零初始化本来有效，本批补齐显式零值与空指针，消除缺失字段警告。
- 不把普通读取转换为额外占用 pinned budget 的 view，不修改共享缓存、持久性、私有完整二进制缓冲及减法裁剪。

证据根为 `build/store-buffers-20261107/`：

| 验证 | 证据 | 结果 |
| --- | --- | --- |
| Windows Python 默认租约、显式 fallback 租约、Java direct | windows-final/report.json | 4 条编译/运行命令成功，源码清单未变 |
| Linux 同三条生命周期检查 | linux-final/report.json | 4 条命令成功，源码清单未变 |
| 旧版已编译 Java 类，无需重编译 | legacy-java-windows-final.log、legacy-java-linux-final.log | 两平台读取/复用/批量及数据校验通过 |
| 正式核心 DLL + JNI 的三语言业务互通与成本 | business-final/report.json | 25 条命令、69 组测量，源码清单未变 |
| 更新后业务共库 Win→Linux→Win 顺序交接 | linux-business-handoff.log、windows-business-return.log | 字段、约束、整理及重启通过 |
| 隔离 SDK 安装 | sdk-check.json、sdk-install.log | 安装的绑定和示例与源码相同，无示例编译缓存或业务 jar |

覆盖输出不足时原字节不变、容量恢复后的读取、零长度、缺失、slice、offset/limit 保持、整理、父 Store 关闭后的子句柄、准备键关闭与读取竞争。Python 通过保留真实 native 调用的参数租约，确定性验证关闭必须等待当前调用完成，且闲置读取器不能阻塞 key.close。Java 每线程独立 direct buffer，与 key.close 竞争，并启用 -Xcheck:jni。fallback 检查在普通 CPython 上强制使用计数租约，不等于实际在 free-threaded Python 或 PyPy 上运行。

Windows JDK 为 Unity 附带 11.0.14.1；Linux 使用独立 Temurin 11.0.33+10，下载来源及 SHA256 保存在 artifacts/tooling/java11-linux-11.0.33/origin.json，没有安装或替换系统 JDK。Linux 构建存在 WSL 挂载盘时钟偏差提示，编译和运行均成功。初次手动兼容检查被 PowerShell 拆分了未加引号的 -D 参数，JVM 未进入程序；修正命令后成功，初始日志保留。

## 同条件可选路径测量

沿用 4176 B 业务载荷，Windows 同机同核心 DLL；三轮交替语言顺序，每轮单条一百万次，预热后计时。下表是各轮中位数，读取吞吐按记录/秒，P99 为每 64 次抽样一次的调用时间。启动、写入样本及校验不计入。

| 语言与路径 | 吞吐 | 抽样 P99 μs |
| --- | ---: | ---: |
| Python 复制返回 bytes | 429,143 | 3.9 |
| Python 原 get_into | 655,581 | 2.6 |
| Python 固定目标读取器 | 907,523 | 2.0 |
| Java 复制返回 byte[] | 2,511,968 | 0.7 |
| Java 原 getInto(byte[]) | 5,158,803 | 0.3 |
| Java direct ByteBuffer | 7,564,028 | 0.2 |
| C# 原准备键复用 | 8,949,563 | 0.2 |
| 原生准备键复用 | 11,333,300 | 0.2 |

本次 Python 固定读取器比同轮原 get_into 提高 38.4%，比普通复制读取提高 111.5%；Java direct 比同轮 byte[] 复用提高 46.6%，比普通复制读取提高 201.1%。Java direct 约为同语义原生吞吐的 66.7%，Python 仍只有约 8.0%，语言执行和 FFI 成本仍在。没有将收益外推到冷数据、并发、写入、移动平台或其他数据库，也没有宣称消除全部语言衰减。

普通路径相比前一批有正常运行波动，例如 C# 复用约 9.146M → 8.950M/s；本批没有改变它的实现，不据此归因于新可选接口。原始全部工作负载、逐轮 P99 与校验和位于 report.json，保留结果中的负值。

## 包体与边界

Windows 核心 DLL 保持 790016 B，SHA256 `1ec3cbd02a709e983b4cc00a55a280a27c8e6a8f1b7103ba2616c201b245e44e`；可选 JNI 从 188416 B 增至 188928 B，增加 512 B。新 JNI 的 SHA256 为 `7e7c0c37376b9ecf797f9a552a683cff8a48afeccaa4b047cbc9983164d216ea`。Python 仍只用标准库，业务 Protobuf 依赖不进入核心。已发布 0.3.2 DLL 保持 `3411f07db33140f7b7fc1a479d36c51b862460295680194462489a39d07ee9c9`，未覆盖发布目录或打包。

本批为绑定局部变化，运行两平台相关边界和互通，没有机械重跑引擎全量回归。移动真机、长期数值边界、必要能力补强和最终 Docker Linux 2 核/4 GiB 的跨数据库持久性与最大吞吐对比仍未完成。
