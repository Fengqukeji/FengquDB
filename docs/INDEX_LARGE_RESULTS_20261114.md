# 大结果读取修复与底座同步

合法的批量查询可以超过单条数据大小限制。本次修复的是查询输出错误复用单值 16 MiB 限制，未减少记录数、未降低 limit。

- PTpma 原始 20 × 900,000 字符正文用例：19 条、20 条、summary 限定字段查询均成功；20 条完整输出 18,005,392 字节。
- 默认仍返回完整文档；工作内存与编码后结果大小分别设预算。结果预算默认 64 MiB，可增大，不是数据库的固定总结果上限。
- 另用 80 条合法记录验证可调预算：完整正文共 72,000,880 字节，64 MiB 预算明确返回 LIMIT；提高到 128 MiB 后 80 条全文全部一致。
- 可选 HEADER 投影：同一原始用例 20 条输出 2,972 字节，ID、评分、版本、元数据、来源和 payload 与完整结果一致；不返回文本和向量，Get 仍可完整读取。
- 返回时裁剪已存二进制的连续范围，不重新解码、编码全部正文，不回传内部词项缓存。
- 超过输出预算明确返回 LIMIT、空缓冲；allow_partial 不会把装不下的结果伪装成完整结果。
- after_id 仅用于 List 的 ID 分页；Search 不支持排名分页，传入该参数明确报错。

## 已验证范围

Windows 相关 CTest 5 组通过；C++ 和 Python 新增完整 20 条大记录回归，覆盖全文、概要、Get、输出预算及错误缓冲。Python 共 23 项通过。Linux 便携 GCC 11 原生 C++ 大结果测试、扩展索引 914 检查和 Python 23 项通过。Linux ASan/UBSan/泄漏检测通过新增大结果所在 C++ 测试组。

组合 Python SDK 在 Windows/Linux 的空目录接入检查均通过，覆盖 Core、Index、RAG、Wiki。新版 RAG 记忆 38 项、来源 26 项通过。Wiki 回归发现独立的任务续租问题，由 Wiki 会话修复领取计时、续租及心跳覆盖范围；Windows/Linux 各 53 项（含新增 4 项）通过，修复提交 `ff05368` 已推送。生产文件与 r2 包字节一致，详细边界见 Wiki 分支 `docs/LLMWIKI_LEASE_FIX_20261114.md`。

## 消费者接入与部署验收

PTpma 会话已完成项目索引、电脑经验和技能发现的适配、审查及既有开发期索引部署。该会话报告 Windows/Linux 正式记录单元各 872 项、Windows 相关 CTest 8 组通过，保留完整 20 条大记录和反复关闭重开的验证。经验的适用条件、观察租约、反馈及成本等语义保留在底座之外；其他业务存档未清理。PTFramework 的实际索引已返回 `fengqudb-core-index-v1`，Unity 运行时、插件和程序集未修改。

发布通过 SkillsManager 更新 PTFramework 普通源、中央库及原有 5 个目标（PTpma、WordGame、webframework、FengquAI3D、FengquAI）。协调端独立逐文件核对这 7 个入口的 35 个运行文件及 release_id，全部匹配。已运行的旧 PTpma GUI 不会被热替换，新启动的进程使用新版。

WordGame 会话已整体更新 r2 Python SDK，报告 42 项相关回归通过，正式包追加的 3 项 SDK 验收包含在这 42 项中；协调端独立核对完整 40 个交付文件。现有 KaLM、玩家存档和上下文预算保持原状。

上述项目测试来自对应接入会话的验收记录，文件哈希复核由协调端直接执行；不混算为数据库核心测试。部署清单、验收来源及其摘要哈希保存在 `docs/evidence/foundation-deployments-20261114.json`。

2026-11-15 补充：另一条由 FengquAI 原生技能中心管理的同名技能，已有 Codex/OpenCode 两个部署仍未更新。其普通源已备份并准备同版工具；唯一一次公开 `update` 请求被 `INVALID_PATH — Skill path crosses a link or its registered directory` 拒绝，API 未给出具体被拒路径。管理库和两个代理目录仍与旧包一致，未冒称新版验收通过。此拒绝来自原生管理器的路径校验，非自动审批系统。该范围与前述已完成的 7 个入口区分记录，后续须通过正常管理流程解决；未改写私有目录或转移管理归属。

## 更新规则

三个分支使用一致的 Core 源码和公开桥接；公共原生索引归 Index 分支，RAG/Wiki 使用同一发布依赖。Python 共享包装由 Index 维护并同步到 Wiki 工作树，RAG 复用它。

`scripts/package-foundations.py` 在打包前核对三个工作树的 Core 源码、公共头文件、Core Python 包装及共享索引包装。不一致直接停止；不允许把旧 Core 混进新底座。输出 `manifest.json` 记录每个交付文件的 SHA-256 和整套 `release_id`，供宿主记录实际依赖版本。更新完整目录并重新构建宿主，不能只单独替换一份 DLL。

原始接口复现与 ASan 记录保留在 `docs/evidence/index-large-result-20261114-*`。发布依赖身份见各分支 `docs/FOUNDATION_DEPENDENCIES.json`；旧验收记录继续作为历史证据保留。
