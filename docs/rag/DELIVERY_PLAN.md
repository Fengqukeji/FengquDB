# 交付与 WordGame 接入

当前 API 与证据见 [API.md](API.md)、[ACCEPTANCE.md](ACCEPTANCE.md)。本文件替代早期迁移/多语言 Rag 库路线。用户已取消旧数据迁移；没有迁移工具，不处理真实旧库，不训练或下载新模型。

## 底座交付

通用 MemoryStore、事实/审计治理、可靠清理、强过滤、原生 dense/BM25、RRF、注入重排、引用上下文、现有 KaLM 注入与兼容门面已实现。公共桥接、Index、共享 wrapper 由公共模块负责人维护，RAG 不复制实现。

交付源码、测试、示例及带摘要的本地验收清单。当前分支不提交/推送；主会话统一集成 Index、Wiki、RAG 后再协调 WordGame。

## 后续接入步骤

1. 发布同 Core ABI 的 Core/Index 动态库、公共 wrapper、fengqudb_rag 和 wordgame_rag；不得部署内部临时测试库。
2. 用 examples/rag/wordgame_adapter.py 在明确新根目录创建 store 单例，替换 app/services/rag.py 的旧存储实现；保留旧签名、模型和 AppError。
3. 保留路由、工具、outbox、世界锁、观察/清除时间复核、规则、图标及 NPC 领域状态。状态/预热继续共享当前 Provider。
4. 在隔离游戏副本验证 G01–G12：outbox 失败重放、世界/NPC 清理、私有投影、小时/秒单位、旧事实、工具权限、场景预算。再由主会话安排正式切换。
5. 旧库保留/删除按用户的业务安排处理；本分支没有删除。新库不自动读取旧数据，也不承诺恢复被舍弃的历史。

## 证据边界

1000 条 896 维样本只展示小负载成本。未做旧 zvec 同条件对比、10k/50k/百万级性能、系统中文质量保留集或 ANN 召回评测；生产负载须先确认预算。默认 exact 不构成大规模延迟承诺。

Windows/Linux 的实际结果分别报告。macOS、Android/iOS、C#/Unity RAG 绑定未验收。正式 WordGame 切换前称“可接入并验证的替代底座”，不称线上替换完成。
