# RAG / FengquIndex 契约测试设计

状态：保留早期场景规范，实际已实现/执行的测试与取消范围见 [ACCEPTANCE.md](ACCEPTANCE.md)。Windows/Linux 真实原生 MemoryStore 与 source/chunk 用例、Windows WordGame 模型/路由/真实 KaLM 已执行；下文更广的目标不表示均已通过。

## 1. 分层执行与替身边界

| 层 | 可用替身 | 必须真实验证 |
| --- | --- | --- |
| RAG 请求编排 / Provider | 可记录参数的 Index 替身、确定性向量、可阻塞 Provider | 强制过滤不可被用户覆盖、输入映射、完成性处理、取消后不提交 |
| 原生治理 | 确定性已算向量；Index plan 使用真实或失败注入入口 | 同 Store 事务、事实关闭、投影 etag、生命周期、恢复及 receipt |
| 公共检索集成 | 无模型，手工已知 FP32 向量与合成文本 | 真 Index 过滤/快照/水位、版本读取、exact oracle、预算及删除 |
| WordGame 适配 | SQLite 隔离副本；先假 Provider 后真实 KaLM | outbox/观察/cutoff/墓碑、旧接口及真实中文效果 |

编排替身只证明调用方构造了正确请求，不证明 Index 强过滤正确。集成必须运行真实库。静态类型检查也不代替持久化故障测试。

## 2. 固定合成数据

用两 tenant A/B、collection M、scope W1/W2、timeline main/fork、owner gm/npc:p/npc:q/公共空 owner；它们不是玩家数据。

主样本：A/W1/main/npc:p 的旧住址在 t=1 生效，新住址 t=5；另有 t=20 的未来事件、npc:q 的最高相似私有文本、B 的同 ID 文本、W2 同 ID、fork 同 source 文本以及公共资料。向量取三维单位向量/可手算点积；ID 按外部字节顺序固定，能构造超过 top_k 的无权高分干扰记录。

时钟固定 logical-hour，另造 logical-second cutoff，用于验证适配层显式转换。Store 提交版本取真实回执，不以 t=1/5/20 充当版本号。

各用例使用独立 Store/fixture 子集；时间闭合等断言限定到住址事实，跨权限干扰仅在相应用例加载，避免其他合法样本污染“只有旧/新记录”的预期。

## 3. 公共接口用例

下列 Q 编号对应 [检索契约](RETRIEVAL_CONTRACT.md)；括号内为现有 WordGame 需求编号。

| 用例 | 操作 / 注入 | 必须断言 |
| --- | --- | --- |
| Q01 标识隔离 | 相同 external ID 在 tenant/scope/timeline 重复，使用受限 handle get/search | 只得到授权空间；内部 DocId 不能泄漏为业务 ID（R01–03/G03） |
| Q02 过滤后 top-k | 插入 100 条无权高分、3 条有权低分，top_k=3 | 三条有权结果均可召回；不能全局 top-k 后过滤为空（R03/R21/G04） |
| Q03 时间闭合 | 旧址 t=1，新址 t=5；查 as_of=4/5，旧 record_revision 保持不变 | 4 只旧、5 只新；旧 projection_version 推进且不用重嵌入（R04/R08） |
| Q04 投影来源 | 同 event source 给 GM 与 npc:p 不同正文 | npc:p 的 hit、span、解释和 versioned get 均无 GM 文本（G04–05） |
| Q05 原子 stage | 在 RAG canonical staging 后、Index stage 中注入 LIMIT/IO，尝试提交 | 整批不发布；不能出现有正文无过滤投影或关闭旧事实但缺新版（R16） |
| Q06 提交未知 | reliable commit 注入结果未知，重开查 operation_id | 依真实 receipt 决定是否重试；outbox 不提前 ack，不假设必回滚（R16/G02） |
| Q07 防复活 | Provider 阻塞，另线程 source purge/lineage delete，随后 Provider 返回 | 旧 ticket 被拒，旧图节点不可查；其他 source 的保留记录仍可见（R12–15） |
| Q08 分批清理 | 第 129 条所在批失败并重启 | 已完成批次不恢复；剩余仍受屏障阻断，可幂等续跑（R17/G07） |
| Q09 元数据更新 | 仅变 expires_at、source revision 标签或 searchable | 推进 projection_version、更新过滤和回包；正文与向量不重新编码/嵌入（R09/R14） |
| Q10 同快照 | S 获取候选后另一事务更新正文；以原 ref 批读 | 读到 S 的原正文或明确 snapshot/ref unavailable；不混最新正文（R10/R11） |
| Q11 增量覆盖 | 图 I 后提交新增/更新/删除至 S，保持图未追平 | I<=S+完整delta 可 complete_at_S；新增可查，删除/旧 revision 排除 |
| Q12 未来基准/缺口 | 图 I>S 或回收 `(I,S]` 所需 delta | exact 完整路径或明确拒绝；不标完整、无未来数据；不跳过 history gap |
| Q13 完成性 | 分别制造 complete empty、ANN 正常完成、budget partial、cancel、stale | 前两者可按策略使用，后三者不是普通成功空列表；兼容门面抛明确错误 |
| Q14 等待与取消 | wait_until 在构建积压时超时/取消；随后构建继续 | 等待不阻塞写入/构建，不泄露 handle/pin；deadline 有实际作用 |
| Q15 空间验证 | 同维度不同 space、query profile 未兼容、零/NaN/Inf/超长向量 | 明确错误；无降级换模型/换词法，不写入半条向量（R22/R25） |
| Q16 分数 | 可手算的相同/正交/反向向量，min_similarity 边界 | similarity 为 1/0/-1；阈值包括等号；不把 distance 或 RRF 当余弦（R20） |
| Q17 强历史词法 | 固定 as_of，加入其他 owner/未来高频文本 | 严格 BM25 模式的语料统计与结果不受其影响；未支持就拒绝严格模式 |
| Q18 摘录与偏移 | 中文/emoji/组合字符/路径符号，经 Analyzer 归一化 | span 是原字段有效 UTF-8 byte 区间，版本正确、无越界/切坏字符 |
| Q19 游标 | next page 换 owner/source/space；重建后复用旧 cursor | 绑定选择器；按策略明确 stale，不能扩大权限或重复/漏页冒充完整 |
| Q20 短路径资源 | 小 owner 位于大 tenant 中；大量相同正文和历史 | 候选、临时内存、返回字节受预算约束；不能只限制输出条数 |
| Q21 开关 | 未启向量模块时 lexical/dense 各调用 | lexical 可用，dense 明确 unavailable，不静默用另一模式 |
| Q22 更新删除排序 | 查询执行中发生可靠删除，再物化/发布 | 删除屏障先完成则旧结果重试/取消；已发送的旧响应不声称能撤回 |
| Q23 ANN 空结果证据 | 图返回空，授权集合中实际有满足阈值的文档；另造真正零授权候选 | 前者 exact 验证后返回命中或明确预算错误，不能成功空列表；后者 proven_none 可正常 [] |
| Q24 公开原生租约 | Python transaction/snapshot borrow 与另一线程 close 交错，跨 Store 传 lease | 借用期无悬空指针，跨库/已关闭拒绝；不读私有 ctypes 字段 |
| Q25 版本幂等与溢出 | 同 projection_version 同/异 digest 重放；旧删除晚到；最大版本推进 | 同内容幂等、异内容冲突、旧版本不覆盖新；溢出拒绝，墓碑/图重建不重置版本 |

Q01–16、Q19–25 是 dense 替换的接口/治理必需项；Q17–18 在启用词法及摘录时加入门槛。ANN 正常完成的近似排名要另测相对 exact 的 Recall，不能据 Q13 表达正确就宣称召回足够。公共命名映射和缺口复核见 [协作复核](COORDINATION_REVIEW.md)。

## 4. Provider 与类型用例

| 用例 | 输入 / 注入 | 断言 |
| --- | --- | --- |
| P01 文档/查询 | 记录实际调用文本和 query 标志 | 文档无查询指令；query 只加一次；批次按原顺序对应 |
| P02 重复与重开 | 重试完全相同 MemoryWrite/import，关闭再重开 | ID 不变；持久去重先于 Provider，不调用 Embedding（R05–06） |
| P03 坏输出 | 缺条、乱 ID、bool、NaN、Inf、零向量、F64→F32 溢出 | 拒绝整批；无 canonical/Index 变更（R25） |
| P04 安全归一化 | 极大/极小有限非零值 | 稳定归一化或明确不支持；不能得到零向量却成功 |
| P05 设备策略 | auto 计算故障、强制 cuda、坏输入 | 仅 auto 的设备故障可回 CPU；空间身份不变（R24） |
| P06 取消能力 | 旧 Provider 不能立即停止，deadline 先到 | 即使计算后来返回也不提交；不声称已物理中断 |
| P07 合约演进 | query 模板变更 vs 文档模型变更 | 前者需显式兼容注册，后者新 space；unknown provenance 不伪造完整身份 |
| P08 生命周期 | 图标与 RAG 共用 Provider，关闭 RAG 包装器 | 不误关宿主共用模型；owned/external 生命周期明确 |
| P09 无模型 | 只用预计算向量或 lexical | 不下载模型、不启动训练/推理进程；status 不加载权重 |

## 5. 组合调用与 WordGame 断言

- Q05/Q06 与游戏事务回滚、outbox 分 owner 部分成功重试结合：SQLite 未提交时不能索引；RAG 未确认时不能 ack（G01–02）。
- Q07/Q08/Q22 与 NPC cutoff、event/observation 删除、同名新世界结合：外部索引滞后也不能召回旧观察（G03/G06–08）。
- Q03 与小时/秒、fork 结合：单位不猜测，被放弃分支的未来事件不能改标过去（G09）。
- Q10 与 scene 去重/关注/字符预算结合：原有规则和能力授权仍由游戏决定（G10–12）。
- HTTP/工具/管理页保持 R26，unknown/partial 不伪装 code=0 的空列表；scene 捕获错误时 memory_status 仍可追踪。

## 6. 执行与报告

先按真实公共签名实现最小适配测试，再集中运行相应层。RAG 和 Index 可共用这些场景编号与合成数据，检索 oracle 放 Index 侧；RAG 不复制 Index 算法作为测试依赖。最小接口 smoke 不代表完整差分或真实 KaLM 评测。

结果报告分别列 designed / implemented / executed / passed，附 source/library 哈希、运行命令、平台、未支持项和故障注入位置。当前 Q24 对应的 Store 身份/公开租约子路径已在独立脚本实现并于 Windows/Linux 执行通过，见 [原生桥接](NATIVE_BRIDGE.md)；其余 Q/P 及实际 Index 集成仍属 designed，不能将桥接通过当作 RAG 功能通过。
