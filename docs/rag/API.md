# 使用与接口

需要 Python 3.10+、同 ABI 的 Core/Index、公共 fengqudb.py、fengqudb_index.py、fengqudb_index_wire.py。RAG 无模型运行库依赖。当前测试的二进制和 wrapper 摘要见 build/rag-memory-verification.json。

组合 SDK 可直接使用 `from FengquDB.bindings.python.fengqudb_rag import MemoryStore`。RAG 优先使用同一组合包的 Core/Index/wire wrapper，不创建全局模块别名，也不修改进程搜索路径；动态库参数仍默认 None，由公共 wrapper 解析组合包 native 目录。组合发行物由主会话统一组装，不能把本工作树临时依赖路径当发布布局。

独立源码模式仍可将 bindings/python 和公共 wrapper 目录加入宿主 Python 路径；WordGame 门面另加入 integrations。此模式保持旧顶层导入。组合包依赖损坏不会由 RAG 静默回退至其他项目的同名模块。不要部署 build 下临时数据库。

```python
from fengqudb_rag import MemoryStore, MemoryWrite, MemorySearch, CallbackEmbedding, EmbeddingContract

# 回调自行执行 query=True 的指令，RAG 不重复加查询前缀。
embedding = CallbackEmbedding(existing_embed_callback,
    EmbeddingContract("model-weight-sha256-and-version", 896, pooling="mean",
                      query_transform="query-instruction-sha256"))
with MemoryStore("fresh-rag-data", embedding=embedding,
                 core_library=CORE_PATH, index_library=INDEX_PATH) as store:
    rows = store.put("account-1", [MemoryWrite(text="李山住在东村。",
        scope="world:uuid", owner_id="npc:1", source_id="event:42",
        known_at=8, fact_key="person:lishan:home")])
    request = MemorySearch(query="李山住哪里？", scope="world:uuid",
        owner_id="npc:1", include_public=False, as_of=10, mode="hybrid")
    result = store.retrieve("account-1", request)
    packed = store.context("account-1", request, max_chars=2000)
    npc = store.scoped("account-1", scope="world:uuid", owner_id="npc:1")
    npc.search({"query": "住址", "as_of": 10, "include_public": False})
```

## 方法

| 方法 | 语义 |
| --- | --- |
| put(account, items, record_id=None) | 原子批写；record_id 时单条纠错，expected_revision 做 CAS |
| import_text(account, item) | 1200 字符、100 重叠；默认 import:全文 SHA，拒绝 fact_key |
| get / list | 管理读取；list 返回 items/next_after，不读取整库向量 |
| history(account, id, after_revision=0, limit=50) | items/next_revision，全谱系严格递增 |
| delete(account, id) | 同步完成整谱系逻辑清理，中断保留屏障 |
| delete_matching(account, selector) | 精确 scope/owner/timeline，可限 source，返回 deleted |
| search(account, request) | 兼容记录列表，默认 exact cosine |
| retrieve(account, request) | 附快照、排名、完成性、证据、原生工作量 |
| context(account, request, **packing) | 附正文、引用、预算和跳过数 |
| retire_namespace / recover | 持久禁止 namespace 写入 / 恢复清理 |
| embedding_status / warmup | 外部模型契约、状态 / 显式预热 |
| stats / backup / close | 计数 / Store 备份 / 等待在途操作再关闭 |

接受 dataclass、dict、Pydantic model_dump。owner_id/npc_id 冲突报错；source_revision 保持字符串。timeline 默认 main，旧 HTTP 无需新增字段。普通记录附 timeline/projection_version/namespace_generation，旧字段保留。

dense similarity 为 [-1,1] 原生 cosine；lexical 无阈值时 similarity=None；hybrid 补算 cosine。score_kind 区分 cosine/rrf/reranker，不能把融合分数当相似度。candidate_k 默认64、最多256；top_k 最多30。

重排回调：reranker(query, authorized_rows) -> [id 或 (id, score)]，禁止未知/重复 ID 或非有限分数。上下文支持 max_chars、max_tokens、token_counter、exclude_sources、predicate，回调后仍复核当前版本。JSON 引用边界不能替代宿主的提示词安全策略。

## WordGame 注入

见 examples/rag/wordgame_adapter.py。组合包使用 `from FengquDB.integrations.wordgame_rag import create_store`，门面相对引用同 SDK 的 bindings.python.fengqudb_rag；独立源码仍支持顶层 wordgame_rag。create_store 接受现有 embed_with_provider、IDENTITY、DIMENSION、查询指令摘要、status/warmup 和 AppError；不导入游戏、不重复模型、不管理 Provider 关闭。

root 显式采用新目录；本包不迁移/删除旧 zvec。SQLite/outbox 和完整游戏回归由之后接入负责。隔离测试入口：

```powershell
D:/work/WordGame/.venv/Scripts/python.exe tests/rag/test_wordgame_adapter.py --core CORE_DLL --index INDEX_DLL --shared-python SHARED_WRAPPERS --wordgame D:/work/WordGame --real-model --output build/rag-wordgame-verification.json -v
```

--real-model 仅使用既有模型和运行库，日志写本工作树 build，不打开业务库。Linux 的真实 KaLM 推理未执行。

## 预算和错误

默认账户句柄8、写批次1024、总文本4MiB、账户原生 cache/delta 各16MiB、模型缓存4MiB。store_options 可配置原生预算，不能覆盖 library/create/name；超限整批拒绝，不自动拆写。这不是进程总 RSS 上限。

QueryBudget 控制 timeout、posting、candidate、scan、原生 memory、结果字节。MemoryStore 默认 candidate100000、scan250000；owner/public 和 hybrid 合计扣减。模型调用与可靠提交不支持任意抢占，只在边界检查取消/超时；清理超时可能留下已可靠保存的屏障，recover/reopen 继续。

RagConflict：版本/身份/生命周期冲突；RagNotFound：不存在或屏蔽；RagStaleRead：重新检索；RagLimit/RagDeadline/RagCancelled：显式超限/截止/取消；RagIntegrityError：数据/契约异常；RagRecoveryRequired：关闭并重开。门面可转换为宿主 AppError。直接写保留键空间不受 SDK 并发与权限保证。
