# 大结果读取修复与底座同步

合法的批量查询可以超过单条数据大小限制。本次修复的是查询输出错误复用单值 16 MiB 限制，未减少记录数、未降低 limit。

- PTpma 原始 20 × 900,000 字符正文用例：19 条、20 条、summary 限定字段查询均成功；20 条完整输出 18,005,392 字节。
- 默认仍返回完整文档；工作内存与编码后结果大小分别设预算。结果预算默认 64 MiB，可增大，不是数据库的固定总结果上限。
- 另用 80 条合法记录验证可调预算：完整正文共 72,000,880 字节，64 MiB 预算明确返回 LIMIT；提高到 128 MiB 后 80 条全文全部一致。
- 可选 HEADER 投影：同一原始用例 20 条输出 2,972 字节，ID、评分、版本、元数据、来源和 payload 与完整结果一致；不返回文本和向量，Get 仍可完整读取。
- 返回时裁剪已存二进制的连续范围，不重新解码、编码全部正文，不回传内部词项缓存。
- 超过输出预算明确返回 LIMIT、空缓冲；allow_partial 不会把装不下的结果伪装成完整结果。
- after_id 仅用于 List 的 ID 分页；Search 不支持排名分页，传入该参数明确报错。

## 已验证范围

Windows 相关 CTest 5 组通过；C++ 和 Python 新增完整 20 条大记录回归，覆盖全文、概要、Get、输出预算及错误缓冲。Python 共 23 项通过。Linux 便携 GCC 11 原生 C++ 大结果测试、扩展索引 914 检查和 Python 23 项通过。Linux ASan/UBSan/泄漏检测通过新增大结果所在 C++ 测试组。

组合 Python SDK 在 Windows/Linux 的空目录接入检查均通过，覆盖 Core、Index、RAG、Wiki。新版 RAG 记忆 38 项、来源 26 项通过。Wiki 回归发现独立的任务续租问题，由 Wiki 会话修复领取计时、续租及心跳覆盖范围；Windows 53 项（含新增 4 项）通过。其他平台的 Wiki 领域验收由该会话单独记录。

上述验证不表示已完成 PTpma、PTFramework 或技能中心部署。各消费者须使用完整匹配 SDK，完成项目验收后再部署。

## 更新规则

三个分支使用一致的 Core 源码和公开桥接；公共原生索引归 Index 分支，RAG/Wiki 使用同一发布依赖。Python 共享包装由 Index 维护并同步到 Wiki 工作树，RAG 复用它。

`scripts/package-foundations.py` 在打包前核对三个工作树的 Core 源码、公共头文件、Core Python 包装及共享索引包装。不一致直接停止；不允许把旧 Core 混进新底座。输出 `manifest.json` 记录每个交付文件的 SHA-256 和整套 `release_id`，供宿主记录实际依赖版本。更新完整目录并重新构建宿主，不能只单独替换一份 DLL。

原始接口复现与 ASan 记录保留在 `docs/evidence/index-large-result-20261114-*`。发布依赖身份见各分支 `docs/FOUNDATION_DEPENDENCIES.json`；旧验收记录继续作为历史证据保留。
