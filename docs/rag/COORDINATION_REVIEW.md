# 公共协作契约的 RAG 复核

状态：2026-11-14，已完成契约复核、[公开租约桥接](NATIVE_BRIDGE.md) 和共享 Index v1 的[有界来源路径集成](NATIVE_SOURCE_PATH.md)，后两者在 Windows/Linux 定向验证通过。仅修改 `D:/study/FengquDB-rag`；以下清单保留完整设计要求，未完成项不因局部接入而视为通过。

复核来源：`D:/study/FengquDB-index/docs/FOUNDATION_COORDINATION.md`，读取版本 SHA-256 为 `ec73c11f9f5fbef8492378468500015ae025b71f1155beb30d15980a4f5863bf`。主契约仍是草案，本文反馈不代表三边已经冻结 C ABI。

## 1. 已覆盖并接受的设计

当前公共草案覆盖了 RAG 的主要接入方向：同一外部 Store 事务/快照、独立单调 projection_version、类型化授权/时态过滤、模型空间校验、取消和多维资源预算、排名/终止/覆盖分别表示、ANN 差量追平，以及最终领域生命周期复核。

RAG 的旧 hnswlib 候选已经撤回。倒排、BM25、精确向量、自研 HNSW、检索水位和重建全部由 FengquIndex 唯一实现；RAG 负责领域治理、Provider、RRF/重排、上下文及 WordGame 适配。不会先在 RAG 内写另一套引擎等待合并。

主契约没有显示出必须推翻的架构冲突。以下项目需要在实现前补齐明确语义或签名；其中有些主草案已原则覆盖，本文给出 RAG 需要的可验收细化。

## 2. 字段名收敛

| 原 RAG 草案 | 公共名称 | 采用方式 |
| --- | --- | --- |
| record_revision / business_revision | domain_revision | 旧 WordGame revision 原样映射，不把检索更新塞进旧修订 |
| projection_etag | projection_version | 接受独立 u64 单调版本；另留 payload_digest 校验同版本异内容 |
| collection/analyzer 配置身份 | index_space | 与数据库 UUID、业务 namespace、向量空间分别带类型 |
| space_id | vector_space_id | RAG 局部字段可保持 space_id，但 ABI 中不得误当 index_space |
| ranking_guarantee | ranking_kind | exact / approximate，与终止/覆盖独立 |
| completed/partial/reason | termination | finished/cancelled/deadline/*_limit/index_lag 等，避免只用一个 bool |
| coverage=complete_at_S | index_coverage + snapshot_version | complete 仅说明 S 的数据覆盖，不能证明 ANN 排名是精确 |
| base_applied_version | applied_sequence + 坐标说明 | 需明确是否 StoreVersion；如不是，提供可验证映射 |

projection_version 由可信领域/事务路径分配，任何可检索元数据变化都推进；同版本同 digest 为幂等重试，同版本异 digest 为冲突。递增不能溢出环回。删除保留足够的 version/lifecycle 墓碑；重建派生图不重置领域投影版本。

## 3. 仍需细化的八项

| 编号 | 需要的细化 | RAG 要求与缺失时行为 |
| --- | --- | --- |
| C01：事务/快照桥接 | Python 的公开 lease/borrow API、所有者和关闭竞态、跨库句柄校验 | 不读取私有 ctypes 字段；调用期间 pin 所有者。跨 Store/已关闭对象报错；Index 不自行开 Store/提交。未实现前不宣称原子接入完成 |
| C02：投影元数据补丁 | patch 的 expected projection_version、新 version、同版本 digest；向量保留规则 | 关闭旧事实 expiry 不增加 domain_revision，但推进 projection_version；不得要求重嵌入。多记录 stage 失败后整事务不可半提交 |
| C03：空结果证据 | 在排名/终止/覆盖之外，明确空结果是否有完整无匹配证据 | `match_evidence=proven_none/has_hits/not_established` 或等效字段。ANN finished+complete 返回空仍不能当确证无匹配；exact 或零授权候选证明后旧 API 才能回正常 []，否则明确错误 |
| C04：历史 BM25 统计 | 持久静态统计分区如何处理动态 as_of 与 public 并集 | tenant/owner 分区已覆盖权限隔离，但不自动隔离未来文档的 DF/N/avgdl。启用严格历史 lexical 时统计必须与实际时间 Predicate 一致；不支持就拒绝严格模式。此项不阻塞现有 dense 替换 |
| C05：结果取正文 | fetch 的 versioned identity、正文 locator、text_hash 和同 snapshot span 语义 | 不强制复制整份权威原件；Index 为检索物化字段时仍需可验证来源。fetch 只能读命中的版本或报 stale，不能换成最新正文。仅权威 locator 与有界摘录均可满足 |
| C06：水位坐标/等待 | applied_sequence 的类型、连续覆盖证明、wait_until 与持久差量保留 | 不把最大已见序号当完整水位；StoreVersion 与 IndexSequence 不同则需映射。snapshot S 不用未来图，差量缺口 exact/重建/错误；wait 不持有阻塞构建的锁 |
| C07：生命周期范围 | source 删除时 owner 写票据失效与旧内容可见性的分别处理 | 提升 owner write_epoch 只作防回写；同 owner 其他来源的保留记录继续可见。read revocation 精确到 source/lineage/namespace，不能全 owner 误隐藏 |
| C08：空间与查询变换 | index_space、document vector space、query profile 分开；旧来源 unknown 的处理 | 同维度不能混模型；query 指令变化需显式兼容注册/评测，但不自动等于文档全部重嵌入。旧迁移不能把 unknown 历史模板补成当前已验证哈希 |

C01/C02/C05/C06 是 RAG 运行接入前置；C03/C07/C08 是正确性语义需固定的细节；C04 随严格 lexical 启用验收。预算、类型化过滤、分页绑定等其余具体要求保留在 [检索契约](RETRIEVAL_CONTRACT.md)。

### 3.1 Python 桥接的最低可用形状

```text
with transaction.borrow_native(expected_store_uuid) as lease:
    index.stage(lease, prepared_projection)
// lease 退出后不再持有借用指针；不调用 commit

with snapshot.borrow_native(expected_store_uuid) as lease:
    result = index.search(lease, request)
```

仅示意语义，不要求照此命名。close 与 lease 的排序必须定义：已经取得的 lease 要么保持有效直至返回，要么新 close 等待；禁止另一线程释放原生指针后继续调用。C ABI 句柄的来源、Core 版本和 Store 身份必须校验。桥接实现由公共 SDK/Index 接入一侧统一拥有，RAG 不修改其他工作树的 binding。

## 4. 空结果处理修正

```text
finished + coverage=complete + ranking=exact + proven_none → 旧 API []
finished + coverage=complete + ranking=approximate + hits → 按允许 ANN 策略返回
finished + coverage=complete + ranking=approximate + empty + not_established
    → 在授权候选上预算内 exact 验证；仍不能证明则 AppError
deadline/cancelled/任何 limit/index_lag 或 coverage 非 complete → 明确不完整/失败
```

`total_hits` 是 OPTIONAL，且需有 exact/lower_bound/unknown 关系；无需为每个正常非空 top-k 全量计数。但未定义 total_hits 时，不能通过它推断没有匹配；`has_more` 也不能从 top_k 数量随意推导。公共草案已经指出 ANN 空列表不能证明无匹配，这里将其补入 RAG 门面规则和 Q23。

## 5. 模型当前决定

主会话给完整教师蒸馏约 **12–24 小时工程预算**，不是实测耗时，超过用户 10 小时上限。因此本轮明确排除蒸馏且不启动训练。小型预训练模型的评测是独立工作，不作为迁移或底座开发前置条件。

迁移保持当前 document vector space 与来源身份，不重标为任何新模型。KaLM 是原有运行模型和效果评测基线，基础包仍零模型依赖；后续候选模型若获采用，显式新空间、重建副本、独立效果与性能验证。

## 6. 交付与验证状态

- 已完成：阅读公共草案、字段映射、八项细化反馈、原 RAG 草案同步、新增契约场景设计。
- 设计中：公共 C ABI、Python lease 桥接及实际投影/快照/水位实现。
- 前置依赖：C01/C02/C05/C06 的实现和联调，不能以双写或另一套算法绕过。
- 未执行：公共库链接、事务/故障/模型/性能测试、WordGame 迁移或正式替换。

后续验证按 [契约测试设计](CONTRACT_TEST_PLAN.md) 逐项映射，尤其补齐 ANN 空结果证据、公开 lease 关闭竞态和 source 删除不误清其他来源。
