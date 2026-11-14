# FengquIndex Python 接入

可直接复制的组合SDK和当前完整接口见 [Python接入](FOUNDATION_PYTHON_SETUP.md)、[Python API](FOUNDATION_PYTHON_API.md)。支持短语/邻近、前缀、位置、评分解释、可见语料统计，以及显式建图后的HNSW和有界维护。

准备配套的 Core 和 Index 原生库，以及 `bindings/python/` 中的三个文件：`fengqudb.py`、`fengqudb_index.py`、`fengqudb_index_wire.py`。把文件所在目录加入项目模块路径。原生库路径也可通过 `FENGQUDB_LIBRARY` 和 `FENGQUDB_INDEX_LIBRARY` 环境变量设置。

## 保存与搜索

```python
from fengqudb import Store
from fengqudb_index import (NativeIndex, Document, Mutation, Predicate, Query,
                           QueryControl, UInt, vector)

# 应用启动时保留这两个实例，退出时关闭 Store。
store = Store("your-save-directory/main", library="path/to/fengqudb.dll")
index = NativeIndex("path/to/fengqudb_index.dll")
scope = dict(index_space=b"knowledge", partition=b"public")

document = Document(
    b"file-1", projection_version=1, lifecycle_generation=1,
    texts={"title": ("备份说明", 2), "body": "事务完成后执行备份"},
    metadata={"kind": "manual", "priority": 3},
    source_locator=b"file-1",
)
with store.transaction() as tx:
    tx.put(b"original/file-1", "业务原文".encode())
    index.mutate(tx, **scope, operation=Mutation.UPSERT, document=document)
    tx.commit(force_reliable=True)

with store.snapshot() as view:
    result = index.execute(view, **scope, text="备份", limit=8,
                           where=Predicate(1, "kind", ("manual",)))
    for hit in result["hits"]:
        record = hit["document"]
        print(record["id"], hit["score"])
        original = view.get(b"original/" + record["source_locator"])
```

Linux 使用配套 `.so` 路径。文本和元数据名使用 `str`，ID、资料分区和原文使用 `bytes`。Python 普通整数保存为 int64，需要 uint64 时使用 `UInt(value)`。编解码内置，业务不用编写 JSON 或 Protobuf 字段编号。

普通事务提交遵循 Store 配置；关键内容使用 `force_reliable=True`。原文和索引必须在同一事务中更新。索引写入错误会取消整个事务，应让异常退出事务上下文；不要捕获错误后继续提交。

## 修改、删除和读取

完整修改：提高 `document.projection_version`，调用 `mutate(...,operation=Mutation.UPSERT,expected_projection_version=旧版本,document=document)`。

仅修改元数据：调用 `Mutation.PATCH`，传 `id`、`expected_projection_version`、`next_projection_version`、原 `lifecycle_generation`，以及只含待修改元数据的 `Document`。不会重新分词或生成向量。

删除：调用 `Mutation.ERASE`，传上述身份和版本字段。删除后以同一个 ID 重建须提高生命周期代数。

`execute(...,operation=Query.GET,id=...)` 读取单条，`Query.STATS` 读取统计，`Query.LIST` 分页。下一页传结果中的 `next_cursor` 到 `after_id`，并传 `expected_mutation_sequence`；保持同一快照可获得稳定遍历。

`Predicate` 操作号：1 相等、2 不等、3 小于、4 小于等于、5 大于、6 大于等于、7 IN、8 存在、9 AND、10 OR、11 NOT。组合条件通过 `children=(...)` 传入子条件；类型须与元数据一致。

向量保存使用 `vector_f32_le=vector([...])` 和 `vector_space=b"模型空间身份"`；查询使用 `mode=1` 和对应的这两个字段，省略 `text`。原生核心做过滤后的精确余弦搜索，模型由业务提供。

## 预算与并发

`execute` 可以设置 `posting_budget`、`candidate_budget`、`scan_budget`、`memory_budget`、`timeout_ms`，或传 `QueryControl(event.is_set)` 取消。默认中断抛异常；显式开启 `allow_partial` 后，业务必须检查 `termination` 和 `match_evidence`。空结果不一定证明没有匹配。

包装自动持有公开原生租约并释放结果缓冲，返回 Python 自有数据。事务串行使用；不可变快照可用于并发查询。原生库不负责宿主权限判断，资料分区应由可信业务层固定。
