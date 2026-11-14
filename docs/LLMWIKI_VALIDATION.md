# FengquWiki 0.1 验证记录

日期：2026-11-14。分支：`codex/llmwiki`，数据库基线：`32348fb`。

## 实际交付与范围

- 独立 worktree：`D:/study/FengquDB-llmwiki`。原 `D:/study/FengquDB` 的 `develop` 与未提交 SDK 工作保留。
- 数据库核心和已有 Python 绑定未改动；Wiki 调用现有公共 Store API。
- 新增 Python 库、CLI、HTTP 服务、模型适配层、部署/验证脚本、容器配置和独立 CI。
- 无额外 Python 运行时依赖，无额外存储服务。未执行真实付费模型调用。

## 构建与集成测试

| 平台 | 实际环境 | 原生构建 | 相关测试 |
| --- | --- | --- | --- |
| Windows x64 | MSVC 19.51、Python 3.12.10 | 本分支 Release，关闭 IPO、工具与诊断组件 | 20/20 通过，3.507 秒 |
| Linux x64 | Docker `fqdb-query-dev:3`、GCC 13.3、Python 3.12.3 | 本分支 Release，关闭 IPO、工具与诊断组件 | 20/20 通过，1.982 秒 |

使用 `python -m unittest discover -s tests -p test_llmwiki.py -v`，原生库由 `FENGQUDB_LIBRARY` 指定。

覆盖：

1. 同 URI 去重、中英文与混合文字检索、游标分页、namespace 分区。
2. 不可变来源版本、编译页历史、来源更新后的旧页失效、按依赖选择性重编译。
3. 跨来源主题页、引用、反向链接；删除部分来源后的重建及全部来源删除后的退役。
4. 在已暂存索引写入后注入异常，确认来源指针与旧索引原子回滚。
5. 租约重启恢复、两个 worker 的令牌拦截、模型运行期间来源变化后的旧结果丢弃。
6. 编译缓存、未知模型引用拒绝、3 次尝试预算与显式恢复。
7. 带证据回答、证据变化时 stale 标识、空证据与无效问答引用。
8. 非回环地址要求 token、HTTP 鉴权、参数校验、摄入/查询/删除接口。
9. 本地 HTTP 模型协议模拟，确认请求格式、凭据传递及错误信息脱敏。
10. 子进程在可靠摄入成功后 `os._exit(0)`，重开后原文索引和待编译任务仍可用。
11. 完整备份可重开、Markdown 导出、原生 verify/compact 后数据可查。
12. 启用原生后台维护后，写入模式、周期和字节阈值保持原值。

没有改动数据库核心，因此未重跑与本次集成无关的全语言 SDK、Unity、移动端、消毒器和全库性能测试。新增 CI 配置已提交，未声称远程 CI 已运行。

## 定向性能

同一台 AMD Ryzen 9 9950X3D（16 核 / 32 线程）主机，Windows 本机与 Docker Linux。每个平台独立运行，单进程、热缓存、可靠事务、离线抽取式编译器，嵌入 API 的自动后台整理未启用。语料为 1,000 份合成双语文本，每份约 1,080 个 Unicode 字符，编译后共 2,000 个检索文档。100 次查询各有一半稀有词、一半宽泛中文组合。

| 指标 | Windows | Linux |
| --- | ---: | ---: |
| 可靠摄入，份/秒 | 90.58 | 102.71 |
| 抽取式编译，页/秒 | 72.30 | 64.53 |
| 内容不变的重复摄入，份/秒 | 11,426.78 | 6,497.44 |
| 稀有词查询 P50 / P95，ms | 0.579 / 0.874 | 0.418 / 0.630 |
| 宽泛中文查询 P50 / P95，ms | 64.658 / 82.631 | 50.922 / 54.730 |
| 全部查询 P95，ms | 79.271 | 54.071 |
| 标记截断的查询 | 50/100 | 50/100 |
| 单查询最多访问倒排记录 | 12,000 | 12,000 |
| 未整理数据库文件字节数 | 166,778,661 | 170,213,636 |

原始结果：[Windows](evidence/llmwiki/windows-1000-20261114.json)、[Linux](evidence/llmwiki/linux-1000-20261114.json)。复现：

```sh
python tools/llmwiki_benchmark.py --sources 1000 --queries 100 --output build/wiki-benchmark.json
```

稀有词查询只读取少量倒排项，宽泛中文查询会触发候选或倒排预算；这些结果属于**有界近似召回**，不能宣称完成精确全库 Top K。文件大小包含追加写入、版本、编译缓存与索引开销，是整理前的结果，不代表长期保留策略下的最终占用。服务默认启用原生后台整理，手动 `compact` 可在空闲时回收物理冗余；历史原文与页面不会因此删除。

这些是特定合成负载下的单轮数据，不是跨数据库排名，也不证明真实模型吞吐量、生产 P95、百万级语料规模或语义召回质量。

## 部署包

最终 ZIP 各含一个平台的原生库、公共 Python 绑定、Wiki 模块、说明和启动器。通过 `scripts/verify-llmwiki-package.py` 在临时目录解压运行，清除源码路径和外部原生库配置，验证随包运行库的摄入、编译、问答、备份、校验与整理。交付清单与 SHA-256 保存在 `artifacts/FengquWiki-0.1.0-release.json`。

容器 Dockerfile 和 Compose 配置已提供；Linux 测试在现有开发容器中完成，未执行新 Dockerfile 的完整联网镜像构建。未验证 macOS、Android/iOS，也未把带引用格式校验当作模型事实正确性的证明。

## Index Provider 协作补充

在首版之后抽出了独立检索 Provider，保留原 Python BM25 实现。变更包括摄入/删除、可见性镜像、检索、统计与问答后的版本复核路由，以及格式身份与旧索引认领保护。契约见 [LLMWIKI_INDEX_PROVIDER.md](LLMWIKI_INDEX_PROVIDER.md)。

本轮仍使用 `32348fb` 核心与已有原生库，不改数据库、公共绑定或共享构建。相关测试在 Windows x64 **24/24** 通过（3.917 秒），Docker Linux x64 **24/24** 通过（2.730 秒）。新增 4 项覆盖 Provider 全链路路由、禁止静默切换格式、旧 Python 索引认领和拒绝非原子异步实现。

未实现原生 Provider、结构化过滤、异步索引水位和稳定句柄借用桥接；文档将其列为公共索引接入要求。没有另写 C++ 倒排/HNSW，也没有启动模型下载、蒸馏或训练。此前最终 ZIP 和 release manifest 仍对应 `92efa28` 首版，不能把它们当成本轮源码的部署包。

### 公共契约核对与结果状态

根据索引工作树的 `docs/FOUNDATION_COORDINATION.md`，本轮明确记录 domain_revision 与独立 projection_version、生命周期及发布代数、原件归属和 index_space/统计隔离要求。页面 stale 镜像不被当成完整的版本化投影实现；版本化 CAS/patch/erase 仍待原生接入。

运行时新增排名、终止、覆盖、命中总量关系及空结果确认的独立字段，search/ask 均传播；仅有空数组的旧 Provider 回包采用 unknown 覆盖，近似且正常结束不自动算截断。Python 穷尽评分使用未舍入分数排序，预算受限时明确标记 approximate 和具体限制。

Windows x64 **28/28** 测试通过（4.027 秒），Docker Linux x64 **28/28** 通过（2.800 秒）。新增验证确认词法未命中、受限空结果不能确认未命中、近似完成与截断分离、旧 Provider 覆盖未知；既有问答与预算测试同时验证最终字段传播。本轮未重新测量性能或更新此前发布包，未改其他工作树、公共绑定或共享构建，未启动训练。

## 公共原生索引真实联调

后续收到 Index ABI 1 / wire v1 及配套桥接 Core 后，已实现标准库包装、NativeIndexProvider 和显式 native CLI 选择。公共桥接 Python 文件从拥有方同步，当前 SHA-256 与 Index/RAG 一致；没有在 Wiki 分支重写 Core 桥接或检索算法。

| 平台 | 独立绑定测试 | Wiki 与域适配测试 | 跳过 |
| --- | --- | --- | --- |
| Windows x64，实际 `build/index-win/Release` DLL | 14/14，0.359 秒 | 34/34，4.274 秒 | 0 |
| Linux x64，实际 `build/index-linux` SO，`fqdb-lang-dev:1` | 14/14，0.392 秒 | 34/34，3.384 秒 | 0 |

这 48 项包含 fixture 和实际库测试，不能把全部说成 48 个原生用例。独立绑定包含 4 项真实库测试；Wiki 包含 1 项完整原生流程，其余覆盖域投影、旧 Python 行为、wire/lease 错误边界。实际验证了 stage 整体取消、同快照、错误 Core 映像/模块别名、dense 过滤、metadata patch、版本及分页 guard、页面 dirty/发布代数、删后生命周期与原文 hash 物化。

首轮联调发现的错误来自测试对被取消事务走了正常退出自动提交，已改为显式关闭；没有为此改变引擎持久性或事务语义。最终两平台无失败、无跳过。未扩大到新的模型、数据库全量回归、ASan 或性能评测；这些不由本会话声称完成。

可直接整合的三文件清单及命令见 [LLMWIKI_NATIVE_HANDOFF.md](LLMWIKI_NATIVE_HANDOFF.md)。旧 ZIP/manifest 仍是首版，本轮由主会话集中整合原生库及安装包。
