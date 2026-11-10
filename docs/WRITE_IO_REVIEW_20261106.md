# 单条可靠写入：日志缓冲参考与 I/O 定位

日期：2026-11-06。读取CommonServer和PTFramework实际源码，并对照FengquDB0.2.2。两个参考项目源码与数据库生产内核均未修改；新增独立I/O探针和此分析，group commit尚未接入引擎。

## 两个参考项目实际如何写

| 项目 | 当前机制 | 持久化边界 |
| --- | --- | --- |
| CommonServer | 有界多生产者队列；单后台写者；ArrayBufferWriter<byte>目标64 KiB；FileStream缓冲64 KiB；默认250 ms刷新，达到批次目标也写出 | FlushAsync把应用缓冲交给系统；该模块明确不承诺fsync、断电耐久或事务审计 |
| PTFramework PTLog | 初始容量512K字符的StringBuilder；超过256K字符写出；StreamWriter缓冲256K字符；100 ms定时Write/Flush，导出/关闭也刷新 | StreamWriter.Flush，没有调用FileStream.Flush(true)；记录函数返回不等于系统设备刷盘完成。达到阈值也可能在生产线程写入，并非全部I/O都移到后台 |
| FengquDB0.2.2可靠模式 | 每次提交编码完整事务帧；Windows小帧≤1024字节合为一次WriteFile，POSIX为writev；随后FlushFileBuffers/fsync成功才发布版本并返回 | 当前要求逐次系统同步；缓冲模式另有显式sync，不与可靠模式混淆 |

位置：

* `D:/work/CommonServer/server/CommonServer/Logging/ServerLogOptions.cs:13`：64 KiB、250 ms默认值。
* `D:/work/CommonServer/server/CommonServer/Logging/JsonLogSink.cs:68`：64 KiB异步顺序文件流；`:93`为FlushAsync。
* `D:/work/CommonServer/server/CommonServer/Logging/AsyncLogProvider.cs:207`：复用编码缓冲；`:225`起处理批次/周期/屏障。
* `D:/work/PTFramework/Assets/PTFramework/LogManager/PTLog.cs:72`：StringBuilder；`:91`为100 ms计时器；`:108`为StreamWriter字符缓冲；`:361`起定时写出。
* `src/storage.hpp:142`：普通缓存文件句柄；`:278`为小帧合并；`:325`为FlushFileBuffers。
* `src/fengqudb.cpp:495`起持有同一核心锁；`:573`在锁内完成可靠刷盘。

“字符”不是字节，StringBuilder初始容量也不是内存硬上限。CommonServer的较大批次还可能直接绕过FileStream的小缓冲，不能按缓冲层数简单估算复制次数。源码历史吞吐注释与旧压力记录没有当作本轮实测。

## 缓存与设备同步是不同的步骤

数据大致经过：业务/编码缓冲 → 用户态流缓冲 → 操作系统文件缓存 → 存储设备。

FileStream/StreamWriter通常有用户缓冲；原生HANDLE/文件描述符不会自动附带同样的用户字节缓冲。FengquDB没有禁用系统缓存：Windows未使用NO_BUFFERING，POSIX未使用O_DIRECT。系统文件缓存可由同一文件的多个句柄共享，不等于每个流各占一套独立文件数据缓存。

写系统缓存成功和请求设备同步完成不能视为同一保证。[微软FlushFileBuffers说明](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers) 明确指出逐次写后调用的成本；[FileStream.Flush(bool)](https://learn.microsoft.com/en-us/dotnet/api/system.io.filestream.flush?view=net-10.0) 提供显式flushToDisk参数。日志模块当前没有走相同的逐记录设备同步路径。

对于“一条写入必须可靠完成才返回”：即使增加64 KiB用户缓冲，也要在本次返回前把缓冲写出并调用设备同步。于是1000次串行调用依旧1000次同步，缓冲基本没有积累的机会。

## 隔离实测

Windows x64、D盘NTFS、MSVC19.51 Release。使用与当前小值记录相同大小的336字节合法FengquDB事务帧，每条值256字节。文件初始化同步在计时外；编码/CRC预先生成，排除AVL索引、读写冲突、线程排队和事务分配成本。交替三轮取中位数。

每个样本写完后都用交付的0.2.2引擎重新打开，检查连续恢复到的版本、已同步版本、文件长度、键数及最终完整值，全部15个样本通过。

| I/O策略 | 记录数 | 系统写调用 | 设备同步调用 | 记录/秒 | 同步耗时占比 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 当前小帧合并写，每条设备同步 | 2000 | 2000 | 2000 | 4,597 | 98.0% |
| 增加64 KiB用户缓冲，但仍每条设备同步 | 2000 | 2000 | 2000 | 4,745 | 98.1% |
| 每64条合并写，并在同组确认前同步（I/O模型） | 4096 | 64 | 64 | 270,679 | 92.9% |
| 逐条系统写，末尾统一设备同步 | 20000 | 20000 | 1 | 674,034 | 4.8% |
| 64 KiB合并系统写，末尾统一设备同步 | 20000 | 103 | 1 | 5,364,807 | 36.2% |

逐条同步约每次207–213 μs，系统写平均约4 μs。两种逐条同步方式吞吐相差约3.2%，主要耗时仍为设备同步，三轮短样本不足以将其认定为稳定优化。此处98%是I/O探针的时间比例，不是所有数据库事务的总体比例。

合并64条后的较高数字仅是设备I/O机制演示，不是FengquDB数据库吞吐：未执行索引构造、逐事务冲突和发布等工作，也没有并发调用/等待调度。它在每组设备同步完成后才能视为本组已确认，不能先向业务返回成功。最后两行只在整个样本末尾提高持久化水位，不提供逐条可靠确认，不能拿它们与逐条可靠提交声称相同安全性。

## 可借鉴的改进，按优先级

1. **并发可靠提交使用group commit。** 借鉴CommonServer的单写者、批次缓冲和屏障，把多个已经到达的独立事务分别编码进日志，共享一次系统同步。每个事务保留独立版本、冲突验证和结果，调用者等自己的日志结束偏移进入已同步水位后才返回成功。不能把这些事务悄悄变成一个大事务，也不能照搬日志队列的满载丢弃策略。
2. **限制排队内存和等待。** 按字节与事务数限额；单个请求、空闲状态立即处理，不为凑满批次无限等待。高并发时可自然收集上一轮I/O期间到达的事务。显式sync、关闭、错误和取消也要按水位完成；写入/刷盘不确定时保留COMMIT_UNKNOWN及恢复必需语义。
3. **缩短已发布状态的锁占用。** 现在提交持核心锁直到设备同步结束，普通get/begin会被一起挡住；已有快照只读不受同一锁影响。可以分离写入串行门和已发布根锁，让读者在I/O期间读取旧的完整根，同步成功后再发布。该项主要改善混合读写的尾延迟，不会消除单次设备同步耗时。
4. **后续测量CPU与大帧。** 可借鉴复用编码字节缓冲和较大帧合并写，但必须设保留容量上限。当前Windows小帧已合为一次写，POSIX已有writev；简单叠加多层缓存收益有限。文件预分配和对齐I/O可单独实验，不能直接为当前非扇区对齐帧开启NO_BUFFERING。

单个线程“等上一条成功，再发下一条”几乎无法自然组成group commit；如果业务允许，多条修改放入一个现有事务已经可以减少同步次数。group commit主要针对并发的独立事务，和应用主动批量事务是两种能力。

实施group commit需要覆盖独立事务/同键冲突/读写偏差、快照可见性、增量版本、sync/关闭竞争、排队限额、分配失败以及组内部分写入和刷盘失败。不把日志里允许丢弃、100/250 ms定时后写出或仅后台返回当作数据库可靠提交优化。

## 复现

```powershell
cmake -S tools/io_probe -B build/io-probe -A x64 -DCMAKE_PREFIX_PATH=D:/study/FengquDB/artifacts/release-0.2.2/windows-x64
cmake --build build/io-probe --config Release --parallel
python tools/run_io_probe.py --executable build/io-probe/Release/fqdb_io_probe.exe --output artifacts/new-io-probe --runs 3
```

原始结果和源码/运行库SHA256在 `artifacts/io-probe-20261106/comparison.json`，逐次数据在同目录samples.jsonl。这里只做本机I/O定位，没有真实断电或Linux性能测试，没有修改当前SDK持久化契约。
