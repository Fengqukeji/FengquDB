# Wiki lease / heartbeat 修复与验收（2026-11-14）

状态：本轮仅修改 Wiki 的租约代码和相关测试；共享 Core、Index 包装器、manifest 和 SDK 原生库均未修改。生产源码已停止编辑。

## 根因与修复

1. `_claim` 在等待 Repository 写锁/事务 admission 前取得时间戳。受控延迟200ms、原租期150ms时，旧实现返回的租约已过期50ms。现在在事务操作内取时，每次重试重新计算截止时间。
2. `_renew` 将“截止时间已到”直接判为丧失所有权，但发布流程本来就是按 token/state fencing。这会让一次调度迟到永久停止心跳，即使没人接管。现在到期只使任务有资格被领取；当前令牌仍匹配时允许事务内续租。竞争者先成功接管后，旧令牌不能续租或发布。
3. 旧心跳只覆盖模型调用，准备、缓存处理和发布不在续租范围内。现在覆盖整个已领取作业；首次心跳按剩余租期提前，快作业不额外强制写一次续租。
4. I/O、CONFLICT、TIMEOUT 等瞬态存储异常不证明令牌已失效，按原有有界心跳频率重试；其他异常停止心跳，发布仍保留 token/state 检查。

没有增加150ms租期或加入额外宽限期。有限TTL不能承诺进程任意长时间停顿后永远不被接管；续租与接管谁先提交，谁取得事务内的权利。模型调用仍为至少执行一次语义。

## 测试时间线

原失败用例用实际 sleep(0.3) 假设后台线程必定在150ms内醒来并完成可靠提交。现在保留150ms租期，显式将测试时钟推进300ms，并等待续租事务实际完成，再验证不能重复领取。接管测试显式模拟已停止的 heartbeat，继续验证旧 worker 不可覆盖新结果。

另有一次Linux重复验证在心跳段之前出现“4 done、3 pending”的排空断言。清理前没有捕获其原始队列，因此不声称已证明该次时钟轨迹。单项80轮及原8项序列100轮均未再次复现。已独立证明：排空前墙钟回拨1ms会精确复现相同计数；`run_jobs`只取当前到期任务，时钟恢复后七项全部完成。测试现在把排队、计数和续租放在同一显式时间线上，所有计数/取消/接管断言保持不变，没有修改生产队列的到期语义。

## 验证结果

- 修复前HEAD的4个租约方法与当前实现以隔离测试类对比：新增4项确定性故障测试全部失败（0个测试错误）；修复后全部通过。
- 定向8项覆盖：四个新增故障、原worker限额/计数/取消/续租、双worker fencing、进程重启回收、源更新阻止晚到发布。
- Windows定向20轮共160次：0失败、0错误；Wiki套件53/53、0跳过，4.326秒。
- Linux portable / Docker定向20轮共160次：0失败、0错误；Wiki套件53/53、0跳过，3.183秒。
- 最后仅调整测试整体时钟后，Windows该单项又定向通过；未再重复Windows整套。Linux最终套件使用完整的新测试时间线。

定向记录与原始偶发失败保存在本工作树忽略目录 `build/lease-review-20261114/`，未覆盖失败证据。没有扩大到共享Index大结果测试、跨项目部署或其他SDK工作。

最终测试使用的配对运行库SHA-256：

| 平台 | Core | Index |
| --- | --- | --- |
| Windows | b35269a424f653fbccda077f588f0ddb98afc2077a3ead39f3103461ab0d59b4 | 8ce87fe62b4b5401ebabf525b4934338a5e1119a55319f959ed7c1380e7c159f |
| Linux portable | b8f07ad194abbec18d02a5e73494455701410ed5f93544ab5bdea35041100d3b | 233d38393038f5ef86ccadbf2c52e3d51001d267080726a38998624ddf6b2c4b |

## 冻结范围

四个源码/测试文件SHA-256：

- `llmwiki/wiki.py`: `60ac832171d975c073084c4c2720952687f27f6ab42ebbbf00f2c3d0d514de58`
- `tests/test_llmwiki.py`: `bf0b896b57f8c759de7057846541e1063e71ddf3dcfa948e54fb86c5b6176b27`
- `tests/test_llmwiki_foundation.py`: `019f7110fc188e054fa396841cc478ea7121cca1383e0312f897b8c99e3496b6`
- `tests/test_llmwiki_leases.py`: `1230fd4b2a74bd69028289a7596125f96a79ebd5d3ead94dfaf24544f6b42dcf`

生产文件 `llmwiki/wiki.py` 与 `D:/study/FengquDB-index/artifacts/foundation-20261114-r2/python/FengquDB/llmwiki/wiki.py` 字节一致，未改变该包中的生产内容。发行清单、release_id与最终重打包由Root负责。本修复不改默认FULL，也不编辑三份共享Python Index文件。
