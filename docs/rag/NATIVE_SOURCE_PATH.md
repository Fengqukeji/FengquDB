# 已实现的原生 Index 来源与分块路径

状态：2026-11-14，RAG 专属有界 source/chunk API 已实现，Windows/Linux 真实共享 Core/Index 集成通过。本文限制仅适用于 NativeRagStore；另已实现完整治理的 MemoryStore，见 [API.md](API.md)、[ACCEPTANCE.md](ACCEPTANCE.md)，不能将两条 API 的功能混为一谈。

## 1. 依赖与归属

本模块位于 `bindings/python/fengqudb_rag/`。直接导入并复用 Wiki 会话拥有的 `fengqudb_index.py`、`fengqudb_index_wire.py`，不复制其内容、不另编解码 Index Protobuf；权威协议为 Index 工作树的 `proto/index.proto`。

依赖共享 Index ABI1、Core ABI2/公开桥接 ABI1。Core 和 Index 必须是同一份实际运行库；NativeIndexProvider.attach 使用公开 lease 核对 Core bridge 地址和 Store token。模块不另开 Store、不拥有传入 Store 的 close，也不实现分词、BM25、向量距离、ANN 或模型推理。

本轮仅新增 RAG 包、RAG 测试和本文档；此前已整合的 Core/桥接文件保持原内容。无共享 wrapper/header/Core/根 CMake 改动。

## 2. 实际接口

```python
from fengqudb import Store
from fengqudb_index import NativeIndex
from fengqudb_rag import Chunk, NativeIndexProvider, NativeRagStore

space = b"example-synthetic-3d-v1"  # 正式应用应使用完整向量契约的稳定身份
client = NativeIndex(index_library_path)
with Store(save_directory, library=core_library_path) as store:
    rag = NativeRagStore(store, NativeIndexProvider(client),
                        tenant_id="alice", vector_space=space, dimension=3)
    ns = rag.namespace("game:world-uuid", "npc:stable-key", clock_id="game-hours-v1")
    ticket = rag.prepare_source(ns, "event:stable-id",
        [Chunk("已提交且此 NPC 明确知道的可见结果", known_at=1, kind="event")],
        expected_revision=0, operation_id="event:stable-id:projection-v1",
        source_revision_label="opaque-label-v1")
    # 此处可以由宿主在数据库事务外计算向量；已重放/未变票据不必再次推理。
    if ticket.replay or ticket.unchanged:
        saved = rag.commit_source(ticket)
    else:
        saved = rag.commit_source(ticket, vectors=[[1.0, 0.0, 0.0]], vector_space=space)
    result = rag.search(ns, as_of=4, query_vector=[1.0, 0.0, 0.0], query_space=space)
    assert result["ranking_kind"] == "exact"
    assert result["termination"] == "finished"
```

宿主负责部署共享 Python wrapper 到可导入位置；本包不根据其他工作树路径自动扫描、拷贝或安装依赖。示例向量是合成值，不代表实际语言模型。正式 space 应绑定文档/查询变换、模型来源、维度和归一化契约；此最小 API 只核对显式 space bytes 和维度，不声称已实现完整 EmbeddingProvider/query profile 注册。

其他入口：`replace_source`（prepare+commit 简写）、`get_source`、`delete_source`、`operation_status`。Namespace 只能由可信宿主为当前账户/世界/知情人创建；这些字符串不是认证凭据，不能直接从 LLM 参数授予跨 owner 访问。

## 3. 来源、版本和原子性

- 一个 Store 绑定一个 RAG tenant 和 collection/vector 契约，重开时校验。已有其他模块数据可共存，但 RAG 保留键空间仅由本包写入。
- scope/owner/timeline 的规范身份摘要作为硬 partition；原字段保留并复核，所有检索还有强制类型化 Predicate。clock 是 namespace 固定契约，小时/秒不自动猜测。
- prepare 捕获来源修订和生命周期，再在 commit 的新事务重新校验，阻止 Embedding 期间发生替换/删除后旧票据写回。
- 一次来源替换的来源头、新分块、旧分块清理、Index 投影/向量/倒排、操作收据均在同一事务提交，显式 force_reliable=True。Index stage 失败时取消/关闭整事务，不保留半批来源或半套索引。
- 每次替换使用新分块 ID，Index 的旧分块变成无正文/向量的 tombstone，canonical 旧分块删除。旧来源版本计数保持递增，但**没有实现编辑历史或稳定分块 ID 的原位纠错**。
- `source_revision_label` 是任意追溯字符串；Index 的数字 source_revision/domain_revision 使用独立来源发布号，不把字符串强转为数字。
- operation_id 与请求摘要同事务保存。完全相同操作可重放；同 ID 异 payload 报冲突。已被后续替换或删除的旧操作返回冲突，不重新发布原内容。
- 正文/元数据完全相同的新操作可生成新收据而保持来源版本、分块和向量不变。调用方可据 ticket.replay/unchanged 跳过模型。

默认一次至多 64 块，可设置 1–128；每块至多 1800 字符、标题 200 字符，合计文本至多 512 KiB。超限在写入前拒绝，不拆成部分成功的大任务。Index/Store 自身预算仍可能使事务整体失败。收据和 tombstone 目前保留，没有自动全局磁盘配额或 GC，不能把单次有界描述为整个 Store 永不增长。

delete_source 是带 CAS 的来源退休，删除其全部 canonical 分块和 Index 可见内容，提升来源代数并保存最小 tombstone。同一删除 operation_id 可重试；不影响其他来源。**退休 source ID 不可复活**，恢复写入需新的 source identity。clear_owner、可恢复大批 purge 和 namespace 退休尚未实现。

COMMIT_UNKNOWN/RECOVERY_REQUIRED 不当作回滚：当前对象封锁后续 RAG 访问，宿主应关闭并恢复/重开原 Store，再通过 operation_status 判断此前操作。已实现该错误处理分支，但本轮未做进程中断或磁盘故障注入，不宣称完整恢复矩阵通过。

## 4. 检索和最终复核

默认 dense 精确余弦检索，调用方必须给 query_space 和预计算 query_vector。dense 不发送 text/match_all_terms；lexical 不发送 vector/vector_space/min_similarity。min_similarity 验证范围 [-1,1]。

授权选择器、source 限制、`known_at <= as_of < expires_at` 在 Index 候选选择前执行。公共 owner 的跨 partition 并集尚未实现，include_public=True 明确拒绝。空 owner 自身可作为一个独立公共 partition 使用，不代表所有 owner。

结果必须同时具有 finished、complete、exact、匹配的 snapshot_version，以及 has_hits/proven_none 证据。预算、取消、落后、未知空结果不包装成正常 []。结果物化在同一快照中读取 canonical chunk/source，并核对 ID、来源版本、投影版本、内容 hash、文本、locator、元数据和向量空间。

发布前在共享生命周期门控下重新开当前快照，复核来源仍有效、版本/代数/分块成员和正文未变；任意命中已被替换或撤销则整次报 RagStaleRead，不丢掉部分命中后冒充完整结果。查询期间删除其他来源不隐藏保留来源。

门控按同一 Core/Store token 在本进程的本模块实例间共享。它协调所有通过本包进行的来源变更和结果发布，不对直接绕过包修改保留键空间的代码提供认证保证。已经返回给调用方的结果不能撤回。

QueryBudget 控制原生 posting/candidate/scan/memory/deadline，另限制最终回包字节；取消传入真实共享 QueryControl，等待生命周期锁也受取消/截止时间约束。此路径不承诺写入阶段可即时取消，也不自动触发模型或网络调用。

严格历史 BM25 统计不受 Index v1 支持，默认严格模式明确报 RagUnsupported。只有可信调用方显式传 `strict_statistics=False` 时才允许分区统计的 lexical 搜索，并回 `statistics_policy="partition_unfiltered"`；此模式的评分统计可能含同 partition 的未来文档，不能用于声称严格历史语义。策略开关必须是真正 bool，None/0 等不能无声关闭严格模式。

## 5. 当前拒绝的能力

事实谱系/新旧事实自动闭合、原位纠错/审计历史、原 WordGame MemoryStore 门面、长文自动分段、EmbeddingProvider 模型进程管理、owner/public 合并、严格动态 BM25、ANN/混合/重排、namespace 批清理/退休、旧 zvec 数据迁移，均未在本路径实现。

`put_fact`、`retire_namespace`、hybrid/ANN 模式、include_public=True 等有明确异常；不存在将这些请求静默降成普通来源覆盖或词法检索的行为。现有 WordGame 没有被切换。

## 6. 验证与整合文件

此路径源码为 `bindings/python/fengqudb_rag/{types,native,sources,__init__}.py`，包入口还导出已实现的 MemoryStore 及其模块，部署须包含整个包；测试为 `tests/rag/test_native_sources.py`。继续复用公共 wrapper，不维护 RAG 内部副本。

Windows x64 与 Linux x64 Docker 各执行 **25 项整批真实集成测试 + 新增策略 bool 校验 1 项定向测试，全部通过、无跳过**。覆盖来源原子替换、真实 native CAS 失败回滚、两个 Embedding 票据竞争、重试去重、来源删除/重开、租户/空间/归属/时间过滤、回包篡改拒绝、真实 native scan 取消、锁等待截止时间、查询期间替换/删除的最终复核和未实现特性拒绝。

```powershell
python tests/rag/test_native_sources.py --core D:/study/FengquDB-index/build/index-win/Release/fengqudb.dll --index D:/study/FengquDB-index/build/index-win/Release/fengqudb_index.dll --shared-python D:/study/FengquDB-llmwiki/bindings/python -v
```

Linux 使用共享 index-linux 的 Core/Index，Wiki wrapper 目录只读挂载，业务测试库在容器临时目录。测试关闭 Python bytecode 写入，不修改 Wiki 的缓存/源码。没有训练、模型效果评测、性能基准、完整 WordGame 验收或新 C# 绑定；超过 10 小时的教师蒸馏仍排除。
