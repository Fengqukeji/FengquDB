# FengquWiki 与公共 FengquIndex 的对接契约

日期：2026-11-14。作用域：`D:/study/FengquDB-llmwiki` / `codex/llmwiki`。本文件供 FengquIndex、FengquRAG 与 Wiki 协作对齐，接口字段为逻辑契约，不定义公共 C ABI 的结构体布局。

共同词汇以 [FOUNDATION_COORDINATION.md](D:/study/FengquDB-index/docs/FOUNDATION_COORDINATION.md) 为准。共同数据库基线仍为 `32348fb`；本轮只改 Wiki 工作树，不实现公共 C++ 检索或启动模型训练。

## 已落地的边界

`llmwiki/index_provider.py` 定义 `IndexProvider`、`IndexDefinition` 和 `PythonIndexProvider`。`Wiki(..., provider=...)` 仍是模型接口；新增 `Wiki(..., index_provider=...)` 才是检索接口，两者互不绑定。

```python
from llmwiki import Wiki, PythonIndexProvider

with Wiki("wiki-data", library="/path/to/libfengqudb.so",
          index_provider=PythonIndexProvider()) as wiki:
    print(wiki.search("数据库事务"))
```

默认适配器直接调用已有 `retrieval.py`，保留 Python BM25、中文单字/双字分词、查询预算及结果作为当前实现和后续对照；没有新增 C++ 倒排、HNSW、embedding 或检索框架依赖。现有 HTTP/CLI 行为保持兼容。

| 当前 Provider 方法 | 必须维持的语义 |
| --- | --- |
| `upsert(tx, document_id, document)` | 在调用者事务中替换文档及派生索引，重复执行幂等 |
| `delete(tx, document_id)` | 在调用者事务中删除可检索文档，缺失文档为无操作 |
| `set_page_state(tx, page_id, state)` | 镜像 Wiki 作出的可见性决定；`{revision, stale}`，`None` 表示退役 |
| `get_page_state(view, page_id)` | 在指定快照读取页面可见性，用于问答响应后的版本复核 |
| `get_document(view, document_id)` | 在指定快照读取文档元信息，至少返回 revision；不要求加载正文 |
| `statistics(view)` | 在指定快照返回 `count`、`length` |
| `search(view, query, limit, posting_budget, candidate_budget)` | 在指定快照返回现有结果结构并排除 stale 页 |

这些方法不自行打开、提交或关闭数据库。`View` 是 `storage.py` 对调用者已有 Store transaction/snapshot 的逻辑分区包装，`prefix` 已包含 Wiki namespace。方法返回前须完成需要的拷贝，不能把借用的原生视图保存到下一次调用。

当前仅接收 `atomic_updates=True` 的实现；异步旁路服务不能通过设置这个布尔值来获得事务保证。该属性是受信任实现的声明，不是自动证明。`index/provider` 持久保存 `{name, format_version, tokenizer_version}`，启动时拒绝静默更换格式。旧 0.1 非空 Python 索引可以被默认适配器认领；其他实现需要显式迁移或新 namespace。NativeIndexProvider 已实现，运行方式与协议见 [原生接入](LLMWIKI_NATIVE.md)；仍无自动迁移器。

Python 对照没有结构化过滤、向量参数或精确水位。Native 包装已支持 v1 结构化谓词、get/list/stats、精确余弦与公开快照版本/索引序列；Wiki NativeProvider 默认仍用词法，并强制过滤 visible。异步水位、ANN、严格动态历史统计和自动迁移仍未实现。

## 职责与记录映射

Wiki 继续拥有来源摄入、不可变原文、分块边界、编译规格、LLM 提示词/缓存、主题页、依赖传播、引用、任务租约、重试、历史和导出。FengquIndex 统一拥有词法及字段过滤索引、评分、候选定位、可选向量/ANN、水位和重建。FengquRAG 可使用同一原生接口组织召回和上下文；不得在 Wiki/RAG 各维护一套原生检索算法。

| 公共记录字段 | Wiki 的映射 |
| --- | --- |
| scope | 当前 Wiki namespace 的稳定分区标识；不是权限凭据 |
| collection | `raw_chunks` 或 `wiki_pages` |
| id | 原文块 `c-<source-id>-<ordinal>`；页 `p-<page-id>`，保持稳定 |
| domain_revision | 当前文档的 `revision`：原文块使用来源 SHA-256 修订，页使用编译指纹 |
| source_revision | 原文块来源修订；页面保留每条引用各自的来源修订 |
| projection_version | Native 适配层同事务保存的独立单调投影版本，传 expected/new version |
| lifecycle_generation | Native 适配层保留删除墓碑，删后重建推进代次；不与普通正文修订混用 |
| title / body | 来源标题及原文块，或主题标题及编译 Markdown |
| metadata | kind、source_id/page_id、来源修订、页可见性；未来业务过滤字段另设有类型的 schema |
| sources | Wiki 提供的引用列表：source_id、revision、URI、标题和 `[start,end)` |
| deleted | 当前检索投影的删除状态；不可变原文历史仍由 Wiki 保留 |

文档 revision、来源 revision、数据库 commit version 与异步索引水位是不同概念。页的编译指纹不能代替每条引用的原文版本；URI 和时间戳也不能代替版本保护。一个页可能引用多个来源，不能只留下第一条来源。

### 投影版本与页面发布状态的具体要求

公共端口覆盖 upsert、delete、页面状态镜像、元信息读取、检索和统计。Python 对照不具有版本化 CAS；Native 适配器现已在域事务中维护 projection_version/lifecycle_generation，并使用原生 expected_projection_version 的 upsert/patch/erase。

| Wiki 变化 | domain_revision | 必须推进的投影/领域状态 |
| --- | --- | --- |
| 来源正文或标题更新 | 来源 SHA-256 修订变化 | 受影响 chunk 的 projection_version、来源发布版本及所有依赖页状态投影 |
| 已发布页 ready → dirty | 页正文指纹可以不变 | page 的 projection_version，保留原 publication_generation 并变更状态 |
| dirty 页再次被新资料作废 | 页正文指纹仍可不变 | 若编译目标/来源发布集合改变，推进状态投影；不能因 stale 已经为 true 就忽略新目标 |
| 新结果发布为 ready | 指纹可能变化；相同输入缓存再发布时也可能不变 | 新 publication_generation 与 projection_version，绑定此次来源/规格快照 |
| chunk 所属发布版本变化 | 块正文或来源哈希可相同 | 新的所属发布标识与 projection_version，候选和物化阶段都核对 |
| 删除/退役、删后重建、清空 | 旧哈希可能在日后再次出现 | 推进 projection_version，保留最小墓碑和 lifecycle_generation；旧请求不可复活新代次 |

`job.generation` 是编译调度代数，`spec.generation` 是编译规格代数，二者都不能直接充当 publication_generation 或 projection_version。现有任务 token 能拦截旧模型响应，但不是公共索引后台任务的生命周期屏障。

Native 适配器在 `native-index/projection/`、`native-index/document/`、`native-index/page/` 保存权威投影状态，向原生接口传完整身份、期望/新版本、输入摘要及来源定位。相同投影版本且摘要不同为冲突，旧版本 update/erase 不得覆盖新投影。普通更新不先 erase；已 dirty 但 build 目标变化会 metadata patch；缓存再发布也增加 publication_generation。v1 uint64 revision 使用稳定整数 token 映射，完整 Wiki SHA-256 不截断，保留于域元信息、locator 和原生 metadata。

`index_space` 由 Provider 的格式、Analyzer、统计分区决定。NativeIndexProvider 的格式身份包含 index_space、显式 partition 配置、wire version 和 Analyzer 的摘要；默认 partition 从 Wiki namespace 前缀派生。未来同 namespace 下若加入不可互见的资料范围，还必须细分获授权统计分区，不能通过 DF、总量、Explain 或分数泄露其他私有范围。外部填写的 scope/owner 字符串不是授权证明。

权威 raw/page 始终由 Wiki 持有。Python 对照的 `body/` 是可重建正文副本；公共原生索引不必再持有一份完整原文，可返回域原件定位引用并在同一快照经域回调物化。不能把旧 Python 物理键布局变成公共索引的强制存储规范。

当前 Wiki 的原文引用范围是 **Unicode code point 偏移**；公共索引计划使用 **UTF-8 原文字节偏移**。适配层必须明确转换：原文块命中字节范围先在未归一化块正文中验证 UTF-8 边界，再转换字符偏移，并加上块在原始来源中的 `start`。NFKC/casefold 后的位置不能直接切原文。Markdown 页内命中只能直接指向页的正文修订，不能据此伪造对底层原文逐字精确的引用。

## 原子更新与删除

1. 来源摄入：旧原文块删除、新原文块与倒排/过滤投影写入、当前来源指针、依赖页 stale 状态和编译任务必须在同一个 Store transaction 提交。
2. 页发布：页当前值、历史、引用/反向链接、页面可见性和页检索投影在同一个 transaction 提交。LLM 调用在事务外，返回后 Wiki 重新核对来源修订、规格和任务令牌。
3. 来源删除：原文块立刻不可检索，所有依赖页同时 stale；后台重编译不能成为隐藏旧知识的前置条件。没有可用来源时，页面和检索投影退役。
4. `set_page_state` 必须进入原生索引的可见性过滤，不能只修改一个 Python 私有状态表。Wiki 决定 stale，索引执行这个决定，不重新推断知识是否过期。
5. Provider 不得提前发布不可回滚的内存索引状态。`Repository.write` 遇到 CONFLICT 会重放操作；解析、模型推理和外部服务调用不能藏在这些方法内部。COMMIT_UNKNOWN 不作盲目提交重试。
6. 超出事务/内存预算时保持旧状态并报告限额。若未来把大操作拆成批次，必须通过完整的 staging generation 和原子切换保证可见性，不能让查询读到一半替换的来源。

### 同 Store C ABI 的必要接点

原生 FengquIndex 应接受同一个 `fqdb_store_txn*` / `fqdb_store_snapshot*` 的受控借用或由统一 session/transaction bridge 创建的上下文。它不能为同一路径再开一个 Store，也不能另造 WAL、关闭独占锁或独立提交派生索引。

当前已同步 RAG/Index 拥有方的公开 `native_identity()` / `borrow_native()`，消费者只使用 lease.pointer/identity/library。包装核对 Core bridge 函数地址、ABI、handle kind 和 Store token，持租约到结果释放后，不强转私有 ctypes 字段。共享 Core/Index C++ 与根构建未由 Wiki 修改；旧 Core 不能因此获得桥接，格式迁移仍待实现。

## 过滤契约

公共搜索需要有类型的等值、集合、范围、存在性及 AND/OR/NOT 组合过滤。scope/collection、kind、source_id/page_id、revision、deleted/stale 是 Wiki 最小需求。模型/向量空间身份另作强制匹配条件，不能作为可以随意忽略的打分提示。

过滤和可见性应在进入最终 Top K 前参与候选选择。强过滤不能只在未过滤的 K 条候选中事后删去不适用记录；需要按过滤候选精确评分或继续有界遍历。若预算耗尽，返回部分结果与具体原因。即使结果不足 K，只要确实耗尽全部符合条件的候选，也可报告完整。

未知字段、未知运算符或未启用的能力必须明确返回不支持，不能悄悄忽略。权限/租户过滤由宿主授权策略决定，用户给出的过滤不能覆盖它。namespace 自身不提供 ACL，当前单 token HTTP 服务也没有细粒度权限能力。

Python 对照的 `search` 不支持上述结构化表达式；接入后由 Provider/适配层声明能力，未实现时拒绝带过滤的请求，不把忽略过滤的词法结果伪装成完整结果。

## 快照、修订与水位

### 同步词法与过滤

文档正文/元信息、倒排、DF/文档长度统计、过滤字段、删除与 stale 状态必须从同一个提交快照读取。查询开始后不得临时读取 Store 当前值“补全”命中，因为那会混合两个修订。返回的来源修订要能通过 Wiki 的历史读取接口解析。

Python 对照仍不把 `observed_database_version` 当作快照或水位。Native 模式通过公开 bridge 和原生 Result 取得 snapshot_version、mutation_sequence，再映射 applied_sequence；两种版本域不直接比较。事务视图版本是捕获的基准版本，不是未来提交版本。

### 可选异步派生索引

建议水位按 `index_space` 和重建 generation 分区，以领域的 `mutation_sequence` 为目标、连续完成的 `applied_sequence` 为实际值：

- 每次影响检索内容、删除状态或页面可见性的提交推进同一 scope 的 `mutation_sequence`；领取任务、日志查询等无关行为不应伪装成内容更新。
- `generation` 在重建/格式切换时更换。旧 generation 的游标、租约和水位不能自动解释成新 generation 的已完成工作。
- `applied_sequence` 表示该 generation 中**此前所有变更**均已处理的连续水位，不能取“曾经处理的最大序号”掩盖缺口。向量还须携带 model-space 身份及自己的连续水位。
- 原文/页写入与 durable outbox 事件同事务提交。worker 重启按检查点幂等重放；通知只用来唤醒，不能成为唯一恢复来源。
- 后台向量/索引结果同时校验来源修订、mutation_sequence 和删除/重建代次。资料删后以相同正文重新出现时，内容哈希可能相同，旧结果也不能绕过删除代次保护。

强一致请求携带最低目标水位或指定快照：索引等待、检索尚未进入主索引的有界差量，或返回 `INDEX_NOT_READY`。最终一致请求可以允许滞后，但必须携带 `stale=true`、实际水位和未追平原因。历史快照超出索引保留窗口时明确拒绝，不能返回当前索引的内容冒充历史结果。

当前 Wiki Provider 只支持同步同事务模式；不能用 `atomic_updates=True` 接入一个“稍后大概追平”的 sidecar。异步模式需要先增加 outbox、水位、等待/取消和读策略接口，再修改能力声明与适配层。

## 查询返回与截断

当前兼容结果必须保留：

```json
{
  "results": [{
    "id": "c-<source-id>-0000",
    "kind": "chunk",
    "revision": "<source-revision>",
    "source_id": "<source-id>",
    "title": "资料标题",
    "score": 1.0,
    "text": "有界片段",
    "text_truncated": false,
    "citations": []
  }],
  "truncated": false,
  "postings_visited": 2,
  "candidates": 1,
  "ranking_kind": "exact",
  "termination": "finished",
  "index_coverage": "complete",
  "snapshot_version": null,
  "applied_sequence": null,
  "total_hits": {"value": 1, "relation": "exact"},
  "degraded_reasons": [],
  "empty_result_confirmed": false
}
```

`citations` 实际使用时必须是 Wiki 摄入/发布时提供的有效引用。页命中带 page_id，原文块带 source_id。返回纯复制值，不能把 C++ 借用视图泄露到已关闭快照后。

本轮 Python 实现和 Wiki 最终 search/ask 回包已采用共同字段：

| 字段 | 本轮实际含义 |
| --- | --- |
| `ranking_kind` | Python 穷尽已请求词项与候选时为 exact；预算裁剪后为 approximate。未来 ANN 可以 approximate 且正常 finished |
| `termination` | finished / candidate_limit / posting_limit / query_term_limit；多个限制保存在 degraded_reasons |
| `index_coverage` | 默认 Python 同事务索引为 complete；其他 Provider 未提供时为 unknown，不从空结果推断 |
| `snapshot_version`、`applied_sequence` | Python 为 null；Native 从同视图原生结果映射，没有使用 observed_database_version 冒充 |
| `total_hits` | 穷尽可见候选时 exact，裁剪时 lower_bound；不是仅统计输出的 K 条 |
| `empty_result_confirmed` | 显式 proven_none、执行完成且覆盖完整且无命中时为 true；不要求伪造 total_hits |
| `match_evidence` | has_hits / proven_none / not_established，与排名和预算状态分开 |

仅有空数组的旧 Provider 默认回填 unknown 覆盖/终止状态，不能被升级成确认无结果。ANN finished/complete 但没有空结果证明时，empty_result_confirmed 仍为 false；approximate 排名本身不使 truncated 变成 true。原生 proven_none 可以在未提供 total_hits 时确认无匹配项，非空 Top K 也不必精确统计全部命中。`ask` 无证据但未确认无匹配项时明确说明尚未确认。

公共索引可再提供命中字段/字节范围、评分解释、稳定游标及精确版本值；Wiki 适配器映射共同字段，不建立另一套 C ABI。`ask.truncated` 还可表示上下文摘录被裁剪，排名/覆盖/termination 字段只描述本次检索，不把上下文预算混入索引终止原因。

以下情况不得返回看似完整的成功：词项、倒排、候选、时间、内存预算耗尽，取消，索引未追平，或可选语义能力不可用。兼容层至少将实际裁剪映射为 `truncated=true`，保留具体状态；取消/不支持也可直接返回错误，不能丢掉原因。正文摘录截断与搜索覆盖截断分别报告。

Python 对照按词面 BM25 评分，保留最多 32 个词项，默认 12,000 条倒排、1,024 个候选。超限结果是有界近似 Top K，不能被当作原生 Block-Max/WAND 精确评分的正确性金标准。小语料上预算足以穷尽时，用未四舍五入的内部得分排序后才输出展示分数，可作为本套分词/权重的精确排名对照；分词或字段权重不同的实现需要另外核对。

## 迁入原生索引与保留在 Wiki 的逻辑

| 当前 Python 位置 | 后续归属 |
| --- | --- |
| `retrieval.tokens`、`term_key` | 公共分词/归一化与配置身份；保留原文字节映射 |
| `index_document`、`remove_document` | 公共记录投影、反向词项、DF/长度统计、有界倒排更新 |
| `search` 的候选扫描、BM25、Top K、批量水合、预算统计 | 公共索引；Wiki 仅负责结果/引用映射 |
| `state/` 的检索投影与快照读取 | 经 Provider 同事务镜像；stale 的决定仍由 Wiki 作出 |
| `retrieval.chunks` | Wiki/资料连接器的分块策略；分词升级不能静默改变 chunk id 和引用 |
| `wiki.py` 的 revision、编译指纹、租约令牌、依赖、引用、证据去重/复核 | Wiki 保留，不迁入搜索内核 |
| 模型、embedding、reranker 的调用与缓存身份 | 上层 Provider；事务之外执行，不成为无模型索引的依赖 |

建议迁移时保留同一份小语料、ID、来源修订和查询，在独立 namespace/临时索引验证后切换。Python 对照只读或离线运行；不能长期保留两份都可写的权威索引。旧格式只能通过明确的导入/重建步骤退出，不通过修改 provider 名称强行认领。

## 接入验收

- 复用现有 Wiki 摄入/重启、回滚、更新/删除、stale 隐藏、引用与多 worker 拦截测试；新增 Provider 路由、格式切换保护、旧索引认领和异步实现拒绝测试。
- 原生实现再覆盖：同事务源/索引/可见性原子回滚，强过滤和隐藏页不漏出，历史快照、水位缺口、取消/预算、删除后旧向量不可复活、Unicode 字节范围、代次切换与稳定游标。
- 模型关闭时上述词法/过滤与一致性能力仍可用。未启用语义模式时不加载模型；显式请求语义模式而能力缺失时返回不支持。
- 性能与召回按同语料分开记录；Python 现有实测不等于未来原生性能，ANN 也不等于精确向量搜索。本轮不启动模型训练或新增 native 检索实现。
