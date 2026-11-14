# RAG 类型与 EmbeddingProvider 契约

状态：保留早期接口设计。运行实现与本轮范围以 [API.md](API.md)、[ACCEPTANCE.md](ACCEPTANCE.md) 为准；MemoryStore、CallbackEmbedding 和现有 KaLM 注入已经实现并验证。以下部分原生类型/迁移设想并非本轮交付，旧数据迁移已按用户要求取消。

## 1. 类型边界

```text
MemoryIdentity = {tenant, collection, scope, timeline, owner, record_id}
MemoryRevision = {business_revision, projection_version, payload_digest, content_hash}
LogicalPeriod = {clock_id, known_at: i64, expires_at: optional i64}
SourceIdentity = {source_id, source_revision_label, optional content_hash}
WriteTicket = opaque(payload_digest, captured_epochs, captured_revisions, contract_id)
OperationReceipt = {operation_id, payload_digest, store_uuid, commit_version, outcome}
```

类型需区分 StoreVersion、BusinessRevision、LogicalTime 和墙上 UTC 时间。不能把 as_of_hours、NPC cutoff_seconds、commit_version 都作为一个无说明的 timestamp。WordGame 保留小时/秒的现有边界，转换由业务显式执行。

WriteTicket 属于本次原生 handle 的不透明对象，不能由 LLM/HTTP 指定 revision/epoch 来制造。过期、关闭或跨 Provider 合约的票据失效；同一事务里再次验证，Embedding 本身不使写入合法。

## 2. Provider 协议草案

```python
class EmbeddingProvider(Protocol):
    def contract(self) -> ProviderContract: ...
    def embed_documents(self, texts: Sequence[str], *, context: RequestContext) -> EmbeddingBatch: ...
    def embed_queries(self, texts: Sequence[str], *, context: RequestContext) -> EmbeddingBatch: ...
    def status(self) -> ProviderStatus: ...
    def warmup(self, *, context: RequestContext) -> ProviderStatus: ...
    def close(self) -> None: ...
```

这是形状草案，不要求业务继承具体类或安装新依赖。既可通过 Python Protocol/duck typing 注入，也可由应用语言的等效接口实现。首个适配器复用 WordGame `embed_with_provider(texts, query=...)`；原有 provider 的进程关闭由原有宿主管理，包装器不得误关闭其他图标消费者共用的模型。

```text
ProviderContract = {
  contract_version,
  document_space: {
    producer_identity, weights_digest_or_remote_version,
    dimension, output_dtype, pooling, normalization, metric,
    tokenizer_identity, document_transform_id,
    legacy_identity?, provenance_status
  },
  query_profiles: [{id, transform_digest, compatible_document_space_id,
                    compatibility_provenance}],
  input_limits: {max_items, max_text_chars?, max_tokens?, tokenizer_id?},
  capabilities: {device_modes, batching, cancellation, deterministic_hint}
}
RequestContext = {deadline_monotonic, cancellation, request_id,
                  max_items, max_input_bytes, allow_remote=false}
EmbeddingBatch = {space_id, query_profile_id?, input_digests,
                  vectors_in_input_order, measured_device, duration,
                  fallback_reason?, usage?}
```

MUST：返回条数和顺序与输入对应，输入摘要绑定实际送模型的规范文本；禁止部分成功却返回普通 batch。SDK 若拆分长批次，完成所有分段后才返回完整结果，或明确失败并丢弃未提交结果。服务商支持显式逐项 ID 时依 ID 排序校验，不信任远端数组天然有序。

文档与查询变换分开：文档不加查询检索指令；适配器不得在旧 WordGame Provider 已加指令后再加一次。Provider 的预处理、截断和 tokenizer 版本属于契约，不允许遇超长文本静默截断后仍用原文摘要登记向量身份。

## 3. 身份与迁移兼容

向量空间用完整规范契约计算稳定 ID，不能只由显示名/维度确定。远端 Provider 无权重 SHA 时保存稳定部署/版本身份及 provenance，不伪造“已校验字节”。

旧 KaLM IDENTITY 包含模型摘要、pooling、L2 和 retrieval-v1，但没有历史查询模板正文哈希。当前使用新库，在模型契约加入实际查询指令摘要；不导入旧向量或将未知历史参数重标为当前配置。

查询模板变化后，用既有 document space 做独立 query compatibility 评测，再显式注册新 query_profile。模型权重、文档变换或维度改变需要新 document space 和显式重嵌入；同维度不能直接混用。向量量化是 Index 的存储配置，不能改变模型来源身份或冒充模型压缩。

缓存 key 至少包含 document_space_id、用途(document/query)、query_profile_id、规范输入摘要。缓存只是计算复用，不构成授权；不存玩家原文的全局无界缓存。重复 MemoryWrite 在 Provider 调用之前命中持久去重，不因进程重开必须重新嵌入。

## 4. 数值验证

- 拒绝维度不符、非数值、bool、NaN/Inf、全零向量及数量/索引缺失；不能让 Python bool 当合法 float。
- 输出 dtype FP32，转换后再次检查 finite；极大 F64 转 FP32 溢出须报错或在既定归一化规则下先安全缩放。
- unit_l2 使用缩放/稳定范数，避免大数平方溢出和极小数下溢成零；验证 norm 容差。
- Index 在 C ABI 再做维度/finite/契约验证，不仅信任 Python Provider。
- 历史迁入 FP32 不反复归一化造成不可解释漂移；合法容差、原向量校验摘要与修改原因分开记录。

## 5. 设备、取消与错误

Provider 默认懒加载。status 不触发模型加载，warmup 为显式操作；不要求基础检索包下载/携带 KaLM 或其他大模型。应用可使用预计算向量、已有本地 Provider 或显式远端 Provider。

auto 的设备计算故障允许 CPU 回退；强制 cuda 失败报错。输入/契约/HTTP 参数错误不触发设备切换；回退保持相同空间与变换，不悄悄换成另一个小模型。日志不记录正文、完整查询或服务密钥。

取消分两层：宿主 Provider 若支持物理中断则执行；若旧实现不能中断已在 GPU/CPU 内运行的计算，SDK 检查 deadline/cancel 后拒绝结果进入 commit，status 明确此能力限制。不能把“结果被丢弃”描述成“计算立即停止”。取消发生在 Store WAL 开始后时，返回真实提交结果或结果未知，不能承诺回滚。

错误类型最少区分 InvalidInput、ContractMismatch、ComputeUnavailable、DeadlineExceeded、Cancelled、MalformedOutput。RAG 将其映射现有 AppError；不因模型不可用返回成功空检索或修改存储契约。

## 6. 体积与蒸馏约束

基础模块无强制模型；WordGame 首期复用已有 KaLM，保持现有检索对照。小模型由主会话独立评估，不作为数据迁移前置，RAG 不重复下载、量化或训练。主会话完整教师蒸馏工程预算约 12–24 小时（非实测），超过用户 10 小时上限，本轮已排除，不启动训练。

若后续出现新学生模型，作为新 space 重建副本并用同语料评测；不能把模型体积减少比例当作质量损失比例，也不能用不同模型的 ANN Recall 混淆模型语义质量。当前没有已验证的小模型替换结论。
