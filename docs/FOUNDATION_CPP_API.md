# C++ API

## 新增、修改、读取、删除

```cpp
#include "fengqudb_index.hpp"
namespace idx = fengqudb::index;

fengqudb::Store db("saves/main"); // 保留在应用的存储服务中
db.put("student/1", "小明");    // 同一 key 再 put 就是覆盖
auto value = db.get("student/1");
db.erase("student/1");
```

`transaction()` 原子处理多个键；`snapshot()` 提供固定视图；`get_many` 一次读取多条，`backup` 备份，`sync` 等待已确认写入可靠落盘。业务实体可以用项目自己的序列化器变成二进制再存入。

## 读取版本（轻量）

| 接口 | 返回内容 |
| --- | --- |
| `db.version()` | 当前数据库已发布的版本；可用于判断数据是否变化 |
| `snapshot.version()` | 创建快照时捕获的固定版本 |
| `transaction.version()` | 当前活动事务捕获的基础版本；提交后的版本看 `CommitResult.version` |

```cpp
const auto current = db.version();
auto view = db.snapshot();
const auto captured = view.version();
auto tx = db.transaction();
const auto base = tx.version();
```

这些方法读取已发布视图，不调用 `info()`、不统计文件、不取得写入锁，也不触发刷盘。性能模式下返回值可包含尚未可靠落盘的提交；它不代表持久化进度。事务仍需处于有效、未取消且未超时状态。底层复用现有 bridge ABI 1 的三个 identity 接口，`fqdb_store_get_version(store,key,...)` 则是单个键的版本。

## 同事务全文索引

```cpp
idx::Client index(db, "knowledge", "public");
idx::Document d;
d.id = "manual-1";
d.texts = {{"title", "备份说明", 2}, {"body", "事务完成后可靠保存", 1}};
d.metadata = {{"kind", idx::Value::text("manual")}};
auto tx = db.transaction();
tx.put("original/manual-1", "业务原文");
index.upsert(tx, d);
auto committed = tx.commit(true);
if (!committed) throw fengqudb::Error(static_cast<fqdb_status>(committed.status));

idx::Query q;
q.text = "保存";
q.where = idx::Filter::eq("kind", idx::Value::text("manual"));
auto view = db.snapshot();
auto result = index.query(view, q);
for (const auto& hit : result.hits) { /* hit.id、score、payload、source_locator */ }
```

| 接口/设置 | 用途 |
| --- | --- |
| upsert(tx,d,expected_version) | 新增或整存；修改提高 projection_version |
| patch(tx,Patch) | 仅增删元数据、域修订和来源修订；保留全文词项与向量 |
| erase(tx,id,old,next,generation) | 删除并保留防复活墓碑；重新建同 ID 须提高生命周期 |
| query(snapshot/tx,q) | 搜索；Get/Stats/List 提供按 ID、统计及游标遍历 |
| Filter::eq/compare/in/exists/all/any/negate | 类型化比较、范围、IN、嵌套条件 |
| phrases/prefix_last/excluded_terms/scoring_fields | 短语邻近、前缀、排除词、字段检索 |
| include_positions/snippet_bytes/include_explain | 原文位置、摘录及评分明细 |
| statistics_policy=1 | 仅使用过滤可见语料计算统计 |
| dense=true / approximate=true | 精确余弦 / HNSW，配置 vector 和 vector_space |
| maintain(tx,MaintenanceOptions) | Compact/RebuildGraph/Verify/CollectGraphs，有界批次与持久续做 |

未显式建图默认保持精确搜索，避免所有向量写入承担建图开销。建图时多批调用 RebuildGraph 并提交至 finished；影子代次完成后原子发布，旧快照仍可查询。

`Result` 拥有原生缓冲，Hit 的字符串视图随该 Result 生存。事务串行使用；不可变快照支持并发查询。修改的编码、原生调用或回包解码失败会取消整个外部事务，不能继续提交部分业务变更。

Query 可设置倒排、候选、扫描、词项、内存、超时预算与取消回调。默认中断报错；显式 allow_partial 后检查 complete、termination、match_evidence。近似排名不等于预算中断，空结果也不总能证明没有匹配。

查询默认返回完整文档，整批结果可以超过单条数据的 16 MiB 限制。`q.output_budget` 默认 64 MiB，可按需要增大；它限制编码后整批结果的大小，与工作内存预算独立。实际内存还包括输出缓冲及回包复制。结果装不下时返回 `FQDB_LIMIT`，不会截断后冒充成功，`allow_partial` 也不会绕过这个限制。

只需要 ID、评分、版本、元数据、来源和 payload 时，设置 `q.result_projection=FQIDX_PROJECT_HEADER`。这只省去回包中的文本和向量，不修改存储内容、检索范围或排名；之后用 Get 可取完整文档。`after_id` 只用于 List 的 ID 分页，Search 暂无排名分页，传入该参数会报错。
