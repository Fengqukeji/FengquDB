# FengquIndex C++ 接入

FengquIndex 是可选原生库：给已有 FengquDB 增加中文/英文全文检索、元数据条件和精确/近似向量检索。普通文本搜索不需要模型。二进制独立SDK见 [接入](FOUNDATION_CPP_SETUP.md) 与 [API](FOUNDATION_CPP_API.md)。

构建时设置 `FQDB_BUILD_INDEX=ON`。安装后的项目使用 `find_package(FengquDB CONFIG REQUIRED)`，链接 `FengquDB::Index`，包含 `fengqudb_index.hpp`。Windows 运行时需要配套的 `fengqudb.dll` 和 `fengqudb_index.dll`；Linux 使用对应的两个 `.so`。完整可编译例子在 `examples/index/`。

## 初始化与保存

Store 放在宿主的存储服务或单例中，按需创建事务/快照。`Client` 固定索引空间和资料分区；不同分区的结果和 BM25 统计互相隔离。

```cpp
#include "fengqudb_index.hpp"
namespace idx = fengqudb::index;

fengqudb::Store store("your-save-directory/main");
idx::Client index(store, "knowledge", "public");

idx::Document d;
d.id = "file-1";
d.texts = {{"title", "备份说明", 2}, {"body", "事务完成后执行备份", 1}};
d.metadata = {{"kind", idx::Value::text("manual")},
              {"priority", idx::Value::integer(3)}};
d.source_locator = "file-1";

auto tx = store.transaction();
tx.put("original/file-1", "业务原文或序列化数据");
index.upsert(tx, d);              // 新记录，预期版本为 0
auto status = tx.commit(true);    // 原文与索引一起可靠提交
if (!status) throw fengqudb::Error(static_cast<fqdb_status>(status.status));
```

普通提交沿用 Store 的配置：默认性能模式在内存生效后确认；`commit(true)` 等可靠同步。Index 不独立提交，也不改变周期或阈值。任何原生索引写入错误会取消整个事务，随后应关闭事务并根据错误重试。

## 搜索与读取

```cpp
idx::Query q;
q.text = "备份";
q.limit = 8;
q.where = idx::Filter::all({
    idx::Filter::eq("kind", idx::Value::text("manual")),
    idx::Filter::compare("priority", idx::Filter::GreaterEqual, idx::Value::integer(2))
});
auto view = store.snapshot();
auto result = index.query(view, q);
for (const auto& hit : result.hits) {
    // hit.id、score、projection_version、source_locator、payload
    // 根据 source_locator，在同一 view 中读取业务原文。
}
```

`Filter` 支持比较、`in`、`exists`、`all`、`any` 和 `negate`。数值类型严格区分，整数不会经过 double 转换；条件值应与保存时的类型一致。

`Query::Get` 配合 `id` 读取单条；`Query::Stats` 返回记录数；`Query::List` 按 ID 分页。下一页传 `after_id=result.next_cursor` 和 `expected_mutation_sequence=result.mutation_sequence`，并保持原快照；改用新快照后发生修改会报告冲突。事务内调用 `index.query(tx,q)` 可以读取本事务的修改。

## 修改与删除

```cpp
auto tx = store.transaction();
idx::Patch p;
p.id = "file-1";
p.expected_version = 1;
p.next_version = 2;
p.metadata = {{"priority", idx::Value::integer(4)}};
index.patch(tx, p); // 不重新分词，不改向量
auto status = tx.commit(true);
if (!status) throw fengqudb::Error(static_cast<fqdb_status>(status.status));
```

完整修改使用 `upsert(tx,d,旧版本)`，同时增大 `d.projection_version`。删除调用 `erase(tx,id,旧版本,新版本,lifecycle_generation)`。版本不匹配会冲突；删除留下最小墓碑，重新使用该 ID 必须提高生命周期代数。业务修订 `domain_revision` 与投影版本独立。

## 可选向量

保存时设置 `Document.vector` 和 `vector_space`。查询设置 `dense=true`、`vector` 和同一 `vector_space`，省略 `text`，按分区过滤后做精确余弦搜索。显式维护 RebuildGraph 后可设置 approximate=true 使用HNSW；未准备或小集合自动精确搜索。写入时自动归一化；同一向量空间的维度固定。模型身份应由宿主完整登记，同维度不表示不同模型可以混用。生成向量的模型由业务提供。

## 预算和结果生命周期

查询默认最多访问 12,000 个倒排项、1,024 个匹配候选、100,000 条记录，工作内存估算预算 16 MiB。可通过 `Query` 调整，并设置超时或传入 `fqidx_query_control` 取消回调。内存预算针对索引自有工作量估算，Store 缓存和宿主内存另计。

默认超预算、超时或取消会报错；只有设置 `allow_partial=true` 才返回部分结果。此时检查 `complete()`、`termination` 和 `match_evidence`，不能把未完成的空结果当成“没有匹配项”。

结果独立拥有原生缓冲，`Hit` 中字符串是该缓冲的视图；使用时保留对应 `Result`。完整文档编码可通过 `document_wire` 交给业务编解码器。事务由调用方串行使用；独立查询可并发使用同一不可变快照，期间不得销毁该快照。
