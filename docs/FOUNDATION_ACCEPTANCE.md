# 索引、RAG、Wiki 本轮交付

2026-11-14。三套底座共用原生 Core/Index，按各自工作树保存；本轮未提交、未推送，也没有修改 develop 中已有的 SDK 工作。

| 交付 | 目录或证据 |
| --- | --- |
| C++ SDK | `D:/study/FengquDB-index/artifacts/foundation/cpp/FengquDB` |
| Python 组合 SDK | `D:/study/FengquDB-index/artifacts/foundation/python/FengquDB` |
| ZIP 与文件摘要 | `artifacts/foundation/FengquDB-CPP.zip`、`FengquDB-Python.zip`、`manifest.json` |
| 原生实现与性能 | [INDEX_IMPLEMENTATION.md](INDEX_IMPLEMENTATION.md) |
| RAG 自审和冻结库验收 | `D:/study/FengquDB-rag/docs/rag/ACCEPTANCE.md` |
| Wiki 自审和冻结库验收 | `D:/study/FengquDB-llmwiki/docs/LLMWIKI_FOUNDATION_VALIDATION.md` |

最终库 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| Windows Core | `e56a3ccf0c959c894c12c33e0f2df590404dd0139b52636da77eec6cb73c7df5` |
| Windows Index | `872794c4181f4afa394a906adbf5c3b22ff84fceb13cfeb461c71b061883c917` |
| Linux portable Core | `b52b29d92b4ac5c29324d876b5a5e859a2e331df0375f863089bd2e0b71d0327` |
| Linux portable Index | `b1928ddbeb612aa160b4309a70c4697fc40c76923aa338fb9de02a78cc9a38e4` |

Windows 两个运行库合计约1.63MiB；Linux约1.62MiB。每个ZIP同时包含两平台，应用只需部署对应平台的原生库。模型另由宿主提供。

Windows/Linux Release原生三组各通过，GCC11便携扩展914项通过；RAG负责人完成两平台各66项，Wiki与共享包装负责人完成两平台各71项，均无跳过。组合Python SDK与独立C++ consumer也在两平台通过。

Linux ASan+UBSan（含泄漏检测、出错即停）最终三组全部通过，耗时193.35秒；17,425条基础断言与914项扩展检查无报告。第一次未优化Debug扩展触及240秒时限，改用带符号O1构建后通过；没有删减用例或把超时标成通过。原始日志与本轮摘要保存在 docs/evidence/foundation-20261114-*。本轮未执行TSan、Android/iOS或ARM64设备测试。

本轮修复：C++前置编码异常取消事务；元数据patch保持减法二进制；分块整理的截断尾部和恢复游标；过滤统计避免泄漏不可见语料；重建期间并发修改、旧代次回收和快照隔离；跨包Python导入和同Core身份。RAG/Wiki各自还审查修复了清理屏障、版本发布、任务租约与编译缓存覆盖等领域问题，详见各自验收文档。

用户取消旧资料迁移。下游只重建对应旧索引/RAG存储；业务存档、模型和源码保留。PTpma接入负责项目命令语义、单持库宿主与本机IPC，WordGame接入复用既有Embedding和游戏事件/outbox语义。底座验收不冒充实际业务替换验收。

完成底座验收后，已向用户指定的PTpma会话 `01a1d74e-7a9f-7673-a5a1-c0bcb51e490b`、WordGame会话 `01a158e5-ef69-7d60-967a-444027094e55` 发送实际接入任务、SDK目录和验证边界；两会话状态已确认active。应用接入仍在执行，尚未报告业务验收通过。
