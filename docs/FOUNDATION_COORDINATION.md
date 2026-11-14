# 索引、RAG、Wiki 协作与公共检索契约

状态：2026-11-14 公共原生 Index v1 与事务桥接已实现并在 Windows/Linux 验证；本文还包含后续增强目标。实际接口以 `include/fengqudb_index.h` 和 `proto/index.proto` 为准，尚未冻结为正式发布 ABI，不构成已完整替换 PTpma/WordGame 的声明。

## 共同基线与代码归属

| 工作树 | 分支 | 归属 |
| --- | --- | --- |
| `D:/study/FengquDB-index` | `codex/index-foundation` | 公共原生索引、词法/结构化/向量检索，PTpma 索引及经验适配 |
| `D:/study/FengquDB-rag` | `codex/rag-foundation` | RAG 原文/事实时态/可见性/删除治理、Provider 与 WordGame 接入 |
| `D:/study/FengquDB-llmwiki` | `codex/llmwiki` | 来源摄入、页面编译、依赖失效、引用与任务恢复、Wiki 服务 |

数据库共同基线为 `32348fb`。Wiki 原首版为 `92efa28`；生产默认已经接入公共 C++ NativeIndex，Python BM25 只保留在 tests 作为对照。RAG 也复用公共索引，不引入独立的 BM25/HNSW 库。

索引模块用 C++20 独立实现，不链接 zvec、FAISS、Lucene 或 SQLite FTS 作为检索引擎。RAG/Wiki 的领域状态和模型编排不挪进通用搜索内核。三个模块共享检索能力，各自保留领域行为及测试。

## 一、同一事务与同一快照

原生索引必须能够附着调用方已存在的 Store 事务和快照。领域原文、事实有效期/删除屏障以及索引投影由同一个事务提交，不能由索引模块重新打开数据库或提前单独提交。

概念操作对应 `fqidx_txn_apply`、`fqidx_snapshot_query` 和 `fqidx_txn_query`；C++ 便捷封装提供 `Client.upsert/patch/erase/query`。批量可在同一外部事务中反复 apply 后统一提交。

索引层不偷偷改变写入模式。宿主决定最终提交及 force_reliable。失败、COMMIT_UNKNOWN、RECOVERY_REQUIRED 沿用 Store 语义，领域层据可靠 receipt 处理重试；不能把未知提交当作已回滚。

Python 已增加公开 `native_identity()` 和 `borrow_native()` 租约，维持调用期间的原生生命周期；详情见 [桥接](NATIVE_BRIDGE.md)。Index 还校验链接到同一 Core 函数地址，防止同进程混用两个 Core 映像。不得读取私有 ctypes 字段传裸指针。

桥接核对 Store UUID、Core ABI 与对象状态；持有租约期间，close 必须等待或保留原生所有者，已关闭对象不能获得新租约。索引 stage 的任何失败使整个外部事务进入只能回滚的状态，禁止调用方误提交“领域原件已改、索引只改一半”的事务；这一状态可基于现有事务取消机制实现，仍需实际验证。

## 二、记录身份与投影版本

- `index_space` 固定索引格式、Analyzer 配置及统计分区。
- `record_id` 是不透明稳定标识；前缀/长度编码由模块处理，用户 ID 不成为物理路径。
- `domain_revision` 保留领域对外版本；`source_revision` 表示可选来源版本。
- `projection_version` 独立单调递增，覆盖一切影响检索的变化。
- `lifecycle_generation` 用于删除、清空和身份重建的防复活检查，不能与普通修订混用。

RAG 关闭旧事实 expires_at 时，对外 revision 可能保持不变，但 projection_version 必须增加。Wiki 的 ready/dirty/deleted、页面发布代数、chunk 所属发布版本变化，同样是投影变化，不能仅凭正文哈希认定无更新。

投影包含可检索字段、类型化元数据、可选向量及其身份、原文定位引用。权威原件归领域模块；索引不强制另外复制一份完整原文。通用记录模式可由索引模块自己托管原件。两种模式都必须在同一 Store 中形成一致的版本引用。

元数据 patch 仍增加 projection_version，但未变正文的词项和向量可按内容身份复用；不能要求为了改 expires_at 或页面状态而重新嵌入正文。投影写入使用预期版本和生命周期令牌检查，不能用相同正文哈希覆盖更高版本。

幂等重放以完整身份、投影版本和输入摘要校验。旧版本更新/删除不可覆盖新版本；相同版本但不同内容属于冲突。清理最小墓碑和生命周期状态须遵守领域防复活规则。

write_epoch 与 read revocation 分开：删除一个 source 可以使该 owner 的旧写票据失效，但不能因此隐藏同 owner 其他 source 的既有记录。实际撤销选择器明确限定 source/lineage/namespace 等范围，不能从写代数的变化猜测全 owner 内容都已被删除。

## 三、结构化过滤与来源约束

过滤是类型化表达式，包含比较、范围、存在性、IN、AND/OR/NOT；字符串、字节、整数、有限浮点数、布尔与 null 的含义显式区分。字段路径和类型在索引契约中登记，不拼接调用方表达式执行。

RAG 授权选择器、owner/public、scope/timeline、逻辑时钟、来源及有效时间必须进入所有检索分支。Unix TTL 不替代逻辑有效区间。Wiki 的内容类型、有效页面状态与发布版本同样在候选生成及最终物化时约束。

BM25 统计分区由领域层选择并持久化，至少隔离不可互见的资料范围；不能通过 DF、总量、Explain 或分数暴露其他私有语料的统计。owner/public 的显式并集可以组合获授权的分区。索引不把调用者可填入的 tenant/owner 字符串当作授权凭据。

严格历史 BM25 还需要逻辑 as_of 下合法文档的统计，不能把未来事实计入评分。请求显式声明统计策略；当前实现若只能提供更宽统计范围，则拒绝严格模式，不静默降级。该增强不阻塞首版现有 dense RAG 替换。

图导航可经过未匹配节点以维持召回，但不将其作为结果、重排输入或上下文。强过滤或小候选集可走精确向量查询；不采用全局 Top K 后简单删除不匹配项的方案。

## 四、质量、覆盖与中断分别报告

响应至少分别表示：

| 维度 | 含义 |
| --- | --- |
| `ranking_kind` | exact 或 approximate；近似排名不等于查询被预算截断 |
| `termination` | finished、cancelled、deadline、memory_limit、candidate_limit、posting_limit、index_lag 等 |
| `index_coverage` | complete、pending、unknown、invalid；说明当前快照相对索引水位的覆盖 |
| `snapshot_version / applied_sequence` | 本次原件视图和派生索引检查点，不允许使用来自未来快照的图 |
| `total_hits` 及其关系 | 可选，exact/lower_bound/unknown；Top K 跳过评分不代表已精确统计所有命中 |

空数组本身不表示完整无结果。只有覆盖与执行均完整，且有足够证据时，才能报告已确认无匹配项；ANN 返回空列表不能自行证明全部候选都不匹配。

增加独立 `match_evidence=proven_none/has_hits/not_established` 或同等语义。ANN 完成且覆盖完整仍返回空时，可预算内 exact 验证，否则保留 not_established，由不支持此状态的兼容门面报错；不要求每次非空 Top K 都精确统计全部命中数。

Store 提交版本、索引 mutation_sequence 和重建 generation 是不同类型，不能把数值直接比较。实现需在快照中读取该索引空间的目标 mutation_sequence；水位表示同代次此前所有索引变更均已覆盖，而不是已见过的最大编号。无关 Store 提交不构成索引事件缺口。下面的 I/S 比较只有在相同版本域内成立。

旧 WordGame 接口无法表达部分结果时，适配器应返回具体异常，不能把截断、滞后或模型缺失包装成正常空列表。Wiki Provider 可保留结构化限制说明，但必须传播至 search/ask 的最终回包。

查询同时携带 Top K、可取消信号、时间、候选、倒排访问和内存预算。结果回传 ID、版本、分数、命中理由、原文位置与来源引用；评分与预算统计不读取无权原文。

fetch 接收版本化 identity、原文 locator 和内容哈希，在同一快照中取得命中版本；不匹配即 stale，不替换成最新正文。摘录字节范围绑定同一版本的原文。等待索引水位不得持有阻塞构建或提交的锁。

向量相似度、距离、BM25 分数及融合分数分别命名；不把旧 zvec 余弦距离直接当成相似度，也不默认不同检索分支的原始分数可直接相加。融合与上下文组织归 RAG，Wiki 负责引用和回包映射。

## 五、异步索引与发布校验

词法/过滤投影的首版同步进入领域事务。ANN 为可重建加速结构，后台任务、检查点及待处理差量在 Store 持久化，不能依赖已过保留窗口的临时通知。

快照 S 只能使用检查点 I <= S 的索引；合并 (I,S] 范围内符合过滤的新增/替换，排除删除和旧版本。所需差量丢失时重建或使用精确路径，不能返回伪完整结果。要求最新可查时，等待水位或搜索有界差量；超预算则明确中断。

返回到用户之前，领域层再次验证生命周期和来源发布状态。RAG 处理删除与结果发布的先后关系，Wiki 处理编译期间来源变化和页面失效；索引快照不能代替这些跨操作的领域屏障。

## 六、模型边界和当前决策

文档向量空间身份包含权重、Tokenizer、Pooling、归一化、维度及文档预处理。查询 profile 单独描述查询模型/指令/预处理，使用经过显式验证的兼容关系关联文档空间；同维度不是兼容证据。查询指令变化不自动意味着全部文档必须重嵌入，也不自动视为兼容。旧资料未知的历史模板不能被迁移器填成当前已验证模板。

新小模型不冒用 KaLM 身份；文档编码空间改变后需要单独重建向量，不能与旧向量直接比较。旧资料迁移已按用户要求取消，业务接入时只重建对应旧索引/RAG 数据，当前 KaLM Q8_0 保持评测基线。

用户给教师蒸馏设置完整流程 10 小时上限。本轮未训练，也不启动无法控制在预算内的蒸馏任务；具体估时及当前排除决定记录在 EMBEDDING_COMPRESSION_ASSESSMENT.md。默认索引零模型；小体积预训练模型的转换与评测作为独立候选，不能阻塞底座开发。

## 七、本轮协作验收

RAG 会话交付字段映射和 Provider 要求，Wiki 会话交付可替换检索边界及现有 Python 行为对照；索引会话汇总冲突并负责公共实现。未经实际链接、事务及恢复验证，不把三个模块描述为已经集成完成。

本轮已收到并阅读的协作材料：RAG 工作树的 `docs/rag/RETRIEVAL_CONTRACT.md`、`PROVIDER_CONTRACT.md`、`CONTRACT_TEST_PLAN.md`、`COORDINATION_REVIEW.md`；Wiki 工作树的 `docs/LLMWIKI_INDEX_PROVIDER.md`。原生签名、桥接和基础实现测试已补齐。Python 截断搜索只能在穷尽的小语料配置下作为排名对照，不能将已有有界候选实现当成精确 Top K 金标准；本轮另加了独立词频/评分的穷尽对照。新公共契约与两边文档随实现继续对齐。

Wiki 会话在 `c88c304` 报告完成 Provider 路由及完整性状态拆分，Windows/Linux 各 28 项测试通过；这是该会话的验证结果，本索引会话未复跑，也不等于公共原生 Provider 已接通。

后续实装：公共 Python 封装已从 Wiki 会话整合到 Index 分支，独立包装测试在本分支 Windows/Linux 各 14/14 通过。Wiki 会话另报告当前原生 Provider 与领域测试各 34/34 通过，RAG 会话报告最小原生来源/分块链路各 25/25 通过；两边领域成果保留各自分支。这些验证不包含 PTpma/WordGame 整体替换和移动设备验收。
