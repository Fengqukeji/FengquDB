# FengquWiki 0.2 最终验证记录

日期：2026-11-14。工作树 `D:/study/FengquDB-llmwiki`，分支 `codex/llmwiki`，HEAD 为 f632878；本轮功能保留为未提交修改，没有推送。没有迁移旧数据、训练模型或修改公共 Core/Index C++、header、proto、CMake。公开 Core Python 依赖按拥有方要求原样同步，SHA 为 c9e057eaccb8706f6dd1ab2b695980e23b715f77493d0220e36be7b2d53ddf62。

## 自行运行的最终测试

| 环境 | 独立 Index | Wiki | 合计 |
| --- | --- | --- | --- |
| Windows 11 x64 / Python 3.12.10 | 22/22 | 49/49 | 71/71，6.472秒 |
| Docker / WSL2 Linux x64 / Python 3.14.4 | 22/22 | 49/49 | 71/71，4.513秒 |

两套最终运行均无失败、无错误、无跳过。当前权威报告是 [Windows final JSON](evidence/llmwiki-foundation-windows-final.json) 与 [Linux final JSON](evidence/llmwiki-foundation-linux-final.json)，各有同名 log；此前分阶段报告保留用于追踪，不替代 final。

独立 Index 的22项包括14项 wire/transport fixture 与8项实际 Core+Index 测试。真实用例覆盖外部事务、失败后整事务取消、旧快照、错误 Core 映像、公共 binding alias、过滤/向量、字段检索/NOT、位置/Explain、短语/前缀、predicate统计、显式影子图准备及回收入口、词项内部整理游标。fixture 没有冒充检索算法验证。

Wiki 的49项包括27项旧领域/对照场景、1项本机 HTTP 模型协议 fixture、12项默认 NativeIndex 的领域/恢复/API/连接器场景、6项真实 Store 加 fake Index 的契约/缺库场景、2项完整真实原生流程、1项组合目录默认构造/CLI。旧对照明确注入 tests/reference_wiki_index.py，其中进程恢复场景使用原生默认；生产目录不含该实现。原生维护算法自身由拥有方测试，这里验证 Python ABI 与 Wiki 接口行为。

最终库 SHA-256：

| 平台 | Core | Index |
| --- | --- | --- |
| Windows | e56a3ccf0c959c894c12c33e0f2df590404dd0139b52636da77eec6cb73c7df5 | 872794c4181f4afa394a906adbf5c3b22ff84fceb13cfeb461c71b061883c917 |
| Linux portable | b52b29d92b4ac5c29324d876b5a5e859a2e331df0375f863089bd2e0b71d0327 | b1928ddbeb612aa160b4309a70c4697fc40c76923aa338fb9de02a78cc9a38e4 |

Windows 使用拥有方 build/index-win/Release 下配套 DLL；Linux 使用 build/index-portable/libfengqudb.so.2 和 libfengqudb_index.so.1。Linux 测试环境实际为 glibc 2.43；拥有方提供的二进制兼容基线不等于我们另行在 glibc 2.31 主机上测试过。

```powershell
$env:FENGQUDB_LIBRARY='D:\study\FengquDB-index\build\index-win\Release\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY='D:\study\FengquDB-index\build\index-win\Release\fengqudb_index.dll'
python tools/verify_llmwiki_foundation.py --suite all --output build/wiki-verification.json
```

Linux 使用 fqdb-lang-dev:1，挂载 Wiki 为 /src、只读 Index 为 /index，指定上述 portable 配套库和 LD_LIBRARY_PATH=/index/build/index-portable，运行相同工具。没有重跑无关数据库全量回归，也没有将其他工作树报告的 CTest/ASan/914checks 计入本表。

## 小样本性能

64份合成双语来源，单份约1092字符；16项可靠原子批次，annotations 建立20个实体；编译64来源页+20实体页，148个索引文档。60次交替稀有英文词和宽泛中文查询。单进程、热缓存、真实最终 NativeIndex、离线 ExtractiveProvider；不包含模型网络或推理时间。

| 指标 | Windows | Linux portable / Docker |
| --- | --- | --- |
| 摄入来源/秒 | 984.69 | 1041.31 |
| 编译页面/秒 | 291.68 | 264.98 |
| 查询P50 / P95（毫秒） | 1.409 / 2.074 | 0.991 / 1.567 |
| 截断查询 | 0/60 | 0/60 |

原始记录：[Windows benchmark](evidence/llmwiki-foundation-windows-benchmark.json)、[Linux benchmark](evidence/llmwiki-foundation-linux-benchmark.json)。这是本地小样本，不是数据库横向排名、生产容量保证、LLM质量或向量召回率评测；不据此比较两个操作系统的优劣。

## 自审修正与打包路径

- 来源修改/删除/恢复均推进单调版本，阻止相同内容哈希的删后恢复混淆旧结果。
- 历史冲突保留完整 claim；撤销当前来源不抹掉旧竞争证据和决定。
- 人工页固定模式，源变更后 review；过期编辑须明确确认当前来源版本。
- 人工来源页仍可抽取知识，已审核编辑不会被在途抽取覆盖。
- Answer期间证据变化后丢弃回答和evidence；缓存命中也重新核验。
- annotations对象键顺序不影响内容/幂等身份。
- 编译指纹选择同一份不可变产物；两个worker不同输出不会让迟到结果污染缓存、重发和历史。
- Windows短路径按规范化路径计算相对URI；连接器真实验证Windows junction及Linux symlink拒绝路径。
- 向量变更使用带CAS的完整UPSERT，metadata PATCH不携带向量。
- 默认仅NativeIndex，缺库失败且不创建Store；reference仅留tests，不进入SDK或ZIP。
- ANN用例先对1030个32维向量循环op2到完成，再确认graph_visited；未准备时的exact回退不冒充ANN。
- 160条热门词记录按17条预算续做，验证词项内部边界、旧快照、整理后排名/分数。
- 组合目录在两个平台的隔离子进程清除PYTHONPATH、库路径和Linux loader环境变量，只复制一个FengquDB文件夹即完成默认import、真实摄入/问答和模块CLI。没有sys.modules别名。

独立临时包验证覆盖摄入、默认编译/问答、backup、verify、compact；同时检查生产ZIP没有reference、retrieval.py或旧pyc。组合SDK最终发布由主工作树负责，本轮临时包不覆盖旧发布ZIP。

## 交付边界

没有调用真实外部模型或验证事实抽取/问答语义准确率；只验证协议、引用和版本一致性。没有验证macOS、arm64/x86、手机设备、生产容量或分布式部署。Native Analyzer不是完整Unicode NFKC/casefold，测试reference的Unicode能力不作为生产能力。服务无细粒度用户ACL，actor仅审计标签。使用fresh schema-2 namespace。

共享wrapper复制清单和SHA见 [handoff JSON](evidence/llmwiki-index-wrapper-handoff.json)：两份Index binding及其独立测试。Core依赖来自拥有方，无私有改动；Wiki领域文件留在本分支。最终全部文件清单见 [source manifest](evidence/llmwiki-foundation-source-manifest.json)。
