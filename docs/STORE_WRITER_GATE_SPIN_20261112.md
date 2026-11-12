# 缓冲写多线程弱项：WriterGate 自旋（2026-11-12）

## 结论

缓冲模式下 256 B 的 `update`、`patch8`、`insert`、`put_many` 在多线程时落后，主因是写者锁 `WriterGate` 的交接开销，不是存储路径。这些操作走 `commit_direct`，调用线程各自抢 `Core::writer`。原实现是 mutex + condition_variable，抢不到就停车，释放时唤醒，8 个线程在 2 vCPU 上反复停车和唤醒。

证据（基线，Linux 2 vCPU，32768 条，256 B，每例 2 秒，3 轮）：

- `update` 1 线程 352k/s，每条记录 CPU 2.9 µs，领先 SQLite（138k）。
- `update` 8 线程 70k/s，每条记录 CPU 18.8 µs（1 线程的 6.5 倍），p50 从 2.3 µs 升到 16.5 µs，落后 SQLite（114k）。
- 持锁区本身约 3 µs，不能解释每次 14 µs 的串行间隔，所以时间花在线程交接上。

此归因来自 CPU/记录和延迟的变化与改动后的 A/B 结果，没有做 perf 剖析。

## 改动

`src/paged/core.hpp` 的 `WriterGate`：先用原子交换抢锁，失败后最多自旋 1024 次 `pause`，仍失败再停车。解锁只在有停车者时才进入互斥量并唤醒。仅在进程有两个以上可用 CPU 时自旋（沿用 `AdaptiveCommitWait::parallel_execution`）。接口、持久性语义和磁盘格式不变。

## 同轮交替 A/B（缓冲模式，records/s 中位数）

自旋预算选择（8 线程，4 轮，`build/bufwrite-ab-06`）：

| 负载 | 基线 | 256 次 | 1024 次（采用） | 4096 次 |
| --- | ---: | ---: | ---: | ---: |
| update | 67k | 208k（ab-03） | 283k | 276k |
| put_many | 425k | 450k（ab-03） | 640k | 863k |
| mixed95 | 1.89M | | 2.02M | 1.75M |

4096 次的 `put_many` 最高，但 `mixed95` 回退约 7%，`put_many` p99 升到约 8 ms，所以采用 1024 次。

采用版本与基线（`build/bufwrite-ab-07`，3 轮）：

| 负载 / 线程 | 基线 | 采用 |
| --- | ---: | ---: |
| update t1 | 347k | 352k |
| update t2 | 85k | 316k |
| update t4 | 74k | 299k |
| patch8 t4 | 124k | 428k |
| insert t4 | 90k | 483k |
| put_many t4 | 440k | 593k |
| mixed95 t2 | 1.39M | 2.19M |
| mixed95 t1 | 1.52M | 1.47M（在波动范围内） |

同步模式（4096 次版本，`build/bufwrite-ab-05`）：`update`、`put_many`、`mixed95` 与基线无差别。

## 未解决

- `put_many` 8 线程（约 640k）仍低于 SQLite（717k～804k）和 Redis（835k）。一次 32 条的持锁时间约 40 µs，超过自旋预算，更大的预算又伤 `mixed95`。更靠谱的方向是缩短锁内工作，或让批量事务在锁外完成准备，需要另行研究。
- 自旋会增加连续写压力下的 CPU 占用。
- 单次 2 秒、32768 条的短样本，只适用于所述容器与配置。

## 验证

Windows Release 全量 ctest 32/32 通过（排除 `diagnostics`）。`diagnostics` 在原版 `core.hpp` 上同样失败（`diagnostics_tests.cpp` 第 96 行 `repaired_bytes`），与本改动无关。Linux TSan（`setarch -R`）下 `paged_store`、`paged_atomic_owner`、`paged_buffered_submit`、`paged_write_policy` 4/4 通过。未跑 Java 和完整 TSan 矩阵。

原始数据：`build/bufwrite-base-01`、`bufwrite-ab-02` 至 `ab-07`。原版 `core.hpp` 备份在 `build/bufwrite-backup/`。
