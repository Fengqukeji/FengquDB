# WordGame 需求审计与兼容矩阵

审计日期：2026-11-14。依据 `D:/work/WordGame` 当前源码和测试定义，不把历史日志当作当前实现或本轮测试结果。

WordGame HEAD 为 `9bf85d064f3754f3046d18018e6c6e20034ae34c`，但工作区有其他未提交开发；文件摘要见 [source-audit.json](source-audit.json)。FengquDB 专用分支基于 `32348fb7d269a4b328936d8ab1d208d70b7f9a64`，未包含原 develop 工作区尚未提交的 SDK 改动。后续接入 SDK 时须显式协调，不能覆盖或假定已合入。

## 1. 实际调用链

```text
通用 HTTP /rag 与 search_memory 工具
    → app/services/rag.py::MemoryStore
    → provider.embed_with_provider
    → KaLM 本地 CPU/CUDA llama-server
    → 每账户一个 zvec collection-v2

游戏公共行动事务（SQLite）
    → game/events.py::record
    → game_event + game_npc_observation + game_memory_outbox 同事务
    → events.flush：GM / 各知情 NPC 分别产生安全投影
    → MemoryStore.import_text → 可靠索引成功 → outbox indexed=1
    → events.recall：时间、owner 召回 + 事件存在性/观察/清除时间再校验
    → scene.context：关注筛选、近期事件去重、字符预算

NPC 结构化观察：npc_memory.recall → SQLite 有界候选排序
图标匹配：item_icons → vectors.npy + vectors.json（独立语料库）
```

后两条不是 `MemoryStore` 的现有组成，不能在替换时误删或宣称已统一。当前 `gm` 只获得非 `npc_private` 的游戏事件投影，不等于可以读取 NPC 一切私有生活的“全知身份”。

## 2. 必须兼容的 RAG 行为

以下 R 编号直接用于验收用例。路径约定：`services/`、`models/`、`api/` 相对 `D:/work/WordGame/backend/app/`，`game/` 是其中 `services/game/` 的缩写；`tests/` 和 `RAG.md` 相对 `backend/`；`frontend/` 和项目 `AGENTS.md` 相对 WordGame 根目录。

| 编号 | 现有契约 / 失败边界 | 源码依据 | 新底座归属 |
| --- | --- | --- | --- |
| R01 | 每账户独立；按 ID get/edit/delete/history 同样不能跨账户 | `services/rag.py:125,423`；`tests/test_rag.py:47` | tenant 句柄与租户存储边界 |
| R02 | scope 精确匹配；owner 支持旧 `npc_id` 别名；公共资料仅同账户同 scope | `models/rag.py:12`；`services/rag.py:463` | 授权 namespace + 兼容适配 |
| R03 | `include_public=false` 只查指定归属；空 owner 是公共归属，绝非全部归属 | `services/rag.py:443,463` | 强类型 owner 选择器 |
| R04 | 检索使用 `known_at <= as_of < expires_at`，空 expiry 为无限；不只按 is_latest 过滤 | `services/rag.py:387,463`；`tests/test_rag.py:47` | 逻辑时间版本 |
| R05 | 完全相同的 payload 重试复用 ID，重复项不再次 Embedding；不同来源/归属不能合并 | `services/rag.py:80,262`；`tests/test_rag_governance.py:47` | 精确去重与写入准备 |
| R06 | 长文按 1200 字符、100 字符重叠分段；缺 source_id 时 `import:SHA256(全文)`；重复导入复用片段 | `services/rag.py:363`；`tests/test_rag_governance.py:56` | 命名 chunker 的 legacy 配置 |
| R07 | 长文导入拒绝 fact_key；不能把所有分段当同一事实版本 | `services/rag.py:363` | 导入验证 |
| R08 | 同 scope/owner/fact_key 的新事实必须更晚；关闭前版、不延长原失效时间、保留过去；同批多版一致 | `services/rag.py:262`；`tests/test_rag_governance.py:65,81` | 原生事实谱系事务 |
| R09 | 纠错保持 ID、归属、source_id、fact_key，追加旧版本审计；事实的生效区间不能原位修改 | `services/rag.py:262`；`tests/test_rag_governance.py:92,196` | 原生 edit + CAS |
| R10 | expected_revision 拒绝旧页面；Embedding 后再次核对 revision/is_latest；已关闭事实不能编辑 | `services/rag.py:262`；`tests/test_rag_governance.py:290` | prepare/commit 双阶段验证 |
| R11 | history 汇集事实版本与编辑历史，revision 游标严格递增且分页不重复 | `services/rag.py:408`；`tests/test_rag_governance.py:290` | 谱系级修订序号与独立历史键 |
| R12 | 单条 delete 删除整条谱系、原文、向量及编辑历史；旧事实不重新生效 | `services/rag.py:430`；`tests/test_rag_governance.py:106` | 谱系删除 |
| R13 | delete_matching 必须有 scope 和精确 owner；source 限制只删该来源，不扩大到其他来源的整条谱系 | `models/rag.py:67`；`services/rag.py:443` | 精确选择器与分批删除 |
| R14 | 删除最新来源后，后续事实仍接在保留谱系上；已关闭旧版本不重开 | `services/rag.py:262`；`tests/test_rag_governance.py:139` | 缺 head 的谱系续接 |
| R15 | 在途 Embedding 遇删除不能把原记忆/已清理归属写回 | `services/rag.py:262,439`；`tests/test_rag.py:126`；`test_rag_governance.py:123` | 持久 generation 与提交检查 |
| R16 | 批量失败不保留半条事实转换/半批新增；失败恢复前不开放部分状态 | `services/rag.py:231`；`tests/test_rag.py:99,110`；`test_rag_governance.py:163,175,187` | Store 原子可靠提交及恢复状态机 |
| R17 | 大清理分批提交、失败保留已删批次并允许重试；不会把成功批次恢复回来 | `services/rag.py:443`；`RAG.md` 持久化段 | 持久删除任务及幂等重放 |
| R18 | 1000 owner 共用账户集合，不各开模型进程/向量集合 | `tests/test_rag_governance.py:132` | 集合共享与有界缓存 |
| R19 | list 按 ID 升序 after 游标；默认包含失效版本；有 as_of 时按区间过滤；不读取整库向量 | `services/rag.py:398`；`tests/test_rag_governance.py:148` | 有序目录索引 |
| R20 | search 输出 `similarity` 为余弦相似度 [-1,1]，min_similarity 同义；非模型置信度 | `services/rag.py:58,463`；`tests/test_rag.py:95` | 明确分数字段 |
| R21 | 过滤注入不能扩大账户/空间/归属；输入不进入可执行过滤字符串 | `tests/test_rag.py:77` | 原生结构化过滤 AST |
| R22 | 模型身份/清单缺失或不匹配必须拒绝打开，不能建空库、重标向量或因维度相同就接受 | `services/rag.py:130`；`tests/test_rag_governance.py:231,245` | 集合契约与显式 create/open |
| R23 | 历史需求为旧格式迁移；用户后续明确取消迁移，本轮不开发、不删除真实旧库 | `services/rag.py:130`；`tests/test_rag_governance.py:222,259` | 已移出本轮范围，采用明确新根目录 |
| R24 | 模型懒加载；auto 的设备故障可回 CPU，强制 cuda 不静默回退；坏输入不触发设备切换 | `services/ai/embedding_provider.py:91,197`；`tests/test_rag.py:142,149,155` | Provider 层 |
| R25 | 模型权重 SHA 校验；非有限、维度错、零向量拒绝；大数归一化避免溢出 | `services/ai/embedding_provider.py:77,175`；`tests/test_rag.py:163,174` | Provider + 原生防御验证 |
| R26 | 保留 HTTP /rag、管理页、search_memory 工具的记录字段/别名/游标/错误响应；status/warmup 可用 | `api/v1/rag.py`；`services/ai/rag_tool.py`；`frontend/src/pages/rag.js` | Python 兼容门面 |

## 3. 游戏接入必须保留的约束

| 编号 | 需求 | 真实落点 / 验收线索 |
| --- | --- | --- |
| G01 | 角色结算、权威事件、观察记录和 outbox 在同一 SQLite 事务；不从自由生成正文提取数值事实 | `game/events.py:104`；`game/npc_memory.py:68`；`tests/test_game.py:217` |
| G02 | RAG 只消费已提交事件；索引故障保持 pending，稳定 event 来源支持部分成功后重试 | `game/events.py:163`；`tests/test_game.py:868` |
| G03 | scope 为 `game:<world UUID>`，owner 为 `gm` 或 `npc:<稳定 key>`；同名世界/人物不复用 | `game/events.py:163,221`；`tests/test_game.py:883,901` |
| G04 | NPC 只索引自身获知的观察投影；audience、actor 和 observation 共同校验；墓碑 NPC 不重新索引 | `game/events.py:183`；`game/npc_memory.py:68`；`tests/test_npcs.py:110` |
| G05 | 私有生活不生成 GM 记忆；旁观者不获得完整玩家状态/物品、私有运气、内部授权或未来分支 | `game/events.py:93,104,183`；`game/npc_memory.py:68`；项目 `AGENTS.md` |
| G06 | recall 再检查 event 仍存在、NPC 仍有 observation、没有被清除时间挡住；不能只信外部索引 | `game/events.py:221`；`tests/test_npcs.py:383` |
| G07 | 删除世界/NPC 与索引共享世界锁；失败清理持久排队；重建同名角色仍清原 world | `game/events.py:23,247,261`；`game/npc_cleanup.py:46,134`；`tests/test_game.py:901` |
| G08 | 个人记忆清理不移除能力、道具、有效承诺；整人清理有墓碑与领域级联，不能用 RAG 重建旧 ID | `game/npc_cleanup.py:46`；`NPC_SYSTEM.md`；`tests/test_npcs.py:123,252` |
| G09 | RAG 事件时间按游戏小时；NPC observation/cutoff 使用游戏秒；二者禁止直接比较 | `game/events.py:104,221`；`game/npc_memory.py:68,115` |
| G10 | scene 保留独立事件与记忆字符预算、近期完整事件按 source 去重、当前关注按结构化实体筛选 | `game/scene.py:55`；`game/engagement.py:145,155`；`tests/test_game.py:779` |
| G11 | 游戏工具不开放可任意指定归属的 search_memory；精确读事件走已授权 game_read_events | `RAG.md`；项目 `AGENTS.md`；`services/ai/rag_tool.py` |
| G12 | 规则只有 active 可进入正文，always 常驻、其余按 ID 查；RAG 历史不授予已遗忘能力 | 项目 `AGENTS.md`；`game/scene.py:52` |

通用底座负责 R01–R25 中除已取消 R23 外的存储/检索不变量，R26 和 G01–G12 由兼容适配与游戏接入验收共同负责。当前测试映射见 [ACCEPTANCE.md](ACCEPTANCE.md)；游戏业务验收不能由 SDK 测试代替。

## 4. 现有接口形状

兼容门面保留以下签名，接受 WordGame 的 Pydantic 模型但不把这些类型引入原生层：

```python
MemoryStore(root=None)
put(account_id, items, *, record_id=None) -> list[record]
import_text(account_id, item) -> list[record]
list(account_id, selector) -> {"items": [...], "next_after": "..."}
history(account_id, record_id, *, after_revision=0, limit=50)
    -> {"items": [...], "next_revision": int | None}
get(account_id, record_id) -> record
delete(account_id, record_id) -> None
delete_matching(account_id, selector) -> {"deleted": int}
search(account_id, request) -> list[record]
stats(account_id) -> {"records": int, "storage": "fengqudb"}
close() -> None
```

对外记录保留 `id, text, title, scope, owner_id, npc_id, kind, known_at, expires_at, source_id, source_uri, source_revision, fact_key, content_hash, lineage_id, supersedes, revision, edit_count, is_latest, created_at, updated_at`；search 才附 `similarity`。内部 history payload 和向量不进入普通回包。

现有 HTTP 限制：单条文本 1800 字符，import 60000；search top_k 1–30；list/history limit 1–100；HTTP batch 1–16。`MemoryStore.put` 本身可接更大列表，当前 1000 owner 测试直接调用它；新门面不得误把 HTTP batch 限制套到所有 SDK 调用。

管理 API 的 get/history 是当前登录账户的管理能力，不能直接暴露给 NPC 自主调用。新通用 SDK 的受限 namespace 句柄与 tenant 管理句柄必须分开。

## 5. 明确区分兼容与新增

| 项目 | 首期兼容 | 显式新增能力 |
| --- | --- | --- |
| 模型 | 复用 KaLM 既有 IDENTITY、896 维和查询指令，新库不迁入旧数据 | 完整模型/查询指令指纹、其他 Provider |
| 检索 | dense cosine；保持旧阈值语义 | exact/HNSW 选择、lexical/BM25、RRF 混合、可选重排 |
| 分段 | 固定 legacy 配置和重复导入身份 | 可版本化 chunker、结构化来源区间 |
| 来源 | source_revision 仅追溯标签，不自动替换全文 | 显式 replace_source 暂存并原子发布 |
| 删除 | 谱系删除与精确来源删除，成功回包同步完成逻辑清理 | 持久异步任务、namespace retire、物理回收状态 |
| 时间 | 旧 API as_of 默认 0，原单位与数值不变 | namespace 时钟声明、未来分叉的独立 timeline |
| 文档接入 | 应用提供文本；事件已由业务投影 | 文件提取器插件、结构化元数据、来源引用和上下文装配 |

新增严格模式不能暗中改变旧资料的去重/修订号或时间语义。新时钟、新 timeline、混合评分和来源同步分别显式启用。

## 6. 本轮证据边界

本文件初始为只读需求审计。后续已在隔离临时库运行真实 Pydantic/HTTP 路由测试及既有 KaLM 推理，见 [ACCEPTANCE.md](ACCEPTANCE.md)；未改游戏源码、扫描玩家原文或访问业务数据库。完整游戏测试仍未执行。历史 smoke 注释与当前配置不一致时，以实际 Provider 为准。
