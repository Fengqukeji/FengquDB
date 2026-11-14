# RAG 对 FengquIndex 的公共检索契约需求

状态：保留完整能力需求提案。共享 Index ABI1/proto/index.proto、有界来源路径和 MemoryStore 治理已实现；实际接口及已验证范围见 [API.md](API.md)、[ACCEPTANCE.md](ACCEPTANCE.md)。本文概念函数名不是已发布 API，也不授权修改其他工作树或共享根 CMake。

依据：当前 `D:/study/FengquDB/docs/INDEX_FOUNDATION_DESIGN.md`、`EMBEDDING_COMPRESSION_ASSESSMENT.md` 及本目录 WordGame R01–R26 / G01–G12 需求。共享数据库基线为 `32348fb`。主设计仍在演进，以下函数名是调用草案，不表示 FengquIndex 已接受或已有这些 API。

已对照 Index 工作树的 `docs/FOUNDATION_COORDINATION.md`，接受其 projection_version、ranking_kind/termination/index_coverage 命名与单调投影版本；字段映射和待细化项见 [协作复核](COORDINATION_REVIEW.md)。公共 ABI 仍未冻结。

## 1. 所有权与优先级

| 模块 | 唯一负责的内容 | RAG 调用方式 |
| --- | --- | --- |
| FengquDB Store | 事务、快照、可靠提交、恢复、备份、原子版本 | 沿用公开 C ABI，不增加第二套 WAL |
| FengquIndex，C++20 | 搜索投影、元数据过滤、倒排/BM25、精确向量、可选自研 HNSW、索引水位/重建/Explain | 同一 Store 的投影模式；所有检索算法只实现一份 |
| FengquRAG | 租户/知情人授权、逻辑时间、事实谱系、编辑历史、生命周期、防复活、Provider、融合/重排、上下文及旧 API 适配 | 组织强制过滤与一致快照，消费带版本的 Index 命中 |
| WordGame | SQLite 权威事件/观察、可见投影、世界锁、outbox、NPC 墓碑/cutoff、规则发布 | 保留现有业务校验；RAG 不推导知情关系 |

必需能力标为 MUST；增强能力标为 OPTIONAL。P1 MUST 可由精确检索满足；不要求 Index 先实现 HNSW、量化、重排或模型。RAG 不链接 zvec/FAISS/Lucene/SQLite FTS 或 hnswlib 充当新检索实现；zvec 仅供旧库迁移读取和对照。

RAG 可保持 C++17 源码，经 C ABI 调用 C++20 Index；组合发行的编译器和链接配置须能构建 Index。不能以 RAG 的语言标准承诺整个发行包仅需 C++17 编译器。

## 2. 记录与版本类型

### 2.1 标识

```text
ProjectionKey = {
  tenant_store_uuid, collection_uuid,
  namespace_id, timeline_id,
  external_record_id
}
RecordRef = {
  key: ProjectionKey,
  record_revision: u64,
  projection_version: u64,
  payload_digest: sha256,
  text_hash: sha256,
  canonical_commit_version: StoreVersion
}
```

MUST：外部 ID 按原字节精确匹配，不按路径解析、不大小写折叠、不因标题同名合并。WordGame 的 external_record_id 保留 32 位 hex；通用接口可接受有界 UTF-8 ID，长度上限由统一协议确定。Index 的内部整数 DocId 不作为业务身份，重建/压缩后不能用旧 DocId 命中另一个文档。

三个版本不可合并：

| 字段 | 意义 |
| --- | --- |
| record_revision | 旧 API 事实/编辑修订号；沿用迁入值及谱系顺序 |
| projection_version | 独立单调推进，覆盖有效期、head、来源、生命周期等任意检索可见变化；同版本用 payload_digest 校验幂等 |
| canonical_commit_version | 发布此投影的 Store 提交位置；用于快照/水位，非业务时间 |

例如新增 t=5 的事实会把旧事实 expires_at 关闭为 5，但当前 WordGame 不因此增加旧记录 revision。Index 若只用 record_revision 忽略这次更新，将在 t>=5 错误召回旧事实。projection_version 必须增加，旧向量可保持，不能要求重新推理才能改有效期。

canonical_commit_version 是提交回执或快照读取产生的输出 provenance，prepare 不能猜测未来 Store 版本后写进输入。Index 如采用独立单调 IndexSequence，必须使用独立类型并明确与 Store 快照的可验证映射，不能直接把两个数当同一水位。

### 2.2 索引投影

```text
IndexProjection = {
  key,
  record_revision, projection_version, payload_digest,
  fields: {title, body},
  metadata: {
    scope: string, timeline_id: string, owner_id: string,
    known_at: i64, has_expiry: bool, expires_at: optional i64,
    source_id: string, source_revision: string,
    lineage_id: string, kind: string,
    lifecycle_instance: opaque, searchable: bool
  },
  text_hash,
  sources: [SourceRef],
  vector: optional {space_id, data_fp32, document_transform_id}
}
SourceRef = {
  source_id, source_revision_label,
  optional source_content_hash,
  optional authority_ref,
  optional chunk_index,
  optional span: {field, start_byte, end_byte}
}
```

MUST：metadata 类型固定注册，不允许同字段有时 string 有时 number。`source_revision` 是业务追溯标签，不是数值锁，也不能代替内容摘要。正文范围为 UTF-8 原文字节 `[start,end)`；必须在字段边界内，不能用归一化后的词元偏移切原文。

同一 event source 可以产生 GM 与多个 NPC 的不同可见文本：它们是不同 key、不同 body/text_hash/投影版本。不能因 source_id 相同就共享 GM 原文，或将 GM 摘录交给 NPC。向量缓存可按相同文本与完整空间身份复用，授权仍逐投影执行。

## 3. 同事务写入接口是首要依赖

以下 primitive 是 MUST；具体名字可调整，语义须保留：

```text
plan = index.prepare_change(projection, expected_projection_version, budgets)
// 分词/编码等可在事务外准备；plan 绑定输入及配置指纹
txn = store.begin()
rag.validate_ticket_and_fact_state(txn)
rag.stage_canonical_records_vectors_history(txn)
index.stage(txn, plan)             // 不自行提交
rag.stage_operation_receipt(txn)
receipt = txn.commit(reliable=true)
```

`stage` 需接受同一个 Store 的公开事务句柄（C ABI），维护元数据、过滤索引、正文/向量投影、必要倒排差量及待办。指针/句柄绑定 Store UUID，拒绝跨 Store 或 ABI 不兼容事务；组合发行不能各静态链接一份不可互认的 Core 再传裸句柄。

Index 不重新保存第二份“权威记忆”：RAG 保有治理记录，Index 投影字段可物化以支持快照检索。两份表示在同一事务发布，必须标记角色和重建方向。Index 的通用 records API 可供其他应用使用，但 RAG 不同时经 records API 再提交一次。

元数据更新与删除 MUST 支持：

- `patch_metadata(txn, key, expected_projection_version, new_version, changes)`：有效期/head 更新保留正文与原向量，不触发 Embedding。
- `remove_projection(txn, key, expected_projection_version)`：删除过滤/倒排/向量可见性，生成派生图删除记录；旧图不能继续返回该记录。
- 同一事务内多条事实关闭/新增、同批重复合并；prepare 过期时不忽略冲突。
- `stage` 资源失败后事务不得提交部分 RAG 内容；按 Store 的 rollback/close 规则处理。
- 原文提交后异步图更新失败，canonical+delta 仍然存在；不能把已提交的原文当失败回滚。

若暂时只有“Index 自己提交”的 API，RAG 不采用原文库与索引库双写伪装原子性；此项为待协商接口依赖。可推进类型、Provider、迁移格式和测试设计，暂不声称持久 RAG 原子路径已实现。

## 4. 类型化过滤

MUST 支持下列核心表达式，使用注册 FieldId 和有界类型值：

```text
Eq(field, value) / In(field, [values])
Range(field, lower, upper, include_lower, include_upper)
Exists(field) / All([expr]) / Any([expr]) / Not(expr)
```

最少需要 string 精确相等/集合、i64 范围、bool、字段存在性。区分 missing、empty string、null；`Not(Eq(missing,x))` 的语义必须固定，本应用不依赖模糊的 SQL 三值逻辑。暂不依赖任意 JSON 路径或运行时脚本。

WordGame NPC 请求的强制 Predicate 草案：

```text
All([
  Eq(collection_uuid, authorized_collection),
  Eq(scope, trusted_world_scope),
  Eq(timeline_id, trusted_timeline),
  Eq(owner_id, "npc:" + trusted_person_key),
  Eq(searchable, true),
  Range(known_at, upper=as_of, include_upper=true),
  Any([Eq(has_expiry,false), Range(expires_at,lower=as_of,include_lower=false)]),
  revocation_filter_from_trusted_lifecycle
])
```

tenant 由句柄限定，不作为用户可覆盖条件。可选 source_id 只能收窄此 Predicate。通用 public 并集为 `In(owner_id,[trusted_owner, ""])`；WordGame 游戏请求固定 include_public=false。空 owner 仅公共归属，绝不表示任意 owner。

Predicate 必须在 dense/lexical 候选选择中执行，结果物化再次核对同一快照。不能全局取 top-k 再去掉其他 owner。ANN 导航可经过非匹配节点，但此类节点不能进入结果、摘录、重排、上下文或可见 Explain 样例。

RAG 的票据 write_epoch 与内容可见性分开：按 source 删除时提升 owner 写入代数是为了阻止在途写回，**不能将同 owner 其他来源的保留记录一起隐藏**。Index 接收独立的 source/lineage/namespace 撤销 Predicate 或原子 tombstone；不把“record.write_epoch 等于最新 owner epoch”作为通用可见性规则。

## 5. 向量空间与 Provider 边界

MUST：Index 不加载/下载模型、不启动推理、不在事务中回调 Provider。写入已算向量，查询传已算 query vector；关闭向量模块后词法检索仍可用，但显式 dense 请求不能偷偷变词法。

```text
VectorSpace = {
  space_id: opaque stable digest,
  producer_identity, dimension, scalar_type=fp32,
  metric=cosine | dot,
  normalization=unit_l2 | none,
  document_transform_id,
  query_compatibility_contract_id
}
QueryVector = {space_id, query_profile_id, data_fp32}
```

Index MUST 校验 space_id、长度、finite 和 metric/norm 契约，拒绝异模型同维度混查。cosine 对外为 similarity，不能直接回某库的 cosine distance。KaLM 首期配置由 Provider 注入 896 维/L2/cosine；主索引库无 KaLM 常量。

query_profile 与文档空间分别记录：查询指令改变后须显式注册/评测与已有文档空间的兼容性，不能静默重标旧向量，也不默认要求仅查询模板变化就重嵌入所有文档。旧来源未知的 transform/provenance 需显式 unknown，不能补成已证明的哈希。细节见 [Provider 契约](PROVIDER_CONTRACT.md)。

压缩向量/模型为 OPTIONAL。FP32 为正确性基线；是否携带大模型由应用决定，基础包没有 500 MiB 权重依赖。主会话完整蒸馏工程预算约 12–24 小时（非实测），超过 10 小时上限，本轮已排除且不启动训练。小模型另行评测，不作为数据迁移前置。

## 6. SearchRequest 草案

```text
SearchRequest = {
  handle: authorized_collection_or_namespace,
  query_text: optional utf8,
  vector: optional QueryVector,
  mode: lexical | dense,           // hybrid 由 RAG 编排同视图两次调用
  predicate: typed Predicate,
  fields: [title, body],
  top_k: u32,
  min_cosine_similarity: optional float,
  ranking: exact_required | ann_allowed,
  consistency: {snapshot_token, min_canonical_version, latest_required},
  budget: {
    deadline_monotonic, max_work, max_scan_candidates,
    max_temp_bytes, max_result_bytes, cancellation_token
  },
  incomplete_policy: reject | return_tagged_partial,
  output: ids | excerpt, explain_level,
  cursor: optional opaque
}
```

MUST：预算与取消作用于准备、过滤、统计、距离计算、排序和物化，不能只限制最终 top_k。取消句柄生命周期及线程安全单独定义；回调不得在 Index 内部锁上重入数据库。真实计费内存与进程总 RSS 分开报告。

一致性参数不能同时许诺相冲突的时点：传 snapshot_token 时固定 S，要求 min_canonical_version<=S，且禁止 latest_required。请求 latest 时先捕获单个 S，再使本次所有检索/取正文使用它；不在两路 hybrid 中各自重新选择“最新”。无可满足的组合在执行前拒绝。

WordGame top_k<=30 是适配限制，不限制通用 Index；通用 top_k 有可配置硬上限。调用方要求 min_cosine_similarity 时 dense 的阈值按归一化后真余弦执行；排序稳定 tie-break 使用外部 key，不能用会重建的内部 DocId。

RAG 默认只消费 ids/ref 后在同一快照批量取原文。lexical 摘录可按需返回，长度/字节预算明确，不能为每个候选读取全文。完整正文/历史用 versioned get，避免结果 ID 对应的正文已变更。

Hybrid 由 RAG 在同一 snapshot/predicate 下获得 dense 与 lexical 两路候选，以 RRF 合并；首期不要求 Index 另造融合算法。Reranker 只接受最终授权候选，不跨权限拉取“补充”文档。

## 7. 快照、水位与新鲜度

MUST 提供绑定同一 Store UUID 的不透明快照/租约，或等效可验证的同视图接口。`snapshot_version=S` 不可仅是输出标签：过滤、词项、向量、投影版本及正文读取必须实际对应 S。

```text
IndexWatermark = {
  store_uuid, collection_uuid, index_generation, analyzer_id, space_id,
  base_applied_version: I,
  delta_available_from, canonical_version: C,
  pending_work, state
}
```

I 表示所有 <=I 的相关变更已处理，不能取“见过的最大 commit”掩盖中间空洞。水位的 StoreVersion、索引构建 generation、业务 known_at 是不同坐标。

查询固定 S：图/倒排基准 I<=S，并覆盖 `(I,S]` 的同视图差量。基准晚于 S、差量缺口或格式/模型身份错时，不能读取未来状态冒充一致：选择有效旧基准/完整 exact 路径、等待可满足的构建，或明确错误。等待不得持有会阻塞构建/写入的锁。

`wait_until(version, deadline, cancel)` 和 `status` 是 MUST：支持调用方等待索引追平、处理超时、索引损坏和重建。read-your-writes 可用可靠 receipt 的版本指定 min_canonical_version；后续删除仍优先撤销可见性，不能为满足旧 receipt 复活内容。

若 Index 丢失可重建图，状态须说明 rebuilding；覆盖全量授权向量的 exact 路径可返回完整结果。一个只搜索旧图、未处理差量的请求不能标记 complete。基于 Store.watch 的实现必须持久保留/重放所需版本；history lost 后重建，不从新水位直接跳过缺口。

## 8. 结果完整性与错误

MUST 分开表达以下三个维度：

| 维度 | 值示例 | 不能混淆 |
| --- | --- | --- |
| 执行完成性 | completed / partial / cancelled / limit / error | top_k 没填满不代表执行中断；完成空结果是合法结果 |
| 快照覆盖 | complete_at_S / stale_at_I / gap | canonical 已提交不等于图已覆盖 |
| 排名保证 | exact / approximate_ann | 正常完成 ANN 仍是近似；与资源中断是两回事 |

这些维度在公共协议采用 `termination`、`index_coverage`、`ranking_kind` 名称；上表是本地说明性别名。空结果还须带 `match_evidence=proven_none/has_hits/not_established` 或等效可验证依据，不从 finished/complete 自动推导。

```text
SearchOutcome = Completed(SearchResult)
              | Incomplete(reason, optional tagged_hits, observed_watermarks)
              | Failed(error_code)
SearchResult = {
  hits: [{ref: RecordRef, dense_similarity?, lexical_score?, matched_fields?, spans?}],
  snapshot_version, index_generation, base_applied_version,
  coverage=complete_at_S,
  ranking_kind, termination=finished, index_coverage=complete,
  match_evidence,
  next_cursor?, has_more?, strategy,
  bounded_stats: {scanned, candidates, work_units, temp_peak_bytes}
}
```

公共 C ABI 可以采用等效版本化 struct/状态码，不要求修改 Core 的全局 enum。默认 `incomplete_policy=reject`：预算/取消/索引落后不能返回普通成功列表，包括不能用 `[]` 表示失败。允许 partial 时必须单独 outcome，partial cursor 不当作完整分页游标。

WordGame 兼容门面只接受 Completed 且 complete_at_S，并且满足本请求排名策略。正常非空 ANN 在显式 ann_allowed 下可返回；ANN 空结果只有经完整 exact/零授权候选证明为 proven_none 才能映射正常 []，否则预算内验证或明确 AppError。partial/stale/gap 同样报错，上层 scene 可按现有逻辑降级并报告 memory_status。

dense 未启用 → mode_unavailable；空间不匹配 → space_mismatch；旧 cursor → cursor_stale；缺覆盖 → index_not_ready/history_gap；预算 → limit；取消 → cancelled。错误应可辨别和有界，无原文/密钥日志。Index 不决定 WordGame 的统一响应文案。

Explain 中精确候选计数、词频和样例也受授权及时间过滤。严格历史 BM25 需按授权且时间有效集合的统计评分；若当前后端只有集合全局统计，明确拒绝此严格模式，不能把未来或别的 NPC 文档纳入评分统计而宣称满足该契约。

## 9. 版本读取、分页与删除

MUST：`get_many_at(snapshot, [RecordRef])` 或等效版本读取；ref 不匹配时返回 stale，不悄悄用最新正文。RAG 在返回前再次检查 namespace/source/lineage 撤销状态，删除与结果发布在生命周期屏障排序。

MUST：管理遍历支持按外部 ID 的稳定 keyset 与有界页；WordGame 的 after/next_revision 形状由 RAG 保持。跨页冻结快照为 OPTIONAL；实时 keyset 和冻结 snapshot 不得用同一完成性描述。游标绑定 collection、Predicate、索引/配置代数及 snapshot 策略，换 owner/向量空间不可复用。

MUST：原子 tombstone 或 generation gate 能使新查询立即排除被撤销目标，物理批删/图回收可随后进行。删除某 source 不扩展为整个 lineage；删除 lineage 则清该谱系全部版本。RAG 负责选择删除目标和可靠 purge 状态，Index 负责对应搜索投影及派生项的实际移除。

## 10. 端到端调用草案

### 10.1 单次事件写入

```text
// 可信业务先提交 SQLite event + observations + outbox
items = business.visible_projections(committed_event)
ticket = rag.prepare_write(authorized_handle, items)
vectors = provider.embed_documents(ticket.unique_texts, request_context)
plans = index.prepare_changes(ticket.projected_records, vectors)
txn = store.begin()
rag.revalidate_and_stage(txn, ticket, vectors)
index.stage(txn, plans)
rag.stage_receipt(txn, operation_id)
receipt = txn.commit(force_reliable=true)
// 所有 owner/chunk 成功才 ack；COMMIT_UNKNOWN 先恢复查 receipt
business.ack_outbox_when_complete(receipt)
```

### 10.2 NPC recall

```text
authorized = business.authorize(account, world_uuid, npc_stable_key)
predicate = rag.mandatory_predicate(authorized, as_of_hours, include_public=false)
query_vector = provider.embed_queries([query], request_context)[0]
snapshot = store.snapshot()
result = index.search(snapshot, query_vector, predicate,
                      ranking=ann_allowed, incomplete_policy=reject, budget=budget)
rag.require_complete_at_snapshot(result)
records = rag.get_many_at(snapshot, result.refs)
records = business.recheck_event_observation_cutoff(records)
rag.publish_if_lifecycle_still_valid(records)
// scene 继续做关注、近期事件去重与字符预算
```

### 10.3 历史事实查询

```text
// t1=1 东村，t2=5 西村；旧 revision 不因关闭 expiry 增加
query(as_of=4) → old_record
query(as_of=5) → new_record
// 对同一 snapshot 更换逻辑 as_of，不能误换成 Store 提交时间
```

## 11. 交接与依赖状态

| 能力 | RAG 优先级 | 当前状态 / 缺少时的处理 |
| --- | --- | --- |
| projection identity/version/digest、同 Store 事务 stage | MUST，P1 前置 | 公共接口待 Index 确定；不实现另一套索引规避 |
| typed Predicate、同快照精确向量+版本读取 | MUST，dense 替换前置 | 无能力即明确不可用，不用全局 top-k 后过滤 |
| watermark/status/wait、delta 覆盖、取消/预算/完成性 | MUST，可靠接入前置 | 先对齐契约，后做注入/恢复验证 |
| ordered metadata browse / atomic tombstone | MUST，管理和删除前置 | 设计中，保留旧 API 兼容要求 |
| filtered BM25 / 统计范围 | OPTIONAL 对现有 dense 替换；启用 lexical 时 MUST | 未实现不得静默降为全局统计 |
| HNSW/SIMD/量化 | OPTIONAL 对正确性；规模验收按负载决定 | Index 独占实现；RAG 不引依赖 |
| RRF / Provider / 上下文 / WordGame 门面 | RAG 所有 | 契约和测试设计可独立推进，运行代码未实现 |

这份提案不把主设计的“只需摘要”理解为 NPC 可读任何摘要：摘要、命中理由和来源名称同样可能含私密信息，必须经过同一过滤与版本约束。完整验收场景见 [契约测试设计](CONTRACT_TEST_PLAN.md)。
