# RAG 实现验收记录

日期 2026-11-14。工作树 D:/study/FengquDB-rag，分支 codex/rag-foundation，基线 32348fb7d269a4b328936d8ab1d208d70b7f9a64。未提交、未推送。完整实现入口见 [API.md](API.md) 和 [ARCHITECTURE.md](ARCHITECTURE.md)。

结论：通用 MemoryStore、WordGame 兼容门面、原生检索与治理已实现；隔离的真实路由和现有 KaLM 接入可运行。正式游戏切换、全游戏 G01–G12 和生产规模对照没有执行，因此不宣称线上替换已经完成。

最终冻结库的当前验收见末尾“最终冻结版回归”；前面的首轮测试与性能保留为历史证据。主清单 build/rag-memory-verification.json 指向最终冻结结果，早期记录保留在 build/rag-initial-memory-verification.json。

## 验证结果

| 执行环境 | 用例 | 结果 |
| --- | --- | --- |
| Windows x64 / Python 3.12 | test_memory.py | 38/38，无跳过 |
| Linux x64 Docker fqdb-lang-dev:1 | test_memory.py | 38/38，无跳过 |
| Windows x64 | test_native_sources.py | 26/26，无跳过 |
| Linux x64 Docker | test_native_sources.py | 26/26，无跳过 |
| WordGame 现有 Windows venv | test_wordgame_adapter.py --real-model | 4/4，无跳过 |

合计 68 个不同用例、132 次平台执行。MemoryStore 的隔离、时间、事实、CAS、删除竞态、可靠批次、重开、清理屏障、审计分页、取消/预算、最终版本校验均调用真实 Core/Index；不在 RAG 用 Python 实现假的检索引擎。大部分测试使用确定性合成向量来精确检查不变量，模型效果验证单列如下。

日志：build/test-memory-{win,linux}.log、build/test-sources-{win,linux}-final.log、build/test-wordgame-adapter.log。机器可读清单 build/rag-memory-verification.json 保存源文件、共享 DLL/SO、wrapper、日志和样本的 SHA-256。公共依赖继续开发时，验收结论只适用于清单中的实际二进制版本。Linux 记录实际 SO 目标文件，symlink 自身时间不代表构建版本。

公共 Store 身份/租约桥接另见 [NATIVE_BRIDGE.md](NATIVE_BRIDGE.md) 和 build/store-bridge-verification.json；那是先前独立的桥接验证，未冒充本轮重复执行。

## WordGame R01–R26 映射

下列测试名均来自 tests/rag/test_memory.py，省略 test_ 前缀。适配用例位于 test_wordgame_adapter.py。

| 需求 | 证据 |
| --- | --- |
| R01–R04 账户/归属/公共/时间 | ownership_account_scope_timeline_public_and_time；fact_timeline_half_open_and_owners；实际 Pydantic/HTTP |
| R05 精确去重 | exact_dedupe_sources_owners_and_restart_skip_embedding |
| R06–R07 长文/事实拒绝 | import_chunking_and_durable_dedupe |
| R08 事实转换 | batch_fact_versions_atomic_and_retry_does_not_reopen；fact_expiry_never_extends_and_empty_gap |
| R09–R10 纠错/CAS | correction_cas_source_identity_and_audit_pages；fact_edit_cannot_change_period_or_edit_closed_head；edit_closed_during_embedding_is_rejected |
| R11 审计分页 | correction_cas_source_identity_and_audit_pages；management_list_stable_keyset_and_history_visibility |
| R12 谱系删除 | delete_complete_lineage_and_no_old_fact_reappears |
| R13–R14 精确来源/续接 | source_cleanup_is_exact_and_can_continue_missing_head |
| R15 防复活 | delete_or_cleanup_during_embedding_prevents_resurrection |
| R16 批次一致性 | native_failure_rolls_back_fact_transition_and_batch；process_exit_before_commit_preserves_previous_fact |
| R17 分批清理/恢复 | bulk_cleanup_failure_keeps_barrier_and_recovers_after_reopen；process_exit_after_cleanup_batch_recovers_remaining_jobs；large_edit_history_is_deleted_in_bounded_steps |
| R18 千 owner | same_account_thousand_owners_share_one_store，单次原子批写1000项 |
| R19 浏览 | management_list_stable_keyset_and_history_visibility |
| R20 cosine | dense_threshold_and_native_result_evidence；真实 KaLM 小样本 |
| R21 过滤注入 | filter_injection_and_source_filter_cannot_expand |
| R22 身份/元数据 | model_identity_missing_metadata_and_no_legacy_relabel |
| R23 旧数据迁移 | 用户取消，不实现、不执行旧库删除；新库与旧目录误用明确拒绝 |
| R24 懒加载/设备策略 | 复用现有 Provider；lexical_query_does_not_load_model；真实 CUDA 启动通过，未在本轮重跑其设备故障注入 |
| R25 模型/vector 校验 | vector_validation_normalization_and_provider_contract_change；callback_embedding_large_finite_values_normalize；真实模型由现有 Provider 执行权重校验 |
| R26 兼容接口 | wordgame_facade_translates_errors_and_keeps_model_external；实际 Pydantic、CRUD/search/list/history HTTP；status/warmup 回调测试。未运行浏览器管理页或完整工具/游戏流程 |

扩展能力：lexical_hybrid_strict_time_stats_and_public_union 验证授权/时间过滤与未来文档不改可见词法排名；context_budget_citations_exclusions_and_final_validation、reranker_is_bounded_to_authorized_candidates、scoped_handle_get_history_write_and_selector_permissions、query_limits_and_cancellation_never_become_empty_success 验证装配、授权和预算。

NativeRagStore 的26项另覆盖 source 原子替换、native CAS 整批回滚、票据竞争、幂等收据、退休、防复活、授权、原生 scan 取消、快照/投影篡改拒绝。该有界 API 保持自己的明确能力限制，不把 MemoryStore 的治理功能假称已加入它。

## G01–G12 责任与切换门槛

| 要求 | 底座提供 | 仍由游戏保留和验收 |
| --- | --- | --- |
| G01–G02 提交事件/outbox | 可靠批写、稳定 source、幂等内容去重 | SQLite 同事务提交、pending/indexed 确认与重放 |
| G03–G05 世界/角色/私有见闻 | 精确账户/scope/owner/来源过滤、受限句柄 | UUID、观察投影、audience 与 GM 私有边界 |
| G06–G08 清除/墓碑/领域状态 | 清理屏障、防复活、来源删除、retire | 事件/观察复核、世界锁、清除时间、NPC 级联及有效承诺 |
| G09–G10 时间单位/场景预算 | 显式 clock_id、原数值区间、引用上下文与来源排除 | 游戏小时/观察秒转换、关注筛选、近期事件去重 |
| G11–G12 工具/规则/能力 | 宿主铸造的受限 namespace 句柄 | 工具白名单、active/always 规则、遗忘不恢复能力 |

这些要求已有源码审计映射；没有执行全游戏回归。SDK 测试不能证明游戏投影、工具授权或业务级联正确。主会话在公共底座整合验收后统一通知 WordGame；本会话不通知、不修改其源码。

## 真实 KaLM 小样本

使用 WordGame 既有 KaLM-Embedding v2.5 Q8_0，896维、mean/L2，权重摘要 7e7622f29b1a2b78d609b95be1f2961f7fa7640d870e1e60c4d36af989c8ed4d。查询变换记录实际 QUERY_INSTRUCTION 的 SHA。无下载、无训练，无第二套模型管理。

Windows RTX5090 CUDA，25层卸载，状态 ready。临时库两条合成中文资料，“铁匠铺在哪里？”首条命中铁匠资料，cosine约0.670；词法/混合也命中。包含启动的该用例约3.81秒。这证明接口和真实推理链路可运行，不能证明广泛中文质量或与旧系统效果相当。Linux 没有执行真实模型推理。

测试用真实 Pydantic 模型及原 HTTP 路由，但 store 替换为临时门面，账户依赖固定为合成账户；日志重定向本工作树 build/rag-wordgame-logs。未打开业务数据库、未扫描玩家资料、未改业务源码。

## 896维性能样本

1000条合成记录、相同单位向量、每128条可靠提交；每模式一次预热，10次测量。查询 top8；计时不含模型推理。样本脚本 tests/rag/benchmark_memory.py，原始数据 build/rag-benchmark-{win,linux}.json。Linux 为同机器 Docker，不当作独立硬件对比。

| 环境 | 可靠写入 条/秒 | dense p50/p95 ms | lexical p50/p95 ms | hybrid p50/p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Windows | 275.2 | 5.63 / 5.84 | 18.15 / 18.83 | 35.48 / 36.25 |
| Linux Docker | 413.7 | 3.98 / 4.11 | 13.01 / 13.31 | 25.22 / 26.53 |

原生 dense 查询 p50 为 Windows4.82ms、Linux3.29ms；RAG 多出约0.81/0.70ms，包含权威内容/版本校验、生命周期复核与最终回包预算。两者工作职责不同，该对照不是旧系统替代速度比较。磁盘文件合计约15.2MB，非压缩后稳态，也未测峰值RSS。10次样本的p95非常有限，不能外推大规模或尾延迟。

未对旧 zvec 做同条件比较，未测试10k/50k/百万行或 ANN，未证明对旧系统无性能退化。默认 exact 对符合预算的过滤集合给出精确结果；更大负载需要目标规模测量，不能用此小样本承诺高性能替换所有规模。

## 自审修复与保留限制

- 修复账户目录父路径创建；38项最初全失败的环境问题已定位并修复。
- 1000 owner 原子批写触及默认 delta 预算，只在 RAG 自有 Store 设16MiB delta；未拆批掩盖部分成功，未改全局写策略。
- 审计快照遵守来源清理屏障；目录跨来源/谱系校验；模型变更、元数据缺失明确拒绝。
- 回调后重新校验当前权威版本；未授权重排 ID 拒绝；词法阈值下有界候选不能证明无结果时不返回 proven_none。
- 完整检索回包（含上下文）纳入结果字节预算；空编辑ID拒绝；补算 cosine 与投影版本上限校验补齐。最终 Windows/Linux 各38项通过。
- 清理是逻辑删除，旧 WAL/快照/原生 tombstone/外部备份可能留存，未做安全擦除。
- 取消在模型/提交边界生效，不能抢占任意宿主回调。账户重开自动恢复任务可能先做必要可靠清理。
- NativeRagStore 与 MemoryStore 是不同 API/保留空间，不混用它们的记录、收据或能力。
- 无迁移、无新模型、无 ANN 接入、无新 Rag C ABI/多语言绑定；macOS/Android/iOS 未运行。正式 WordGame 切换后仍须实际游戏验证。

## 组合 SDK 导入补充验证

在上述验收之后新增私有 _bindings.py，将 Core/Index/wire 导入统一为同 SDK 的相对导入；顶层独立源码模式保留绝对导入。RAG 不修改 sys.path 或创建 sys.modules 全局别名，组合依赖异常直接传播，不借机选取其他项目 wrapper。library=None 的原有参数保持不变，动态库自动发现由公共包装负责人提供和验收。

tests/rag/test_sdk_imports.py 在全新子进程组装 FengquDB/bindings/python 布局，并放置会报错的同名顶层模块，验证实际使用同包 Store/Error/Index/vector 且没有顶层别名。使用真实原生库执行批写、事实转换、dense/lexical/hybrid、history 与谱系删除；同一脚本也验证独立源码模式。Windows/Linux 各2项通过，无跳过；这是新增4次相关执行，没有重复模型或整套回归。该阶段证据独立保存在 build/rag-sdk-import-verification.json，不将原132次执行冒充新代码的全量重跑。

## 最终冻结版回归

依主会话通知，在其最终冻结的 Windows Core/Index 及 Linux gcc11/glibc2.31 便携库上集中执行一次相关回归。Linux 使用 index-portable/libfengqudb.so.2 与 libfengqudb_index.so.1，不使用先前 index-linux 构建。四个库的 SHA-256 与负责人提供值完全一致，测试前后的库及 wrapper 摘要也一致。主会话自行组装 artifacts/foundation/python/FengquDB 并验证自动 native 定位；本轮测试显式传库路径。

| 平台 | MemoryStore | 来源/分块 | SDK/门面布局 | 本轮总计 |
| --- | ---: | ---: | ---: | ---: |
| Windows x64 | 38/38 | 26/26 | 2/2 | 66/66 |
| Linux x64 便携库 / Docker | 38/38 | 26/26 | 2/2 | 66/66 |

本轮132次执行全部通过、无跳过。SDK用例现包含 FengquDB/integrations/wordgame_rag 门面相对导入、同一 MemoryStore/RagError 类型、真实写入/get及跨账户AppError转换；同名顶层decoy确保不发生全局别名或错误模块选择。独立源码模式仍通过。已有真实模型和HTTP结果按要求沿用，未重跑。

最终版仍按1000条896维合成记录、可靠128条批次、每模式预热后10次热查询采样；没有模型推理时间，没有zvec比较，不能外推更大规模。

| 平台 | 可靠写入 条/秒 | dense p50/p95 ms | lexical p50/p95 ms | hybrid p50/p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Windows | 1672.9 | 4.98 / 5.36 | 17.51 / 19.45 | 34.85 / 36.01 |
| Linux Docker / 便携库 | 2029.3 | 3.40 / 3.70 | 11.72 / 12.01 | 23.47 / 24.10 |

磁盘文件合计两平台均约12.73MB，非稳态压缩或峰值资源。结果较首轮有变化，但10次微型样本不足以独立归因每项原生优化；不声称所有负载都获得同等提升。原始结果为 build/rag-benchmark-{win,linux}-frozen.json。输入摘要 build/rag-frozen-inputs.json；最终机器清单 build/rag-frozen-verification.json，日志 build/test-{memory,sources,sdk-imports}-{win,linux}-frozen.log。无新模型、迁移或正式WordGame通知。
