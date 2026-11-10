# Store 复合索引、范围查询与字段投影

默认性能模式支持同步接口：内存原子生效后确认，后台按50 ms／1 MiB刷盘。关键写入传force_reliable／forceReliable，或配置Reliable模式；提交凭据完成遵循所选保证。见[写入策略](STORE_WRITE_POLICY.md)。

本页描述显式索引和字段投影。组合条件、自动选择索引、独立排序、聚合及关联见 [原生复杂查询](STORE_SELECT.md)。不提供 SQL 文本解析器；只使用 KV 的业务不建立或维护这些索引。目录、准备对象与私有补丁减法裁剪继续沿用原路径。

本批实现审查、故障验证、性能和体积证据见 [验证记录](STORE_QUERY_REVIEW_20261107.md)。

例如玩家有 team、score 和较大的 profile。索引 `(team, score)` 可以直接找到“红队、分数 100～1000 的玩家”，按分数分页，无须扫描其他队伍或读取每个玩家的 profile。

```cpp
// 字段 1 bytes/string team，字段 2 sint64 score，字段 3 bytes profile。
db.define_compound_index("team-score", "player/", {{1,4}, {2,5}}, 0);
fengqudb::wire::Writer equal, lower, upper;
equal.bytes_field(1, "red");
lower.sint_field(2, 100);
upper.sint_field(2, 1000);
fengqudb::StoreIndexQuery query;
query.equal = equal.data();
query.lower = lower.data();
query.upper = upper.data();
query.upper_inclusive = true;
query.fields = {1, 2};
auto page = db.query_index("team-score", query, {}, 100);
if (!page.cursor.empty()) {
    auto next = db.query_index("team-score", query, page.cursor, 100);
}
auto only_score = db.project("player/42", {2});
```

## 定义和类型

复合索引有 1～8 个有序、不重复的顶层字段，顺序决定字典序。名称最多 128 字节、键前缀最多 4096 字节，最多 64 个定义。flags 为 unique=1、required=2，可组合；唯一约束针对整个元组。可选索引的任意组件缺失就不为该记录建立条目。空 bytes 和显式存在的数值零是有效组件。

| 类型号 | Protobuf 类型 | 排序 |
| --- | --- | --- |
| 1 | uint64 / uint32 | 无符号数值 |
| 2 | int64，varint 补码 | 有符号数值 |
| 3 | double | 有限数值，正负零相同 |
| 4 | bytes / string | 无符号字节序，无语言排序规则 |
| 5 | sint64 / sint32，ZigZag | 有符号数值 |
| 6 | float | 有限数值，正负零相同 |
| 7 | fixed64 | 无符号数值 |
| 8 | sfixed64 | 有符号数值 |
| 9 | fixed32 | 无符号数值 |
| 10 | sfixed32 | 有符号数值 |
| 11 | bool | 只接受 0/1，false 在前 |

原单字段 define_index 也支持这 11 种类型。没有业务 schema 时，引擎不能推断被省略的 proto3 字段是默认值还是不存在；按存在性约束或查询零值时，应在业务 `.proto` 使用 optional，或显式 wire 编码标签。单值重复标签采用最后一个值。磁盘索引不支持嵌套路径、repeated 展开、全文或向量索引；原生复杂查询可在残余过滤中使用嵌套路径。

复合字段零字节转义和边界编码后合计最多 64 KiB，主键仍最多 64 KiB；全零 bytes 可能更早达到编码上限。构建、删除、续建、后台维护、约束和 TTL 继承原索引机制；失败后可能留下 BUILDING 定义，同参数 define 可续建。

## 范围和一致性

equal 是业务消息编码，只指定连续的前几个索引字段；可为空表示全索引。lower/upper 各自只包含“下一个索引字段”的编码。未指定为无界，默认 `[lower, upper)`，可用 lower_exclusive、upper_inclusive 改变边界。不能跳过前导字段，也不能在全部字段等值限定后再加范围。下界大于上界返回 INVALID_ARGUMENT，相同端点的空区间返回空页。

非索引业务字段会被跳过；范围边界消息不能同时携带其他索引组件。无范围且 equal 包含所有组件就是完整元组等值查询。旧 INDEX_LOOKUP 也支持完整元组，唯一等值查询保留点读路径。

默认升序，reverse=true 为整个顺序的逆序；相同元组按业务主键二进制顺序排列。每页 1～4096 条，默认 256；结果受事务预算和 64 MiB 上限限制。满页可带非空游标，即使已经是最后一页；允许随后再取空页。

游标是不透明字节，须配合同一索引、过滤条件和方向使用。普通 Store 调用各自捕获视图；稳定整段分页应在同一事务内完成。事务私有新增、修改、删除参与查询；每次查询从当前私有写集推导临时索引覆盖，不将临时条目写入正式写集。查询登记索引范围及必要点读依赖，随后业务提交检测相关幻读。

## 投影和性能

索引查询默认只返回主键，row.value 为空；include_values=true 返回完整值。fields 非空时返回投影，优先于 include_values。单条 project 的空字段列表返回空 Protobuf，记录不存在则为 NOT_FOUND。

投影最多选择 256 个顶层字段号，列表去重；原标签、编码、出现顺序、repeated/packed 字节和选中字段内的未知内容保持原样。它验证完整外层 wire，损坏返回 CONSTRAINT。选择嵌套消息字段会保留整个嵌套消息，不能选择内部子字段。

Store 级 `project/Project` 使用直接只读入口 `fqdb_store_project`，省去事务、快照槽和外层命令封装。事务内 `project` 仍保留私有写集与读依赖。通用投影合并相邻选中片段，常见记录只扫描一次字段头；超过 64 个不相邻片段时使用有界内存的两遍扫描。重复读取可用各语言的准备投影读取器，见 [共享读取](STORE_SHARED_READ.md)。

只取主键时不读取业务载荷。取值或投影时，目前仍读取对应业务值并校验 CRC；投影减少输出复制和跨语言传输，不是列式磁盘读取。结果按实际行数增长并计预算。只有存在相关私有写入才生成查询覆盖；不会常驻第二份查询索引。

带值或投影的范围查询可在本次请求内复用最多三个不可变索引页，总额最多64 KiB，并计入pin预算；预算余量不足或关闭缓存时保留原查找路径。原记录的点读依赖和索引范围依赖仍保留，非索引字段并发修改也能触发事务冲突。列表连续扫描、游标预算复用的实现和实测见[列表与查询优化](STORE_LIST_QUERY_OPTIMIZATION_20261110.md)。

## 各语言接入

| 语言 | 定义 | 查询 | 投影 |
| --- | --- | --- | --- |
| C | `fqdb_store_index_define_compound` | `fqdb_store_txn_execute`，op=51 | `fqdb_store_project`；事务内 op=52 |
| C++ | `define_compound_index` | `StoreIndexQuery` + `query_index` | `project` |
| C# | `DefineCompoundIndex` + `StoreIndexField[]` | `StoreIndexQuery` + `QueryIndex` | `Project` |
| Java | `defineCompoundIndex` + `Store.IndexField[]` | `Store.IndexQuery` + `queryIndex` | `project` |
| Python | `define_compound_index(..., [(field,type),...])` | `query_index(..., equal=..., lower=..., fields=[...])` | `project` |

查询和投影提供 Store 与事务接口；Java 分页显式传 cursor、limit，其余封装有默认值。业务消息用自己的 `.proto` 序列化，封装负责外层请求。C envelope 见公共头文件：op=51 的 value 为 `proto/store_query.proto`，op=52 的 scope 为空、key 是主键、value 仅带 fields。fields 的 packed/unpacked 编码均接受。

新复合定义采用独立内部键空间，保留旧单字段编码；旧 Store 无须转换。旧开发版不保证读取新定义，须配套新库与绑定。真实生产验收和最终多数据库对比仍在持续目标中。
