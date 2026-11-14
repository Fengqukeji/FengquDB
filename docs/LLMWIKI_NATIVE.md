# Wiki 与公共原生索引的实际接入

本轮在 Wiki 工作树提供标准库 Python wire/ABI 包装与 `NativeIndexProvider`，检索算法只调用索引工作树的共享库。权威定义是 `D:/study/FengquDB-index/include/fengqudb_index.h` 和 `proto/index.proto`（ABI / wire v1）。没有把旧 Python BM25 翻写成另一份 C++ 实现，也没有加入模型依赖。

## 文件与归属

| 本工作树文件 | 职责 |
| --- | --- |
| `bindings/python/fengqudb_index.py` | C ABI 调用、公开 lease、同 Core 地址核对、状态、结果内存、取消控制 |
| `bindings/python/fengqudb_index_wire.py` | v1 Protobuf 标准库编解码、类型化元数据/过滤、FP32 little-endian 向量 |
| `llmwiki/native_index_provider.py` | Wiki 投影版本、可见性、发布代数、原文定位与版本化物化 |
| `tests/test_fengqudb_index_python.py` | 可独立复制的 wire/lease 错误边界与真实绑定联调 |
| `tests/test_llmwiki_native.py` | Wiki 域投影与完整真实 NativeProvider 链路 |
| `bindings/python/fengqudb.py` | 从 RAG/Index 拥有方同步的公开桥接文件，不是另一套私有句柄桥接 |

共享的 Core C++ 桥接、Index header/proto、`src/index/` 和根构建继续由另外两个工作树维护。本轮没有复制或修改这些实现文件。初版公开桥接 SHA-256 为 `b77560795d0d5fda01e73994133054f69bf6b768cdfd709121f8420dfffc242d`。本轮按拥有方要求同步加入 native_library_path 的版本，当前 SHA-256 为 `c9e057eaccb8706f6dd1ab2b695980e23b715f77493d0220e36be7b2d53ddf62`；该文件没有另行修改。组合 SDK 相对导入与默认配套库加载见 [0.2 底座](LLMWIKI_FOUNDATION.md)。

## 运行

必须使用已包含公开 Core bridge ABI 1 的 Core，以及与其链接的 Index ABI 1 共享库。仅把新 Python 文件放到旧 Core 旁不会产生新桥接能力；Wiki 默认原生，缺库或不匹配即明确失败，不包含生产 Python fallback。

```powershell
$env:FENGQUDB_LIBRARY = 'D:\study\FengquDB-index\build\index-win\Release\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY = 'D:\study\FengquDB-index\build\index-win\Release\fengqudb_index.dll'
python -m llmwiki --db build/wiki-native --namespace native-v1 --index-provider native ingest examples/llmwiki/storage.md --uri knowledge:storage
python -m llmwiki --db build/wiki-native --namespace native-v1 --index-provider native compile
python -m llmwiki --db build/wiki-native --namespace native-v1 --index-provider native ask "数据库事务"
```

也可以显式传 `--index-library`、`--index-space`、`--index-partition`。CLI 默认就是 native，不提供 python 选项。任何其他 namespace 的索引身份不能被直接认领；当前使用新 namespace，不提供旧数据迁移工具。

```python
from llmwiki import Wiki, NativeIndexProvider

index = NativeIndexProvider(index_library_path, index_space=b"fengqu-wiki-v1")
with Wiki("wiki-native", library=core_library_path, namespace="native-v1", index_provider=index) as wiki:
    source = wiki.ingest("knowledge:storage", "数据库支持事务。", title="存储")
    while wiki.run_once() is not None:
        pass
    print(wiki.ask("数据库事务"))
```

`index_space` 和可选 partition 由受信任宿主指定；默认 partition 为当前 Wiki namespace 前缀的 SHA-256，隔离记录与 BM25 统计。不要给不可互见的命名空间强制指定同一 partition。名称不是用户授权凭据。

## 公开 Python 包装

```python
from bindings.python.fengqudb_index import NativeIndex, Document, Mutation, Query, Predicate, QueryControl

client = NativeIndex(index_library_path)
with store.transaction() as tx:
    tx.put(b"domain-record", b"authoritative data")
    client.mutate(tx, index_space=b"space", partition=b"partition", operation=Mutation.UPSERT,
        document=Document(b"record", 1, 7, 1, texts={"body": "searchable content"}, metadata={"visible": True}))
    tx.commit(force_reliable=True)
with store.snapshot() as snapshot:
    result = client.execute(snapshot, index_space=b"space", partition=b"partition", operation=Query.SEARCH,
                            text="content", where=Predicate(1, "visible", (True,)))
```

低层 `apply(handle, protobuf_bytes)` / `query(handle, protobuf_bytes)` 也保留。包装只读取 `borrow_native()` 返回的 `lease.pointer`、`identity`、`library`，不读取 Store 的 `_handle` / `_native` 等实现字段。`fqidx_core_bridge_address()` 必须等于 lease.library 中 `fqdb_store_bridge_abi_version` 的函数地址；仅 token 数值相同不够。Store token 始终传给 C ABI，借用必须保持到结果拷贝和 `fqdb_buffer_free` 都完成。

stage 不提交、不重开 Store、不改变写入策略。C apply 失败由原生库取消整个外部事务；Python 在编码、桥接或参数预检失败时也调用公开 `transaction.cancel()`，防止调用方捕获错误后误提交已有域写入。查询错误不偷偷取消调用者事务。被取消的事务应显式关闭或沿异常路径退出；不能走正常退出的自动提交路径。

QueryControl 接收无参取消回调，例如 `QueryControl(stop_event.is_set)`。回调异常按取消处理，保存异常类型而不跨 C 边界抛异常。输出大小、字节字段、嵌套过滤、整数、有限浮点数和向量维度均有边界；无论结果解析或状态如何失败，已分配返回缓冲只由对应 lease.library 释放。

## 修订与投影

Wiki 对外的 revision 是完整 SHA-256 字符串；v1 native Document 的 domain_revision/source_revision 是 uint64。适配器在同一 namespace 的 Store 中为完整哈希分配稳定整数 token，并保留完整哈希于权威元信息、原文 locator 和 native metadata。没有截断哈希，也没有把 token 当作对外版本。多来源页面没有伪造一个单一 source_revision，其所有引用照常保留在域记录中。

`native-index/projection/<id>` 保存独立 projection_version、lifecycle_generation、输入摘要和 tombstone；`native-index/document/<id>` 保存可物化投影元信息；`native-index/page/<page>` 保存发布状态及 publication_generation。每次 native mutation 带 expected_projection_version，patch/erase 带下一版本和生命周期代次。

普通原文/页面替换使用 upsert，不先 erase 再复活，所以正常修改不误增 lifecycle_generation。真实删除保留最小墓碑，删后重建才推进生命周期。来源的 publication_generation 与 chunk 所属发布版本独立于内容哈希。ready→dirty 是无正文的 metadata patch；已经 dirty 但目标 build_generation 改变也推进投影。缓存再发布即使 revision 不变，也产生新的 publication_generation 和投影版本。

页面 ready 状态先在域事务中暂存，随后以相同事务的 upsert 发布新正文投影；事务外不会看到一半更新。Native 查询始终带 `visible=true` 条件。命中物化再次核对 native 的投影版本、生命周期、域 token、完整哈希、locator 与原文内容哈希，按同一 View 读取原文/页；不把当前正文替换到旧快照命中里。

当前 v1 结果包含完整 Document，适配器按 locator 读取权威正文，不以 native texts/payload 作为权威原件。没有为了搜索强制在域外保存第二份可写权威原文。

## 结果与能力边界

- `ranking_kind` 保留原生给出的算法类型；预算终止与它独立，不能根据是否中断改写。扩展 v1 支持 ANN 请求及实际路径的 exact 回退。
- `termination`、`index_coverage`、`snapshot_version`、`mutation_sequence`/`applied_sequence` 原样按同一视图映射。Store version 与索引序列不直接比较。
- `match_evidence` 映射为 `has_hits` / `proven_none` / `not_established`。`proven_none` 在执行和覆盖完整时足以确认空结果，不要求额外伪造 total_hits=0；计数未提供时保持 unknown。
- Wiki 为有界检索显式启用 allow_partial；search/ask 最终回包保留所有限制。通用包装默认 allow_partial=false，限额/取消按原生状态返回错误。
- 标准库包装支持 get、stats、list、版本/序列 guard、类型化过滤、lexical 和 exact cosine。Wiki 默认查询仍用无模型词法；它没有擅自加载 KaLM 或启用语义模式。
- dense Search 不能带 text/match_all_terms；其他模式不能带 vector/vector_space/min_similarity；min_similarity 必须为有限的 [-1,1] 值。
- 扩展 v1 支持 HNSW 请求、按 predicate 统计、短语/邻近、前缀、命中字节位置及 Explain。Wiki 校验位置对应同版本正文；领域引用用 Unicode 字符范围，命中位置用 UTF-8 字节范围。termination=7 为 term_limit。小集合/图覆盖等情况可以 exact 回退，不能由 mode=2 推断 ranking_kind。
- 新包装保留旧 v1 查询兼容；扩展功能通过维护符号检测，旧运行库明确拒绝扩展字段。`maintain()` 使用同一外部事务，错误取消整个事务。完整接口见 [0.2 底座](LLMWIKI_FOUNDATION.md)。

独立打包脚本必须同时提供 Core 和 Index 路径，会打入配套共享库和 Python 包装，不复制 tests/reference_wiki_index.py 或任何独立 Python BM25。组合 SDK 的 native 平台目录与最终发布由主工作树统一组装；旧发布 ZIP 仍对应首版提交，本轮不覆盖旧 ZIP。

## 验证入口

```powershell
$env:FENGQUDB_LIBRARY = 'D:\study\FengquDB-index\build\index-win\Release\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY = 'D:\study\FengquDB-index\build\index-win\Release\fengqudb_index.dll'
python -m unittest discover -s tests -p test_fengqudb_index_python.py -v
python -m unittest discover -s tests -p 'test_llmwiki*.py' -v
```

```sh
docker run --rm \
  --mount type=bind,source=D:/study/FengquDB-llmwiki,target=/src \
  --mount type=bind,source=D:/study/FengquDB-index,target=/index,readonly \
  --workdir /src -e FENGQUDB_LIBRARY=/index/build/index-linux/libfengqudb.so \
  -e FENGQUDB_INDEX_LIBRARY=/index/build/index-linux/libfengqudb_index.so \
  -e LD_LIBRARY_PATH=/index/build/index-linux fqdb-lang-dev:1 \
  python3 -m unittest discover -s tests -p test_fengqudb_index_python.py -v
```

Linux 的 Wiki 测试使用同一容器命令，将末尾 pattern 改为 `'test_llmwiki*.py'`。独立绑定测试不导入 llmwiki，可单独复制到索引工作树运行。

`NativeLibraryIntegrationTests` 与 `NativeWikiLibraryIntegrationTests` 只有在 Core/Index 路径均提供时才运行。结果不得把 skipped 当作通过。覆盖真实事务内查询、C stage 及 Python 编码失败后的整体取消、公开快照读取/取消、精确向量过滤、metadata patch、版本 guard、稳定分页、错误 Core 映像、Wiki 状态/发布/恢复与原文物化；其余 wire/transport 用例是明确的协议 fixture，不冒充真实库验证。

以下是扩展前的历史验证；当前结果见 [0.2 验证](LLMWIKI_FOUNDATION_VALIDATION.md)。

2026-11-14 扩展前结果：Windows 绑定 **14/14**（0.359 秒）、Wiki **34/34**（4.274 秒）；Linux `fqdb-lang-dev:1` 绑定 **14/14**（0.392 秒）、Wiki **34/34**（3.384 秒）。无跳过。独立绑定中有 4 项真实库用例，Wiki 中有 1 项完整原生流程，其余含域/协议 fixture 与原有 Wiki 验证。未重跑无关数据库全量回归或性能评测。
