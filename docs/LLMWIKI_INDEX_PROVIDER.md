# FengquWiki 与共享 NativeIndex 的契约

当前生产默认是 NativeIndexProvider，缺原生运行库明确失败，不回退到 Python 搜索。生产 llmwiki 不包含独立 BM25、分词、ANN 或向量计算算法。测试对照仅在 tests/reference_wiki_index.py，由旧对照用例显式注入，不打入 SDK。

IndexDefinition 和 IndexProvider 保留为宿主注入的契约；仅接收 atomic_updates=True 的可信实现。设置这个布尔值本身不能证明事务能力。宿主不能通过更改名称认领另一份格式；schema 或 index/provider 身份不匹配时使用新 namespace。

| 方法 | 约束 |
| --- | --- |
| upsert(tx,id,document) | 使用调用者已有事务更新领域投影和共享索引 |
| delete(tx,id) | 删除/墓碑和领域撤销在同一事务完成 |
| set_page_state(tx,id,state) | Wiki 决定 stale/ready；Index 镜像可见性 |
| get_page_state(view,id) | 在指定快照读取发布状态 |
| get_document(view,id) | 在指定快照读取投影元信息 |
| statistics(view) | 同快照统计，不能假造跨视图水位 |
| search(view,query,**options) | mandatory visible 过滤、预算、位置及版本化物化 |

Provider 不自行打开、提交或关闭 Store。View.prefix 已带 Wiki namespace；Native 默认 partition 来自该前缀摘要，由受信任宿主指定 index_space/partition，不是用户认证机制。

Wiki 拥有不可变原文、分块、来源版本、页面规格、任务、实体/事实/冲突、人工修订、引用与问答一致性。共享 Index 拥有 Analyzer、倒排、字段检索、评分、向量/ANN、影子图重建和旧图回收。唯一生产文本帮助函数是 text.chunks，按 Unicode 字符切片保存原文范围，不做分词或检索。

Native 文档稳定 ID 为 c-<source-id>-<ordinal> 和 p-<page-id>。每个投影有独立 projection_version/lifecycle_generation；完整领域 SHA-256 通过持久 token 无损映射到 wire uint64，并在 locator/metadata 保留完整哈希。普通更新 upsert；ready→dirty 为 metadata-only patch；删除/复活才推进生命周期。

查询在调用者同一 View 上执行并物化。领域正文仍为权威来源，不用 native payload 替代原文。结果核对投影、领域版本、定位器、可见性、正文哈希和命中字节范围。新查询/维护参数、维护游标和组合 SDK 导入见 [当前完整契约](LLMWIKI_FOUNDATION.md)。

ranking_kind、termination、index_coverage、match_evidence 互相独立。mode=2 可以 exact 回退，不能由请求推断结果排名类型。近似空结果不是没有匹配的证明；只有执行与覆盖完整的 proven_none 才确认空结果。预算/取消/term_limit 保留在 truncated 和 degraded_reasons 中，不能升级为“已查遍”。

Native 当前不是完整 Unicode NFKC/casefold；对照实现更广的 Unicode 规范化不作为原生检索能力。对照只服务验证，不是生产 fallback。接口、源文件归属及真实测试见 [Native 接入](LLMWIKI_NATIVE.md) 与 [验证记录](LLMWIKI_FOUNDATION_VALIDATION.md)。
