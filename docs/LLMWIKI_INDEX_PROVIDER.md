# FengquWiki 与公共 FengquIndex 的对接契约

日期：2026-11-14。作用域：`D:/study/FengquDB-llmwiki` / `codex/llmwiki`。本文件供 FengquIndex、FengquRAG 与 Wiki 协作对齐，接口字段为逻辑契约，不定义公共 C ABI 的结构体布局。

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

当前仅接收 `atomic_updates=True` 的实现；异步旁路服务不能通过设置这个布尔值来获得事务保证。该属性是受信任实现的声明，不是自动证明。`index/provider` 持久保存 `{name, format_version, tokenizer_version}`，启动时拒绝静默更换格式。旧 0.1 非空 Python 索引可以被默认适配器认领；其他实现需要显式迁移或新 namespace。当前没有自动迁移器或原生 Provider。

当前 Provider 方法仍没有结构化过滤、向量参数、变更游标、精确快照版本或等待水位入口；这些属于以下公共索引接入要求，不能从本轮代码推断它们已经可用。

## 职责与记录映射

Wiki 继续拥有来源摄入、不可变原文、分块边界、编译规格、LLM 提示词/缓存、主题页、依赖传播、引用、任务租约、重试、历史和导出。FengquIndex 统一拥有词法及字段过滤索引、评分、候选定位、可选向量/ANN、水位和重建。FengquRAG 可使用同一原生接口组织召回和上下文；不得在 Wiki/RAG 各维护一套原生检索算法。

| 公共记录字段 | Wiki 的映射 |
| --- | --- |
| scope | 当前 Wiki namespace 的稳定分区标识；不是权限凭据 |
| collection | `raw_chunks` 或 `wiki_pages` |
| id | 原文块 `c-<source-id>-<ordinal>`；页 `p-<page-id>`，保持稳定 |
| revision | 原文块使用原始来源 SHA-256 修订；页使用编译指纹 |
| title / body | 来源标题及原文块，或主题标题及编译 Markdown |
| metadata | kind、source_id/page_id、来源修订、页可见性；未来业务过滤字段另设有类型的 schema |
| sources | Wiki 提供的引用列表：source_id、revision、URI、标题和 `[start,end)` |
| deleted | 当前检索投影的删除状态；不可变原文历史仍由 Wiki 保留 |

文档 revision、来源 revision、数据库 commit version 与异步索引水位是不同概念。页的编译指纹不能代替每条引用的原文版本；URI 和时间戳也不能代替版本保护。一个页可能引用多个来源，不能只留下第一条来源。

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

当前公共 Python 绑定只有内部 `_handle` / `_live` 等实现细节，没有稳定的原生句柄借用 API。正式接入应由绑定/索引拥有方定义一个持有生命周期租约的桥接入口；Wiki 本轮不修改公共绑定和共享根构建，也不会强转私有 ctypes 字段。该桥接、原生 Provider 包装和格式迁移尚待公共索引实现后接入。

## 过滤契约

公共搜索需要有类型的等值、集合、范围、存在性及 AND/OR/NOT 组合过滤。scope/collection、kind、source_id/page_id、revision、deleted/stale 是 Wiki 最小需求。模型/向量空间身份另作强制匹配条件，不能作为可以随意忽略的打分提示。

过滤和可见性应在进入最终 Top K 前参与候选选择。强过滤不能只在未过滤的 K 条候选中事后删去不适用记录；需要按过滤候选精确评分或继续有界遍历。若预算耗尽，返回部分结果与具体原因。即使结果不足 K，只要确实耗尽全部符合条件的候选，也可报告完整。

未知字段、未知运算符或未启用的能力必须明确返回不支持，不能悄悄忽略。权限/租户过滤由宿主授权策略决定，用户给出的过滤不能覆盖它。namespace 自身不提供 ACL，当前单 token HTTP 服务也没有细粒度权限能力。

Python 对照的 `search` 不支持上述结构化表达式；接入后由 Provider/适配层声明能力，未实现时拒绝带过滤的请求，不把忽略过滤的词法结果伪装成完整结果。

## 快照、修订与水位

### 同步词法与过滤

文档正文/元信息、倒排、DF/文档长度统计、过滤字段、删除与 stale 状态必须从同一个提交快照读取。查询开始后不得临时读取 Store 当前值“补全”命中，因为那会混合两个修订。返回的来源修订要能通过 Wiki 的历史读取接口解析。

目前 Python 绑定没有暴露 snapshot 的精确 commit version，Wiki 的 `observed_database_version` 只是取证时观察到的 Store 版本。因此不能把它回填为 `snapshot_version`、`indexed_through` 或“已追平”证明。待公共接口提供精确版本后再映射；在此之前明确返回未知，或使用事务内维护的应用序列。

### 可选异步派生索引

建议最小水位标识为 `(scope, generation, mutation_sequence)`：

- 每次影响检索内容、删除状态或页面可见性的提交推进同一 scope 的 `mutation_sequence`；领取任务、日志查询等无关行为不应伪装成内容更新。
- `generation` 在重建/格式切换时更换。旧 generation 的游标、租约和水位不能自动解释成新 generation 的已完成工作。
- `indexed_through` 表示该 generation 中**此前所有变更**均已处理的连续水位，不能取“曾经处理的最大序号”掩盖缺口。向量还须携带 model-space 身份及自己的连续水位。
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
  "candidates": 1
}
```

`citations` 实际使用时必须是 Wiki 摄入/发布时提供的有效引用。页命中带 page_id，原文块带 source_id。返回纯复制值，不能把 C++ 借用视图泄露到已关闭快照后。

公共索引可增加 `strategy`、`exact_top_k`、`degraded_reasons`、命中字段/字节范围、评分解释、稳定游标、精确 snapshot/version 或序列、水位等。字段名由公共接口统一确定，Wiki 适配器负责映射，不建立另一套同名 C ABI。

以下情况不得返回看似完整的成功：词项、倒排、候选、时间、内存预算耗尽，取消，索引未追平，或可选语义能力不可用。兼容层至少将实际裁剪映射为 `truncated=true`，保留具体状态；取消/不支持也可直接返回错误，不能丢掉原因。正文摘录截断与搜索覆盖截断分别报告。

Python 对照按词面 BM25 评分，保留最多 32 个词项，默认 12,000 条倒排、1,024 个候选。它是有界近似 Top K，不能被当作原生 Block-Max/WAND 精确评分的正确性金标准。对照精确排名时须在小语料上提高预算到穷尽，并明确区分分词/字段权重差异。

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
