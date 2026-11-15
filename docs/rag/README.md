# FengquDB RAG

2026-11-14：已实现通用 Python RAG 底座及 WordGame 兼容门面。存储和检索使用公共原生 Core/Index，没有新建向量引擎、下载模型或引入远程数据库。证据和限制见 [ACCEPTANCE.md](ACCEPTANCE.md)。

独立工作树 D:/study/FengquDB-rag，分支 codex/rag-foundation，基线 32348fb7d269a4b328936d8ab1d208d70b7f9a64。领域实现与公共依赖后来已由主会话集成提交；当前提交以 Git 为准，当前发行摘要见 [FOUNDATION_DEPENDENCIES.json](../FOUNDATION_DEPENDENCIES.json)。其他工作树及 WordGame 接入由各自负责人管理。

## 已交付

- bindings/python/fengqudb_rag：账户隔离、精确去重、长文分段、事实时间版本、纠错/CAS、审计分页、来源/谱系删除、持久清理、namespace 退休与恢复。
- 原生 dense exact、可见性统计 BM25、RRF 混合、注入重排、带引用与字符/token 预算的上下文。授权、时间和清理屏障在召回前进入原生过滤。
- 可注入 Embedding，完整契约校验。WordGame 复用当前 KaLM Provider，模型由宿主管理。
- integrations/wordgame_rag：接受现有 Pydantic 模型和 npc_id 别名，保留旧 MemoryStore 方法签名。
- NativeRagStore：接受预计算向量的有界 source/chunk 原子替换 API，见 [NATIVE_SOURCE_PATH.md](NATIVE_SOURCE_PATH.md)。

权威内容、向量、治理目录和原生 Index 同事务可靠提交。查询使用同一快照，输出前复核当前版本和清理屏障；超限、不完整或撤销竞态报错。数据库默认 50ms/1MiB 写策略未改。

## 入口

| 文档 | 内容 |
| --- | --- |
| [API.md](API.md) | 环境、调用、WordGame 注入、错误和预算 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 实际模块、存储键、事务、隔离和检索 |
| [ACCEPTANCE.md](ACCEPTANCE.md) | 需求映射、测试、真实模型、性能、自审 |
| [DELIVERY_PLAN.md](DELIVERY_PLAN.md) | 后续 WordGame 接入责任 |
| [WORDGAME_REQUIREMENTS.md](WORDGAME_REQUIREMENTS.md) | 原业务复杂需求与源码审计 |
| [NATIVE_BRIDGE.md](NATIVE_BRIDGE.md) | 公共 Store 身份与租约 |

RETRIEVAL_CONTRACT.md、PROVIDER_CONTRACT.md、CONTRACT_TEST_PLAN.md、COORDINATION_REVIEW.md 保留早期协议讨论。实际接口以源码、API.md、ACCEPTANCE.md 为准。本轮没有新增独立 Rag C ABI/C++ 库或 C#/Unity RAG 绑定。

## 接入边界

用户已取消旧数据迁移。本包没有迁移工具，不删除真实旧库；接入采用明确新根目录，由游戏按业务授权重建。旧目录、元数据缺失或模型契约错误不会被当成正常空库。

游戏 SQLite、观察、outbox、NPC 墓碑、规则、能力、图标和提示词仍由游戏管理。HTTP 与真实 KaLM 已在隔离测试接入，正式 WordGame 切换及完整游戏回归尚未执行，由主会话统一协调；本会话不通知 WordGame。

默认 dense exact；未做大规模或旧 zvec 对照，不能宣称全面更快。逻辑删除不等于安全擦除旧 WAL、快照或外部备份。
