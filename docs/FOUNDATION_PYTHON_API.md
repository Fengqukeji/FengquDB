# Python API

## 保存、读取、修改、删除

`Store.put(key,value)` 同时用于新增和覆盖，key/value 为 bytes。`get(key)` 返回 bytes，`delete(key)` 删除；`get_many(keys)` 一次读多条。Store 由应用长期持有。

```python
from FengquDB import Store
with Store("saves/main") as db:
    db.put(b"score", (99).to_bytes(8, "little", signed=True))
    score = int.from_bytes(db.get(b"score"), "little", signed=True)
    with db.transaction() as tx:
        tx.put(b"name", "小明".encode())
        tx.put(b"score", (100).to_bytes(8, "little", signed=True))
        tx.commit(force_reliable=True)
    db.delete(b"name", force_reliable=True)
```

`transaction()` 原子修改多个键，`snapshot()` 固定读取视图。事务串行使用；查询可并发使用不可变快照。`backup(destination)` 备份，`sync()` 等待此前写入可靠落盘。性能模式确认后的数据在意外终止前若尚未同步，可能丢失；可靠提交错误或未知结果应按状态处理。

## 索引

```python
from FengquDB import NativeIndex, Document, Mutation, Query, Predicate
index = NativeIndex()
with db.transaction() as tx:
    index.mutate(tx, index_space=b"knowledge", partition=b"public", operation=Mutation.UPSERT,
        document=Document(id=b"manual-1", projection_version=1, lifecycle_generation=1,
                          texts={"body": "数据库可靠保存"},
                          metadata={"kind": "manual"}))
    tx.commit(force_reliable=True)
with db.snapshot() as view:
    result = index.execute(view, index_space=b"knowledge", partition=b"public",
        text="可靠", where=Predicate(1, field="kind", values=("manual",)))
```

| 接口/选项 | 用途 |
| --- | --- |
| mutate UPSERT / PATCH / ERASE | 新增或整存 / 只改元数据 / 删除；修改提供旧、新投影版本，删除保留生命周期墓碑 |
| execute SEARCH / GET / LIST / STATS | 搜索 / 按 ID / ID 游标分页 / 统计；事务内可读本次修改 |
| Predicate | 比较、IN、存在、AND/OR/NOT；不同数值类型不混同 |
| phrases、prefix_last、excluded_terms、scoring_fields | 短语及邻近 / 末词前缀 / 排除词 / 限定评分字段 |
| include_positions、snippet_bytes、include_explain | UTF-8 原文位置、摘录及 BM25 评分明细 |
| statistics_policy=1 | 仅按 where 可见文档计算 BM25 统计，适用于历史与权限过滤 |
| mode=1 / mode=2 | 精确余弦 / HNSW 近似搜索；需 vector_f32_le 和 vector_space，向量用 vector([...]) 编码 |
| maintain COMPACT_POSTINGS / REPAIR_GRAPH / VERIFY_RECORDS / COLLECT_GRAPH_GENERATIONS | 有界整理倒排 / 建图重建 / 校验 / 回收旧图 |

`maintain(tx,index_space=...,partition=...,operation=Maintenance.REPAIR_GRAPH)` 显式建图；每批事务提交，重复至 `finished=True`。未建图或小集合自动走精确搜索。整理也可反复调用，持久检查点自动续做。更新不重新拼装未变全文；patch 复用词项和向量。

查询可设候选、扫描、倒排、词项、内存、超时预算和 `QueryControl` 取消。默认超限报错；启用 `allow_partial` 后检查 `termination/index_coverage/match_evidence`。近似 Top K 与预算中断是不同状态，空数组本身不能证明没有匹配。

## RAG

| 方法 | 常见用途 |
| --- | --- |
| put(account,items,record_id=None) | 原子保存记忆；record_id + expected_revision 纠错；fact_key 管理事实有效期 |
| get / list / history | 单条、条件列表、谱系历史分页 |
| search / retrieve / context | 记录列表 / 附完成性与工作量 / 附引用的上下文 |
| import_text | 长文自动分块与内容去重 |
| delete / delete_matching / retire_namespace | 删除整个谱系 / 按精确范围清理 / 禁止该范围旧写入复活 |
| scoped | 固定 account、scope、owner、timeline，交给受限业务组件 |
| recover / backup / stats / close | 恢复中断清理、备份、统计、关闭 |
| embedding_status / warmup | 查看或预热宿主模型 |

`MemorySearch.mode` 为 dense、lexical 或 hybrid；scope/owner/timeline/as_of/source_id 控制资料范围与有效时间，include_public 控制公共并集。原生过滤先约束候选，回包前再次校验生命周期。`QueryBudget` 控制成本，reranker 只接收已授权候选。MemoryStore 的治理写入使用可靠提交。

## Wiki

| 方法 | 常见用途 |
| --- | --- |
| ingest / ingest_many / source | 摄入资料、幂等批写、读取来源历史 |
| search / ask | 检索资料和已发布页；组织证据、引用及可替换答案回调 |
| define_page / page | 定义主题页、读取当前或历史版本 |
| run_once / run_jobs / retry | 持久任务领取、有限并发编译、失败重试 |
| revise_page / release_page | CAS 人工编辑与解除人工保护 |
| delete_source | 删除来源并使依赖页失效 |
| entity / conflict / resolve_conflict | 实体与声明、冲突组、带版本的人工裁决 |
| set_vectors / maintain_index | 为现有条目附向量、有界索引维护 |
| status / lint / export / backup / verify / compact | 状态、自检、Markdown 导出、备份、校验与整理 |

来源变更会使相关页失效，编译回包校验来源版本和任务租约，旧 worker 不能覆盖新发布。默认使用同一个原生索引内核；页面、任务、引用和索引投影在同一库内更新。
