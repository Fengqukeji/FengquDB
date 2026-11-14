# FengquWiki 0.2 底座与接入契约

工作树 `D:/study/FengquDB-llmwiki`，分支 `codex/llmwiki`。本轮修改未提交、未推送；没有修改原 `D:/study/FengquDB` 的 SDK 工作。本文件描述当前 schema 2，取代 0.1 文档中相应的能力限制。旧 namespace 会明确拒绝打开，请使用新的 namespace 或新的数据库目录；不提供旧数据迁移、迁移工具或模型训练。

## 设计依据与维护成本

沿用 [Karpathy LLM Wiki](https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f) 的原始材料/编译知识分层、[DeepWiki-Open](https://github.com/AsyncFuncAI/deepwiki-open) 的相互关联页面，以及 [LightRAG](https://github.com/HKUDS/LightRAG) 的增量更新和来源追踪思路。没有引入其框架或复用其性能结论。

运行依赖为 Python 标准库、FengquDB Core 和共享 NativeIndex。默认直接使用共享 NativeIndex，缺库明确报错。穷尽对照仅在 tests/reference_wiki_index.py，由测试显式注入，不进入 SDK。页面、原文、事实、依赖、任务和索引修改使用同一个 Store 事务；模型调用在事务外执行。不需要另一套数据库、消息队列、向量服务或图服务。模型可以是兼容 chat/completions 的本地或远程服务，也可以直接注入业务对象。

## 可运行示例

```powershell
$env:FENGQUDB_LIBRARY='D:\study\FengquDB-index\build\index-win\Release\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY='D:\study\FengquDB-index\build\index-win\Release\fengqudb_index.dll'
python examples/llmwiki/foundation.py
python -m llmwiki --db build/wiki-v2 --namespace demo-v2 --index-provider native ingest-dir examples/llmwiki --uri-prefix docs:examples
python -m llmwiki --db build/wiki-v2 --namespace demo-v2 --index-provider native compile --workers 2 --limit 100
python -m llmwiki --db build/wiki-v2 --namespace demo-v2 --index-provider native serve
```

示例默认使用临时数据库，输出实体、冲突处理和带引用问答。CLI 和 HTTP 复用同一 Python API。[foundation.http](../examples/llmwiki/foundation.http) 是 HTTP 请求模板。旧 ZIP 对应旧提交，本轮没有将旧 ZIP 标记为新版本。

```python
from llmwiki import Wiki, NativeIndexProvider, Limits

with Wiki("wiki-v2", namespace="knowledge-v2", index_provider=NativeIndexProvider(),
          limits=Limits(batch_items=32, workers=4), compiler=my_compiler,
          answerer=my_answerer) as wiki:
    uploaded = wiki.ingest_many([
        {"uri": "docs:one", "title": "Storage", "content": "Supports reliable transactions."},
        {"uri": "docs:two", "title": "Snapshots", "content": "Reads use consistent snapshots."},
    ], request_key="import-2026-11-14")
    wiki.define_page("storage-guide", "Storage guide", [s["id"] for s in uploaded["items"]])
    print(wiki.run_jobs(limit=100, workers=2, timeout_ms=60000))
    print(wiki.ask("How are transactions read?"))
```

## 组合 SDK 的单文件夹导入

组合包目录由主工作树统一组装：`FengquDB/llmwiki`、`FengquDB/bindings/python` 与 `FengquDB/native/{win|linux}-{x64|arm64|x86}`。复制 `FengquDB` 文件夹后，可以直接：

```python
from FengquDB.llmwiki import Wiki, NativeIndexProvider

with Wiki("wiki-v2", index_provider=NativeIndexProvider()) as wiki:
    wiki.ingest("docs:sample", "Knowledge uses reliable transactions.")
    wiki.run_jobs(limit=10)
    print(wiki.ask("transactions"))
```

`python -m FengquDB.llmwiki --db wiki-v2 --index-provider native search transactions` 也可运行。Wiki 优先使用包内相对导入，独立源码和原单模块交付保留回退；运行库不修改 sys.path 或注入 sys.modules 别名。公开 Core resolver `native_library_path(stem)` 来自拥有方，自动查找包内平台目录。只有默认自动找到同一目录中的配套 Core/Index 时，Index 先加载并保留 Core 引用；显式 library 参数和环境变量覆盖路径保持优先。Linux 使用 libfengqudb.so.2 / libfengqudb_index.so.1 或对应无后缀名称。

Windows 和 Linux 的隔离子进程已验证该布局的 import、真实 NativeIndex 摄入/问答以及模块 CLI；测试清除 PYTHONPATH、原生库环境变量和 LD_LIBRARY_PATH，且只复制一个包文件夹。组合 SDK 本身由主工作树发布，本轮只交付这些兼容入口。

## 来源、批量与版本

`ingest(uri, content, title=None, annotations=None, expected_version=None, expected_revision=None)` 和 `ingest_many(items, request_key=None)` 使用相同逻辑。批次在一个可靠事务内完成，任意项失败会整体回滚；重复 URI 被拒绝。更新和删除都推进单调 `version`，删除后恢复相同原文也会取得新版本。`expected_version=0` 表示只允许首次创建。`expected_revision` 可以同时检查内容版本，但防止“改了又改回来”的版本混淆应使用 `expected_version`。

`request_key` 以请求摘要保存结果。同键同输入返回原结果，不重复执行；同键不同输入返回冲突。重放返回当时结果，调用者如需当前状态应重新读取来源。幂等记录与来源历史保留在数据库中，不会按 TTL 静默清除。

`delete_source(id, expected_version=None, expected_revision=None)` 和 `delete_sources([{id,expected_version,...}], request_key=None)` 立即撤销检索文档、事实和依赖页可见性，保留原文及历史。`source(id, revision=None, version=None)` 可以读取不可变原文和来源生命周期版本；修订哈希包括标题、正文与 annotations。`current_revision` / `current_version` 始终标明当前指针。

## 实体、事实与冲突

来源可以直接提供结构化 annotations，不必调用模型：

```json
{
  "entities": [{"key":"db","type":"product","name":"FengquDB","aliases":["风趣数据库"]}],
  "claims": [{"entity":"db","predicate":"timeout_seconds","value":30,"source":"S1","quote":"30秒"}]
}
```

`quote` 必须是该来源的精确子串。重复子串必须提供 `start`。引用范围使用从 0 开始的 Unicode 字符位置，绑定完整来源 revision 和单调 source_version。实体 ID 由 NFKC 规范化后的 type/name 计算；别名按当前来源合并，不自动猜测同名异人或别名等价。

模型也可在普通 `compile(title,sources,links)` 的一次调用中返回 `entities` / `claims`，格式同上。只有来源页的抽取会写入知识图；实体页/主题页不会把生成文本再次当作原始事实。抽取式离线 provider 不做语义抽取，只有显式 annotations 才产生实体事实。

相同实体和 predicate 的不同标量值形成显式 conflict，所有竞争证据保留。冲突不是自动判错：数值不等可能来自时间、条件或材料差异，调用者应选择合适 predicate 和实体范围。`resolve_conflict(id, claim_ids, expected_generation, note, actor)` 使用 CAS 保存人工决定，不修改原文或删除竞争事实。证据集合变化后清除旧决定并推进 generation。`conflict(id,generation=N)` 可读取旧决定及其完整历史事实；`entity(id)` 返回当前事实和活动冲突。

来源更新立即撤销旧事实；实体来源边暂时保留，实体页进入 stale / waiting_sources。来源抽取完成后重新调度实体。真正删除或新抽取不再包含实体时才撤销关联；最后一个来源移除后实体退役。实体页附加确定性事实/冲突部分，使用普通编译、发布和共享索引路径。

## 人工修订与问答一致性

`revise_page(id, markdown, expected_revision, expected_version, actor, links=None, expected_source_versions=None)` 保存不可变人工修订并固定为 human 模式。Markdown 的 `[S1]` 和允许的 Wiki links 仍须通过校验。页面已经 stale 时，必须先查阅当前原文，再提供精确 `{source_id:version}`；否则拒绝将旧编辑内容重绑到新原文。

来源变动将人工页面标为 stale / review_required，自动编译不会覆盖。人工来源页仍可执行知识抽取，供实体页更新。显式 `release_page(id, expected_revision, expected_version, actor)` 才恢复 compiled 模式。actor 是审计标签，不是身份认证或权限系统。

问答读取同一快照下的检索证据，检查答案中的 `[E1]` 引用，并在模型返回后重新核对来源版本、索引投影、页面可见性及知识 epoch。若发生变化，返回 `answer=""`、`evidence=[]`、`citation_validated=false`、`stale=true`、`retryable=true`。`citation_validated=true` 表示标签和版本有效，不代表模型的每一句话都已经被语义证明。

问答还会沿被引用来源查询当前冲突，即使召回的只有原文块；最多检查32个来源/64个实体/16个冲突，单冲突最多32条事实，超出上下文或此边界时标记 conflict_context_truncated。

answer cache 是进程内 LRU，默认 64 项/2 MiB/60 秒，缓存命中也要经过最终版本校验。编译 artifact 保存在 Store，指纹包含来源版本、页面规格、知识上下文、provider identity 和 prompt 版本。模型网络调用遵循至少执行一次语义，响应落库前崩溃仍可能再次收费。

## 共享 NativeIndex

`search(query, ...)` 支持 `filters={kind/source_id/page_id/publication_state: 字符串或字符串数组}`、`phrases`、`prefix_last`、`match_all_terms`、`excluded_terms`、`scoring_fields`、`statistics_policy=1`、位置、解释和检索预算。excluded_terms 按共享 Analyzer 分词后排除任一命中的词，原生限制合计32词；scoring_fields 最多32个字段名。可在 Python 中直接传公开 `Predicate` 作为 `where`；适配器始终与 mandatory visible=true 组合。HTTP 使用 `POST /v1/search`，业务过滤通过 filters 表达。

`include_positions=true` 的命中位置按 UTF-8 字节计数，与领域引用的 Unicode 字符范围区分。适配器用同版本领域正文核对位置和 snippet，返回 JSON 可序列化文本。评分解释与 terms/statistics/blocks/graph/layout 计数透传共享 Index。

上层可用任意 embedding 模型在事务外计算向量，再调用：

```python
hits = wiki.search("transactions", filters={"kind":"chunk"})["results"]
doc = hits[0]
wiki.set_vectors([{"id": doc["id"], "vector": embedding,
                   "expected_projection_version": doc["projection_version"]}],
                 vector_space="my-model@revision:dimension")
# HNSW preparation is explicit and resumable; normal writes retain FP32 only.
for _ in range(64):
    prepared = wiki.maintain_index(operation=2, vector_space="my-model@revision:dimension", max_records=128)
    if prepared["finished"]:
        break
wiki.search("", mode=2, vector_f32_le=query_embedding,
            vector_space="my-model@revision:dimension", ef_search=96)
```

`mode=1` 是精确余弦，`mode=2` 请求 HNSW 导航。普通向量写入默认只维护 FP32 和统计，不承担 HNSW 维护成本；显式 op2 影子构建完成后才启用该空间的图导航及后续增量维护。未完成的构建可在下一轮按持久 checkpoint 续跑，期间 mode2 精确回退。共享 Index 可以按集合规模、图覆盖或候选情况回退精确路径；结果 ranking_kind 反映实际路径。近似空结果不等于已确认没有匹配。源文或页面更新会使旧向量失效，需要上层重新提交。向量变更走完整 UPSERT，metadata PATCH 仅用于可见性等元数据；本项目没有另一套向量算法或模型依赖。

`maintain_index(operation=1/2/3/4, ...)` 分别调用共享 Index 的倒排整理、影子代次图重建、记录核验、旧图代次回收。返回 continuation cursor 的 bytes 字段使用 hex；继续时原样传回。op4 的 after_key/next_key 最多4096字节，reclaimed 返回回收键数；通常使用原生持久 checkpoint，无需调用者自行遍历图键。同一 partition 一次只允许一个图重建任务，另一个 vector_space 并发重建返回冲突；普通写入继续执行并双写 active/building。影子重建、并发写入和旧快照语义由共享 Index 实现，Wiki 不实现图算法。op1 达到 max_postings 时可正常返回 finished=false，next_term + next_id 表示词项内部续点；通常重复同一维护请求即可按持久 checkpoint 继续，显式 after_id 必须对应已保存边界。内存超限、取消、超时等维护错误仍取消整个事务。普通查询、维护都使用公共 lease 和配套 Core；不读取 Store 私有指针，不缓存裸指针。新查询能力在旧运行库上显式拒绝，不静默忽略字段。

查询默认 postings=12000、candidates=1024、scan=100000、memory=32 MiB、timeout=30秒。Wiki 原生适配器上界为 postings=100000、candidates=4096、scan=1000000、memory=512 MiB、timeout=300秒；内存是 Index 工作内存估计，不代表整个 Python 进程 RSS。终止原因包括 posting/candidate/scan/memory/deadline/cancelled/term_limit，返回结果保留独立 ranking、coverage、match_evidence 和 truncation。生产包没有 Python 检索 fallback。Native Analyzer 的已知归一化范围不是完整 Unicode NFKC/casefold，不能把测试 reference 的 Unicode 能力当作生产能力。

## 任务、配额与连接器

`run_jobs(limit,workers,timeout_ms,cancelled)` 有界领取当前到期任务。超时/取消停止新领取，已经进入同步模型调用的任务等调用返回或 provider 超时；它不是强制中断自定义 Python 回调的硬时限。每个运行任务都有持久令牌/代数和自动续租。进程退出留下的过期租约可被重新领取；旧 worker 不能覆盖新任务。`close()` 停止新 worker 工作并等待在途 compile/ask 收尾。

任务失败只持久化安全分类、恢复提示和重试次数。默认最多 3 次，指数退避。`retry(page_id)` 显式开始新一轮；人工页仍需 review/release。`runtime/job-counts` 事务更新状态计数，status 不扫描完成任务历史。waiting_sources / review_required 不进入热轮询队列。

| `Limits` 默认值 | 边界 |
| --- | --- |
| source_bytes / page_bytes | 256 KiB / 1 MiB |
| batch_items / batch_bytes | 64 / 2 MiB，整个 JSON 批次 |
| sources_per_page | 16，实体与主题相同 |
| entities_per_source / claims_per_source | 32 / 128 |
| compile_context_bytes / answer_bytes | 2 MiB / 256 KiB |
| workers / jobs_per_run | 8 / 1000 |
| answer cache | 64 项 / 2 MiB / 60 秒 |

`CompatibleProvider` 还有自身输入 100000 字符、输出 token、响应 2 MiB、超时和并发限制；最终按较严格边界拒绝请求。批量接入摊薄可靠事务开销，不会把超限内容静默裁掉。多模型使用 `compiler=` / `answerer=`，旧 `provider=` 仍作为两者默认；CLI 支持 `WIKI_COMPILER_MODEL_URL/MODEL/KEY` 与 `WIKI_ANSWERER_MODEL_URL/MODEL/KEY`。

`connectors.load_files(path,limits,uri_prefix=None)` 提供本地 UTF-8 文本/Markdown 接入。拒绝软链和 Windows reparse/junction，核对文件身份/修改时间，限制文件数、总字节、深度32和遍历节点数。它用于受控本地目录，没有后台监听；并发敌对文件系统不是可提供原子快照的安全边界。HTTP 不接受服务器本地路径，也不会根据来源 URI 抓取 URL。

## HTTP 路由

| 路由 | 行为 |
| --- | --- |
| `POST /v1/sources` | 单来源摄入及可选 CAS/annotations |
| `POST /v1/sources/batch`、`/delete-batch` | `{items,request_key}` 原子批次 |
| `GET /v1/sources/{id}?revision=...&version=...` | 原文与生命周期历史 |
| `POST /v1/pages` | 定义主题 |
| `GET /v1/pages/{id}?revision=...` | 页面与修订 |
| `POST /v1/pages/{id}/revise`、`/release` | 人工修订/恢复编译 |
| `GET /v1/entities`、`/claims`、`/conflicts` | limit/after 分页 |
| `GET /v1/entities/{id}` | 当前事实与冲突 |
| `GET /v1/conflicts/{id}?generation=N` | 当前或历史冲突及证据 |
| `POST /v1/conflicts/{id}/resolve` | CAS 决定 |
| `POST /v1/search`、`/ask` | 高级检索 / `{question,limit,context_chars,search:{...}}` |
| `POST /v1/vectors` | 投影 CAS 的预计算向量批次 |
| `POST /v1/jobs/run`、`/v1/jobs/{id}/retry` | 有界执行/恢复任务 |
| `POST /v1/index/maintain` | 公共索引维护 |

原有 health/status/list/lint/DELETE 来源路由保留。应用版本冲突返回 409，配额超限 413，参数错误 400，模型错误 502，存储错误 503。服务端只提供单 namespace 与共享 bearer token；多用户 ACL、前端、分布式部署、URL/仓库抓取连接器不在本轮交付内。

## 完成清单与实际验证

- [x] 独立分支，保留原工作树；本轮不 commit/push。
- [x] 单项/批量摄入与删除、CAS、幂等请求、不可变原文和来源生命周期历史。
- [x] 来源/主题/实体编译、按依赖失效、精确引用、结构化事实、冲突决定及历史证据。
- [x] 人工修订保护、明确 release、过期编辑来源版本校验。
- [x] 可恢复租约、续租、有界并发、重试分类、状态计数。
- [x] 可替换编译/回答模型、编译缓存、受限答案缓存、证据撤销时丢弃回答。
- [x] 真实共享 NativeIndex 的查询、过滤、位置、解释、ANN、向量发布和维护。
- [x] UTF-8 本地连接器、Python/CLI/HTTP API 和示例。
- [x] 未引入迁移工具、模型训练或另一套 C++ 检索算法；未启动其他业务接入。

测试与小样本数据见 [验证记录](LLMWIKI_FOUNDATION_VALIDATION.md)。协议 fixture、真实原生调用与模型质量分开计数；没有真实外部模型质量评测或未验证平台的运行承诺。
