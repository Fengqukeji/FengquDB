# 原生复杂查询

`StoreQuery` 在 C++ 核心完成候选选择、字段解析、过滤、排序、聚合和等值关联，只把指定结果列交给调用语言。C++ 包含 `fengqudb_select.hpp`；Python 使用 `fengqudb_select`；C# 使用 `StoreQuery`；Java 使用可选的 `io.fengqudb.Select`。原有按键读取、字段投影和显式索引查询接口继续保留，固定简单请求仍可使用这些短路径。

## 能力与用途

| 能力 | 直白例子 | 执行方式 |
| --- | --- | --- |
| 比较、字段间比较、嵌套 AND / OR / NOT / IN | 红队且等级 20～60，或者 VIP；排除封禁玩家 | 索引找候选，C++ 短路过滤剩余条件 |
| 多字段独立范围、存在性 | 等级 20～60 且金币 100～5000；找缺手机号的人 | 范围残余过滤；Exists / IsNull 检查存在性 |
| 独立多字段排序、Top N、分页 | 按金币降序、ID 升序取前 20 人 | 兼容索引顺序时提前停止；否则有界 Top N 堆 |
| COUNT / SUM / MIN / MAX / AVG | 付费人数、总收入、平均订单金额 | 原生增量聚合；覆盖时只读索引 |
| GROUP BY、HAVING、DISTINCT | 按职业统计人数，只保留人数超过 100 的职业 | 有界哈希分组/去重，支持聚合参数 DISTINCT |
| 包含、前后缀、LIKE、正则、编辑距离 | 名字包含“张”；Name-1%；与“张三”相差一个字 | 前缀可走索引，其他原生匹配并限制工作量 |
| INNER / LEFT / SEMI / ANTI 等值关联 | 玩家加订单；有订单/无订单的玩家 | 主键、右侧索引探测或一次有界右侧哈希构建 |
| 自动选择已有索引、Explain | 业务只给条件，查看索引与扫描情况 | 有界候选抽样、复合前缀匹配和实际执行统计 |

`BETWEEN` 表达为 `>= AND <=`，`NOT IN` / `IS NOT NULL` 组合 NOT。最多 7 次关联，关联键 1～8 个字段，额外 ON 条件支持比较和组合表达式；不支持任意非等值关联。

## 条件查询示例

玩家字段 1 为 uint64 ID，2 为 string 职业，3 为 uint64 等级，4 为 sint64 金币。需求：“法师、20～60 级、金币至少 1000，按金币最多取 20 人，只返回 ID 和金币”。

```cpp
#include <fengqudb_select.hpp>
using namespace fengqudb;
using F = QueryField;
using E = QueryExpr;
using V = QueryValue;

db.define_compound_index("class-level", "p/", {{2,4}, {3,1}}, 2);
StoreQuery q;
q.prefix = "p/";
q.where = E::all({
    E::compare(F::field(2,4), QueryOp::Eq, V::text("mage")),
    E::compare(F::field(3,1), QueryOp::Ge, V::uint(20)),
    E::compare(F::field(3,1), QueryOp::Le, V::uint(60)),
    E::compare(F::field(4,5), QueryOp::Ge, V::integer(1000))
});
q.columns = {{F::field(1,1)}, {F::field(4,5)}};
q.order = {{F::field(4,5), true}, {F::field(1,1), false}};
q.limit = 20;
q.memory_bytes = 16ull << 20;
auto plan = explain(db, q);
auto result = select(db, q);
```

先按职业、等级定位候选，再读取金币，维护约 21 项堆；不为排序常驻全部候选。金币不在此索引中，仍需读取候选业务二进制；只查询职业和等级时可由索引覆盖。

Python：

```python
from fengqudb_select import Query, Field, Column, Order, Op
uid, kind, level, gold = Field(1,1), Field(2,4), Field(3,1), Field(4,5)
q = Query(b"p/", where=(kind.test(Op.EQ, "mage") &
    level.test(Op.GE, 20) & level.test(Op.LE, 60) & gold.test(Op.GE, 1000)),
    columns=[Column(uid), Column(gold)],
    order=[Order(gold, True), Order(uid)], limit=20)
prepared = q.prepare()  # 冻结请求，反复执行不再由绑定重编码
r = prepared.execute(db)
```

C#：`db.Select(q)` / `tx.Select(q)`，`q.Prepare().Execute(db)`。Java：`q.execute(db)` / `q.execute(tx)`，`q.prepare().execute(db, false)`。完整可运行例子见 `tests/store_select_bindings.py`、`tests/csharp/SelectSmoke.cs`、`tests/java/SelectSmoke.java`。

## 聚合与关联

```cpp
StoreQuery report;
report.prefix = "p/";
report.group = {F::field(2,4)};
report.columns = {{F::field(2,4)},
    {F::field(1,1), QueryAggregate::CountAll},
    {F::field(4,5), QueryAggregate::Sum},
    {F::field(4,5), QueryAggregate::Avg}};
report.having = E::compare(F::column(1), QueryOp::Gt, V::uint(100));
report.order = {{F::column(2), true}};
auto summary = select(db, report);

StoreQuery joined;
joined.prefix = "p/";
QueryJoin orders;
orders.prefix = "o/";
orders.left = {F::field(1,1)};
orders.right = {F::field(2,1,1)}; // 第一个关联源，订单字段2为玩家ID
orders.kind = QueryJoinKind::Left;
joined.joins = {orders};
joined.columns = {{F::field(1,1)}, {F::field(3,1,1)}};
auto rows = select(db, joined);
```

为 `o/` 字段 2 建索引可使用 index_join；没有合适索引时扫描右侧一次建哈希表。一个玩家多张订单生成多行；LEFT 无订单列为 Null。SEMI/ANTI 仅输出左侧，右侧字段不能被后续源引用。分组、排序和投影可用于关联结果。

## 计划与性能边界

* 自动选择显式创建且 READY 的索引，不自动建索引。支持复合等值/IN 前缀及随后一个范围、字符串前缀、LIKE 字面前缀；更后的条件原生过滤。
* OR 最多展开 32 分支，IN 最多展开 128 索引区间；超出则退回安全前缀扫描和完整过滤。OR 去重避免重复结果；NOT 通常不能形成有界候选。
* 可选复合索引缺组件时不建条目，只有证明不会漏结果才选择。覆盖需包含全部已用字段及相同物理类型。
* `access` 返回 primary_key、primary_key_range、index_range、ordered_index、covering_index_scan、prefix_scan、empty 或关联策略，以及 covering/ordered/needs_scan。sampled_candidates 是有界样本，不是精确行数或全量成本估算。
* `allow_scan=false` 拒绝完整源扫描/哈希构建，仍允许宽索引范围，因此应同时限制 max_candidates。Explain 可解释被此开关禁止的计划，不执行查询。
* 每个 Store 最多缓存一个不可变编译程序和一个计划，计入 pin 额度，不保留记录/历史代际。计划校验目录 epoch，数据样本最多复用 255 个提交版本；样本可能过时，不保证始终选择最优计划。
* 只解析所用字段路径，跳过其余载荷；索引覆盖、顺序提前停止、有界堆、一次哈希构建分别减少读取/排序/关联工作。任意排序、无前缀包含/正则、无索引条件仍可能处理大量候选，不能保证所有查询都胜过其他数据库。
* 聚合复用上一条已匹配记录所属的分组状态，连续同组时跳过分组键编码和哈希查找；无 GROUP BY 的全局聚合直接复用唯一状态。分组改变时仍检查完整键并查表，不依赖输入始终有序。该缓存只存在于本次请求，拥有独立字段字节。

## 类型、空值与字符串

物理类型 1～10 同 [复合索引](STORE_QUERY.md)，11 为 bool（仅 0/1）。查询路径最多 8 层，可过滤嵌套 message；磁盘索引只支持顶层标量。空路径 type4 为键，type0 为完整二进制。source0 为主源，1～7 为关联源；`QueryField::column(n)` / `Field.column(n)` 在排序和 HAVING 中引用从 0 开始的结果列。

缺字段返回 Null，不补 proto3 默认值；应使用 optional 或显式编码零值。重复标量标签取最后一个，嵌套匹配子字段按出现顺序覆盖；不展开 repeated，不模拟生成器的所有 message 合并规则。Null 比较得到 Unknown，WHERE/HAVING 只保留 True；AND/OR/NOT 使用三值逻辑。空 IN 始终 False；IN 含 Null 且无匹配项为 Unknown。Exists/IsNull 检查存在性。

uint64/int64/double 比较避免转 double 丢失大整数精度；去重、分组、哈希关联采用一致的数值规范键。整数 SUM 精确且越界报 CONSTRAINT，实数 SUM 使用补偿求和，AVG 为 double。COUNT 忽略 Null，COUNT_ALL 不忽略；空集 COUNT 为 0，其余聚合为 Null。

bytes/string 按无符号字节排序；IContains/ILike/IRegex 只折叠 ASCII。LIKE `%` 含任意字节/换行，`_` 匹配一个合法 UTF-8 码点，反斜线转义下一字面字符。Contains 线性匹配；Fuzzy 是 UTF-8 码点整字段编辑距离，阈值 0～8、模式最多 1024 码点，不是全文检索。

Regex 使用有界 Thompson NFA，支持字面值、`.`、`^$`、字符类、`\d\w\s` 及反类、`\xNN`、分组/非捕获分组、分支、`*+?{m,n}`。点不含 LF，无 multiline 模式。受限重复最大 32，每模式最多 2048 编译状态、1024 字节；回溯引用、前后环视和其他不支持语法明确报错。Regex 的点和字符类按字节匹配；中文单字匹配可用 LIKE/Fuzzy。

排序逐列设置方向和 NullsFirst，同值按稳定身份破同值，方向跟随最后一项排序方向。DISTINCT 的排序字段必须是输出列或输出列引用。无显式排序按记录/关联身份排序，不承诺磁盘自然顺序。

## 资源、分页与事务

| 设置 | 默认 / 范围 |
| --- | --- |
| limit / offset | 100 / 0；1～4096 / 0～65536 |
| memory_bytes | 16 MiB；1～64 MiB |
| max_candidates | 1,000,000；包括关联探测和哈希构建 |
| max_work | 100,000,000；解析/匹配/比较/复制等工作单位，不是 CPU 指令数 |
| timeout_ms | 1000；1～60000 ms |
| AST / 已用字段 / 源 | 1024 节点、64 层 / 256 / 8 |
| 请求 / 返回 | 请求 256 KiB；返回受 64 MiB 事务上限及实际预算限制 |

工作内存包括保守编译预留、受跟踪分配的堆/哈希/字符串、编译状态、编码。底层页/值、事务依赖和返回缓冲另受 Store 原有 cache/pin/事务及总事务预算约束，不是进程 RSS 硬上限。无磁盘临时溢写，预算不足报 LIMIT、整个请求失败，不返回冒充完整结果的部分答案。超时在有界检查点生效，不能中断 OS I/O。

stats 包含 candidates、matched、result_total、work、peak_memory、payload_reads、join_probes、complete。Top N 提前结束时 complete=false，计数只代表已处理部分；count_total=true 要求完整枚举。result_total 是游标之后、offset/limit 之前的逻辑结果数，聚合时为通过 HAVING 的组数。

非空 cursor 表示还有下一页，填入 after。游标绑定逻辑条件、输出与排序，允许改变页大小和预算。顺序索引/主键可定位续读，任意排序仍需重评估候选。固定视图分页在同一事务完成；跨 Store 调用捕获新快照，并发更新可能影响分页。

查询可见事务私有写，保留点读/范围依赖；后续写事务提交须检查冲突、超时和错误。查询不改变性能模式 50 ms／1 MiB、force_reliable、完整二进制缓存和减法裁剪。

当前没有 SQL 文本解析器、子查询/CTE、窗口函数、任意计算表达式、通用非等值 JOIN、自动建索引、磁盘分组溢写；全文、向量和 RAG/Wiki 留待另行设计。
