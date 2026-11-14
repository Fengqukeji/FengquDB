# FengquWiki：基于 FengquDB 的原生 LLM Wiki 底座

状态：0.2.0 / schema 2，独立工作树 `D:/study/FengquDB-llmwiki`，分支 `codex/llmwiki`。本轮修改未提交、未推送。没有修改原开发工作树的 SDK 工作，没有旧数据迁移或模型训练。

默认检索只使用共享 NativeIndex，缺原生库明确报错，不自动回退。生产目录没有独立 Python BM25 引擎。Python 穷尽对照只位于 tests/reference_wiki_index.py，不进入 SDK。运行依赖是 Python 标准库与配套 FengquDB Core + Index，不需要 Redis、向量服务或图数据库。

完整 API、数据边界、配额和完成清单见 [底座契约](LLMWIKI_FOUNDATION.md)，实际测试见 [验证记录](LLMWIKI_FOUNDATION_VALIDATION.md)。本轮没有重新发布旧 ZIP；组合 SDK 由主工作树统一组装。

## 设计参考

| 来源 | 借鉴 | 当前实现 |
| --- | --- | --- |
| [Karpathy LLM Wiki](https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f) | 原始资料与编译知识分层 | 不可变原文、可重建页面、显式编译契约 |
| [DeepWiki-Open](https://github.com/AsyncFuncAI/deepwiki-open) | 相互关联的 Wiki 页面 | 来源/实体/主题页、链接和反向链接 |
| [LightRAG](https://github.com/HKUDS/LightRAG) | 增量维护和来源追踪 | 依赖失效、来源生命周期、引用及冲突历史 |

独立实现，没有引入这些项目的框架，也没有把它们的性能描述当作本地测试结果。

## 组合 SDK：复制一个文件夹后使用

组合包包含 FengquDB/llmwiki、FengquDB/bindings/python 和 FengquDB/native/<平台架构>：

```python
from FengquDB.llmwiki import Wiki

with Wiki("wiki-data") as wiki:
    source = wiki.ingest("knowledge:storage", "数据库支持事务与可靠写入。", title="存储")
    wiki.define_page("storage-guide", "存储说明", [source["id"]])
    wiki.run_jobs(limit=100, workers=2)
    print(wiki.ask("可靠写入"))
```

默认找到包内配套 Core 和 Index，Linux 预加载同目录 Core，维持公开桥接的 Core 映像一致性。显式 library 参数和环境变量覆盖仍优先。没有 sys.path 修改或 sys.modules 全局别名。

```sh
python -m FengquDB.llmwiki --db wiki-data search "数据库事务"
python -m FengquDB.llmwiki --db wiki-data serve
```

## 独立源码入口

```powershell
$env:FENGQUDB_LIBRARY='D:\study\FengquDB-index\build\index-win\Release\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY='D:\study\FengquDB-index\build\index-win\Release\fengqudb_index.dll'
python examples/llmwiki/foundation.py
python -m llmwiki --db build/wiki-v2 ingest examples/llmwiki/storage.md --uri knowledge:storage
python -m llmwiki --db build/wiki-v2 compile --workers 2 --limit 100
python -m llmwiki --db build/wiki-v2 ask "数据库事务"
python -m llmwiki --db build/wiki-v2 serve
```

必须使用配套的公开 Core bridge ABI 1 / Index ABI 1 运行库。默认 HTTP 为 127.0.0.1:8098，非 loopback listener 必须配置 WIKI_TOKEN。服务一次提供一个 namespace，共享 token 不是细粒度 ACL。

[Python 示例](../examples/llmwiki/foundation.py) 展示批量摄入、实体事实、冲突决定、人工修订与问答；[HTTP 模板](../examples/llmwiki/foundation.http) 展示 JSON API。URI 只作为稳定逻辑标识，服务不会抓取 URL。ingest-dir 使用受限本地 UTF-8 连接器，拒绝 symlink/junction。

## 模型与一致性

默认 ExtractiveProvider 离线整理原文，annotations 可直接提供实体/事实。兼容 chat/completions 的模型可通过 WIKI_MODEL_URL / WIKI_MODEL / WIKI_MODEL_KEY 接入；compiler 和 answerer 也可独立注入或使用 WIKI_COMPILER_* / WIKI_ANSWERER_* 配置。模型调用在事务外执行，不成为检索引擎依赖。

来源写入、原文、索引、依赖失效和任务使用同一可靠事务；不修改数据库原有 50 ms / 1 MiB 写入阈值。来源修改/删除/恢复都推进单调 version，批量可使用 request_key 与 expected_version。发布前核对来源版本和租约令牌；引用同时保留完整 SHA-256、来源版本与原文范围。

事实 quote 必须匹配原文；竞争值形成显式冲突。人工页面固定为 human 模式，来源变化后 review_required，显式 release 才恢复编译。问答期间证据变化时丢弃答案及 evidence，返回 retryable。引用/版本校验不代表已经证明模型所有语义判断正确。

## 边界

使用 fresh schema-2 namespace；不打开旧格式并静默迁移。没有网页前端、分布式写入、自动 URL/仓库抓取、细粒度 ACL 或多人实时编辑。原文和历史持续保留，compact 不等于删除历史。

Native Analyzer 当前支持已声明的 ASCII/fullwidth 归一化、标识符和 CJK 分词；它不是完整 Unicode NFKC/casefold。测试 reference 的 Unicode 行为不作为生产能力。实体 ID 的 NFKC 规范化用于领域去重，与检索分词是两套明确的职责。

运行边界与原生接口详见 [Native 接入](LLMWIKI_NATIVE.md) 和 [IndexProvider 契约](LLMWIKI_INDEX_PROVIDER.md)。
