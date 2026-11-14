# Python 接入

把 ZIP 内的 `FengquDB` 文件夹复制到项目的 Python 模块目录，直接导入。文件夹已包含 Windows/Linux x64 的 Core 和 Index 二进制，运行时自动选择。其他平台需提供对应原生库。

在应用的存储服务或单例中长期保留 Store；应用退出时关闭。同一物理库由一个进程打开。

```python
from FengquDB import Store

db = Store("saves/main")
db.put(b"student:1", "小明".encode(), force_reliable=True)
print(db.get(b"student:1").decode())
# 应用退出时：db.close()
```

目录的父目录应由应用创建。可选配置：换 `saves/main` 可以修改存档位置；同时创建 `saves/config` 可使用另一个库。默认性能模式在内存生效后确认，按 50 ms / 1 MiB 合并写盘；关键写入传 `force_reliable=True` 等待可靠落盘。整个库需要可靠模式时导入 `StoreWriteMode`，调用 `db.configure_write(mode=StoreWriteMode.RELIABLE)`。

## 搜索、RAG、Wiki

普通全文搜索不需要模型，见 API 的“索引”。RAG 使用应用现有的嵌入回调：

```python
from FengquDB import CallbackEmbedding, EmbeddingContract, MemoryStore, MemoryWrite, MemorySearch

# existing_embed(texts, query=False) 返回每条文本的向量。
embedding = CallbackEmbedding(existing_embed,
    EmbeddingContract("你的模型权重及处理规则身份", 896, pooling="mean"))
memory = MemoryStore("saves/rag", embedding=embedding)
memory.put("account-1", [MemoryWrite(text="李山住在东村。", scope="world-1",
    owner_id="npc-1", known_at=8, source_id="event-42")])
hits = memory.search("account-1", MemorySearch(query="李山住哪里？", scope="world-1",
    owner_id="npc-1", include_public=False, as_of=10))
# 应用退出时：memory.close()
```

模型身份应包含实际权重、分词、池化及查询指令，维度填写实际值。回调处理查询指令，数据库接收其最终向量。

Wiki 可以直接摄入资料、生成带引用的页面并搜索：

```python
from FengquDB import Wiki

wiki = Wiki("saves/wiki")
source = wiki.ingest("manual:storage", "数据库支持事务与可靠保存。", title="存储说明")
wiki.run_jobs()
print(wiki.search("可靠保存"))
print(wiki.ask("怎样保存？"))
# 应用退出时：wiki.close()
```

默认页面与答案由证据摘录组成。需要大模型写作时给 `Wiki` 注入 compiler/answerer 或 provider，业务自己管理模型。
