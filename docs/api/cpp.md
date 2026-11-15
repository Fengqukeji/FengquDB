# C++ API 与调用例子

[接入文档](../languages/cpp.md) · [English](../en/api/cpp.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取：按键找数据

```cpp
if (auto value = db.get("student/7")) {
    auto row = Codec::decode(value->view());
    auto grade = row.value.score;
}
```

没有记录时返回空 optional。本文 Codec 指接入文档生成的 StudentFqCodec。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `std::optional<StoreValue> get(std::string_view key) const` | 读取对应的数据或状态 |

</details>

## 2. 新增或保存

```cpp
Student student{7, "Alice", 95};
db.put(Codec::key(student), Codec::encode(student));
db.put("name", "Alice"); // 单个字符串可以直接存 UTF-8
```

同键 put 会覆盖旧值。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, bool force_reliable = false)` | 保存或设置数据 |

</details>

## 3. 修改与原子操作

```cpp
if (auto bytes = db.get("student/7")) {
    auto row = Codec::decode(bytes->view());
    row.value.score = 96;
    db.put(Codec::key(row.value), Codec::encode(row));
}
db.add_i64("login-count", 1);
```

add_i64 使用八字节小端整数；不能对实体编码直接加减。compare_exchange 按版本修改，put_if_absent 仅在不存在时新增，patch 适合原始字节区间修改。

<details>
<summary>函数与参数（8项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo add_i64(std::string_view key, int64_t delta, int64_t minimum, int64_t maximum, uint64_t timeout_ms = 0, bool force_reliable = false)` | 条件更新或局部修改 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo add_i64_fields(const fqdb_store_i64_field_update *items, uint32_t count, uint64_t timeout_ms = 0, bool force_reliable = false)` | 条件更新或局部修改 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo add_i64_fields(const std::vector<StoreI64FieldUpdate> &items, uint64_t timeout_ms = 0, bool force_reliable = false)` | 条件更新或局部修改 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo compare_exchange(std::string_view key, uint64_t expected, std::string_view value, uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0, bool force_reliable = false)` | 条件更新或局部修改 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo delete_if_version(std::string_view key, uint64_t expected, uint64_t timeout_ms = 0, bool force_reliable = false)` | 删除记录或成员 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo patch(std::string_view key, uint64_t offset, uint64_t remove, std::string_view insert, bool force_reliable = false)` | 条件更新或局部修改 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo put_if_absent(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0, bool force_reliable = false)` | 保存或设置数据 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo put_if_changed(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0, bool force_reliable = false)` | 保存或设置数据 |

</details>

## 4. 删除

```cpp
db.erase("student/7");
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo erase(std::string_view key, bool force_reliable = false)` | 删除记录或成员 |

</details>

## 5. 批量操作

```cpp
db.put_many({{"a", "one"}, {"b", "two"}});
auto values = db.get_many({"a", "b"});
```

put_many 是一笔原子批写。

<details>
<summary>函数与参数（6项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `uint64_t delete_many(const std::vector<std::string_view> &keys, bool force_reliable = false)` | 删除记录或成员 |
| `fengqudb::Store` | `fengqudb::StoreValues get_many(const std::vector<std::string_view> &keys) const` | 读取对应的数据或状态 |
| `fengqudb::Store` | `fengqudb::StoreBatch prepare_batch(const std::vector<StorePut> &items) const` | 批量处理 |
| `fengqudb::Store` | `uint64_t put_batch(const StoreBatch &batch, bool force_reliable = false)` | 保存或设置数据 |
| `fengqudb::Store` | `uint64_t put_many(const std::vector<StorePut> &items, bool force_reliable = false)` | 保存或设置数据 |
| `fengqudb::StoreBatch` | `StoreBatch(StoreBatch &&) noexcept` | 批量处理 |

</details>

## 6. 事务：一组修改一起提交

```cpp
auto tx = db.transaction();
tx.put("bag/item", "sword");
tx.erase("reward/pending");
auto result = tx.commit();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));
```

未提交的事务析构时丢弃。CONFLICT 时重建事务并重新读取计算。

<details>
<summary>函数与参数（22项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreTransaction transaction(uint64_t timeout_ms = 0)` | 事务操作 |
| `fengqudb::StoreTransaction` | `StoreTransaction(StoreTransaction &&) noexcept` | 事务操作 |
| `fengqudb::StoreTransaction` | `void cancel()` | 事务操作 |
| `fengqudb::StoreTransaction` | `fengqudb::CommitResult commit(bool force_reliable = false)` | 提交事务或查看提交结果 |
| `fengqudb::StoreTransaction` | `void delete_many(const std::vector<std::string_view> &keys)` | 删除记录或成员 |
| `fengqudb::StoreTransaction` | `void erase(std::string_view key)` | 删除记录或成员 |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCommandResult execute(const StoreCommand &command)` | 事务操作 |
| `fengqudb::StoreTransaction` | `void expire(std::string_view key, uint64_t expires_at_ms)` | 事务操作 |
| `fengqudb::StoreTransaction` | `std::optional<std::string> get(std::string_view key)` | 读取对应的数据或状态 |
| `fengqudb::StoreTransaction` | `fengqudb::StoreValues get_many(const std::vector<std::string_view> &keys)` | 读取对应的数据或状态 |
| `fengqudb::StoreTransaction` | `fengqudb::StoreTransaction & operator=(StoreTransaction &&) noexcept` | 事务操作 |
| `fengqudb::StoreTransaction` | `void patch(std::string_view key, uint64_t offset, uint64_t remove, std::string_view insert)` | 事务操作 |
| `fengqudb::StoreTransaction` | `std::string project(std::string_view key, const std::vector<uint32_t> &fields)` | 事务操作 |
| `fengqudb::StoreTransaction` | `void put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0)` | 保存或设置数据 |
| `fengqudb::StoreTransaction` | `void put_batch(const StoreBatch &batch)` | 保存或设置数据 |
| `fengqudb::StoreTransaction` | `void put_many(const std::vector<StorePut> &items)` | 保存或设置数据 |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCommandResult query_index(std::string_view name, const StoreIndexQuery &query = {}, std::string_view cursor = {}, uint64_t limit = 256)` | 事务操作 |
| `fengqudb::StoreTransaction` | `void release(uint64_t id)` | 释放对象或借用的资源 |
| `fengqudb::StoreTransaction` | `void rollback_to(uint64_t id)` | 回退到保存点 |
| `fengqudb::StoreTransaction` | `uint64_t savepoint()` | 创建或操作事务保存点 |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCursor scan(std::string_view begin = {}, std::string_view end = {}, bool reverse = false)` | 事务操作 |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCommit submit(bool force_reliable = false)` | 事务操作 |

</details>

## 7. 筛选、排序和统计

```cpp
#include <fengqudb_query.hpp>
namespace q = fengqudb::q;
auto score = q::s64(Codec::Field_score);
auto name = q::text(Codec::Field_name);
auto query = q::from("student/").where(score >= 90)
    .select(name, score).order_by(score.desc()).limit(10);
auto rows = q::run(db, query);
for (auto row : rows) { auto grade = row[score]; }
```

支持组合条件、聚合、分组及等值关联；prepare 复用同一查询，explain 查看查询计划。

<details>
<summary>函数与参数（190项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `template <class Database> fengqudb::StoreQueryResult explain(Database &db, const StoreQuery &q)` | 查看查询计划 |
| `fengqudb` | `template <class Database> fengqudb::StoreQueryResult select(Database &db, const StoreQuery &q)` | 查询构建或结果读取 |
| `fengqudb::Buffer` | `fengqudb::Buffer & operator=(Buffer &&other) noexcept` | 查询构建或结果读取 |
| `fengqudb::Buffer` | `fengqudb::Buffer & operator=(const Buffer &)` | 查询构建或结果读取 |
| `fengqudb::CommitResult` | `operator bool() const noexcept` | 查询构建或结果读取 |
| `fengqudb::QueryColumn` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr all(std::vector<QueryExpr> c)` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr any(std::vector<QueryExpr> c)` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr compare(QueryField f, QueryOp op, QueryValue value)` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr compare_fields(QueryField f, QueryOp op, QueryField right)` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `std::string encode(unsigned depth = 0) const` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr negate(QueryExpr c)` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr one_of(QueryField f, std::vector<QueryValue> values)` | 查询构建或结果读取 |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr test(QueryField f, QueryOp op)` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `fengqudb::QueryField any() const` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `fengqudb::QueryField column(uint32_t n)` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `fengqudb::QueryField field(uint32_t number, uint32_t type, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `fengqudb::QueryField key(uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `bool operator!=(const QueryField &o) const` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `bool operator==(const QueryField &o) const` | 查询构建或结果读取 |
| `fengqudb::QueryField` | `fengqudb::QueryField record(uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::QueryJoin` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::QueryOrder` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::QueryValue` | `fengqudb::QueryValue boolean(bool v)` | 查询构建或结果读取 |
| `fengqudb::QueryValue` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::QueryValue` | `fengqudb::QueryValue integer(int64_t v)` | 查询构建或结果读取 |
| `fengqudb::QueryValue` | `fengqudb::QueryValue real(double v)` | 查询构建或结果读取 |
| `fengqudb::QueryValue` | `fengqudb::QueryValue text(std::string v)` | 查询构建或结果读取 |
| `fengqudb::QueryValue` | `fengqudb::QueryValue uint(uint64_t v)` | 查询构建或结果读取 |
| `fengqudb::ReadResult` | `operator bool() const noexcept` | 查询构建或结果读取 |
| `fengqudb::Store` | `fengqudb::Store & operator=(Store &&) noexcept` | 查询构建或结果读取 |
| `fengqudb::Store` | `fengqudb::StoreCommandResult select_command(const StoreCommand &command) const` | 查询构建或结果读取 |
| `fengqudb::StoreBatch` | `fengqudb::StoreBatch & operator=(StoreBatch &&) noexcept` | 查询构建或结果读取 |
| `fengqudb::StoreCommit` | `fengqudb::StoreCommit & operator=(StoreCommit &&) noexcept` | 查询构建或结果读取 |
| `fengqudb::StoreIndexQuery` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::StorePreparedQuery` | `StorePreparedQuery(const StoreQuery &q)` | 查询构建或结果读取 |
| `fengqudb::StorePreparedQuery` | `template <class Database> fengqudb::StoreQueryResult execute(Database &db, bool explain_only = false) const` | 查询构建或结果读取 |
| `fengqudb::StoreQuery` | `std::string encode() const` | 查询构建或结果读取 |
| `fengqudb::StoreQueryResult` | `StoreQueryResult(std::string_view bytes)` | 查询构建或结果读取 |
| `fengqudb::StoreReadView` | `fengqudb::StoreResult operator[](uint32_t index) const` | 查询构建或结果读取 |
| `fengqudb::StoreValue` | `fengqudb::StoreValue & operator=(StoreValue &&) noexcept` | 查询构建或结果读取 |
| `fengqudb::StoreValues` | `fengqudb::StoreValues & operator=(StoreValues &&) noexcept` | 查询构建或结果读取 |
| `fengqudb::StoreValues` | `fengqudb::StoreResult operator[](uint32_t index) const` | 查询构建或结果读取 |
| `fengqudb::q` | `Col(QueryField f)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col(uint32_t number, uint32_t type, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Col<bool> any() const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Col<double> any() const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Col<long> any() const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Col<std::basic_string<char>> any() const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Col<unsigned long> any() const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Order asc(bool nulls_first = true) const` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Agg<double> avg(const Col<T> &c)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond between(const T &low, const T &high) const` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<bool> boolean(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond contains(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `Agg<uint64_t> count()` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Agg<uint64_t> count(const Col<T> &c)` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Agg<uint64_t> count_distinct(const Col<T> &c)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Order desc(bool nulls_first = true) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond ends_with(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond exists() const` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class Database> fengqudb::StoreQueryResult explain(Database &db, const Query &q)` | 查看查询计划 |
| `fengqudb::q` | `Col<double> f32(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<double> f64(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<uint64_t> fixed32(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<uint64_t> fixed64(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Query from(std::string prefix)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond fuzzy(const std::string &s, uint64_t distance) const` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<int64_t> i64(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond icontains(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond ilike(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond in(const std::vector<T> &values) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond in(std::initializer_list<T> values) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond is_null() const` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<std::string> key(uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond like(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Agg<T> max(const Col<T> &c)` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Agg<T> min(const Col<T> &c)` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Col<T> nested(std::vector<uint32_t> path, uint32_t type, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator!(Cond a)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator!=(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator!=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator&&(Cond a, Cond b)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator<(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator<(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator<=(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator<=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator==(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator==(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator>(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator>(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator>=(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator>=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond operator\|\|(Cond a, Cond b)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Prepared prepare() const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond regex(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class Database> fengqudb::q::Rows run(Database &db, const Query &q)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<int64_t> s64(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<int64_t> sfixed32(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<int64_t> sfixed64(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond starts_with(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q` | `template <class T> Agg<T> sum(const Col<T> &c)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond test(QueryOp op, const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond test(QueryOp op, const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<std::string> text(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q` | `fengqudb::q::Cond text_test(QueryOp op, const std::string &pattern) const` | 查询构建或结果读取 |
| `fengqudb::q` | `Col<uint64_t> u64(uint32_t n, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Order asc(bool nulls_first = true) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Order desc(bool nulls_first = true) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator!=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator<(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator<=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator==(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator>(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator>=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `Col<T>(QueryField f)` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `Col<T>(uint32_t number, uint32_t type, uint32_t source = 0)` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `Col<T> any() const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Order asc(bool nulls_first = true) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond between(const T &low, const T &high) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond contains(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Order desc(bool nulls_first = true) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond ends_with(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond exists() const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond fuzzy(const std::string &s, uint64_t distance) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond icontains(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond ilike(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond in(const std::vector<T> &values) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond in(std::initializer_list<T> values) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond is_null() const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond like(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator!=(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator!=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<=(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator==(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator==(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>=(const Col &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>=(const T &v) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond regex(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Col` | `fengqudb::q::Cond starts_with(const std::string &s) const` | 查询构建或结果读取 |
| `fengqudb::q::Prepared` | `Prepared(const Query &q)` | 查询构建或结果读取 |
| `fengqudb::q::Prepared` | `template <class Database> fengqudb::q::Rows run(Database &db) const` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `Query(std::string prefix)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & after(std::string cursor)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & allow_scan(bool on)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::StoreQuery build() const` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `const std::vector<QueryColumn> & columns() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & count_total(bool on = true)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & distinct(bool on = true)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `template <class... T> fengqudb::q::Query & group_by(const Col<T> &...cols)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & having(Cond c)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & index(std::string name)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & join(QueryJoin j)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `template <class T> fengqudb::q::Query & join(std::string prefix, const Col<T> &left, const Col<T> &right, QueryJoinKind kind = QueryJoinKind::Inner)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & limit(uint64_t n)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & max_candidates(uint64_t n)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & memory(uint64_t bytes)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & offset(uint64_t n)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `template <class... Items> fengqudb::q::Query & order_by(const Items &...items)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Prepared prepare() const` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `template <class... Items> fengqudb::q::Query & select(const Items &...items)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & timeout_ms(uint64_t ms)` | 查询构建或结果读取 |
| `fengqudb::q::Query` | `fengqudb::q::Query & where(Cond c)` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `Rows(std::vector<QueryColumn> columns, StoreQueryResult result)` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `const std::vector<QueryAccess> & access() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `fengqudb::q::Rows::iterator begin() const` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `const std::string & cursor() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `bool empty() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `fengqudb::q::Rows::iterator end() const` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `fengqudb::q::Rows::Row operator[](size_t i) const` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `const fengqudb::StoreQueryResult & raw() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `size_t size() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows` | `const fengqudb::QueryStats & stats() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows::Row` | `Row(const Rows *owner, const QueryRow *row)` | 查询构建或结果读取 |
| `fengqudb::q::Rows::Row` | `template <class T> std::optional<T> get(const Agg<T> &a) const` | 读取对应的数据或状态 |
| `fengqudb::q::Rows::Row` | `template <class T> std::optional<T> get(const Col<T> &c) const` | 读取对应的数据或状态 |
| `fengqudb::q::Rows::Row` | `const std::string & key() const noexcept` | 查询构建或结果读取 |
| `fengqudb::q::Rows::Row` | `template <class T> T operator[](const Agg<T> &a) const` | 查询构建或结果读取 |
| `fengqudb::q::Rows::Row` | `template <class T> T operator[](const Col<T> &c) const` | 查询构建或结果读取 |
| `fengqudb::q::Rows::iterator` | `iterator(const Rows *owner, size_t at)` | 查询构建或结果读取 |
| `fengqudb::q::Rows::iterator` | `bool operator!=(const iterator &o) const` | 查询构建或结果读取 |
| `fengqudb::q::Rows::iterator` | `fengqudb::q::Rows::Row operator*() const` | 查询构建或结果读取 |
| `fengqudb::q::Rows::iterator` | `fengqudb::q::Rows::iterator & operator++()` | 查询构建或结果读取 |
| `fengqudb::q::Rows::iterator` | `bool operator==(const iterator &o) const` | 查询构建或结果读取 |

</details>

## 8. 索引：经常按字段查询

```cpp
db.define_index("student-score", "student/", Codec::Field_score, 5);
```

字段类型5表示有符号 ZigZag 整数。其他类型与索引选项见本节参数。

<details>
<summary>函数与参数（4项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo define_compound_index(std::string_view name, std::string_view prefix, const std::vector<StoreIndexField> &fields, uint32_t flags = 0, const StoreMaintenanceOptions &options = {})` | 定义或使用索引 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo define_index(std::string_view name, std::string_view prefix, uint32_t field, uint32_t type, uint32_t flags = 0, const StoreMaintenanceOptions &options = {})` | 定义或使用索引 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo drop_index(std::string_view name, const StoreMaintenanceOptions &options = {})` | 定义或使用索引 |
| `fengqudb::Store` | `fengqudb::StoreCommandResult query_index(std::string_view name, const StoreIndexQuery &query = {}, std::string_view cursor = {}, uint64_t limit = 256)` | 定义或使用索引 |

</details>

## 9. 可靠保存

默认性能模式为 50 ms / 1 MiB，普通写入在内存生效后返回。关键写传最后的可靠标志：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```cpp
db.put("payment/receipt", "paid", 0, true);
db.sync();
auto pending = db.submit_put("payment/next", "paid", 0, true);
auto result = pending.wait();
```

UI 线程可保存凭据、稍后 poll。COMMIT_UNKNOWN 时先核对业务编号，不要盲目重放付款。

<details>
<summary>函数与参数（11项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `void configure_write(const StoreWriteOptions &options)` | 控制可靠确认或提交等待 |
| `fengqudb::Store` | `void reset_write_options()` | 控制可靠确认或提交等待 |
| `fengqudb::Store` | `fengqudb::StoreCommit submit_put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, bool force_reliable = false)` | 控制可靠确认或提交等待 |
| `fengqudb::Store` | `void sync()` | 控制可靠确认或提交等待 |
| `fengqudb::Store` | `fengqudb::StoreWriteOptions write_options() const` | 控制可靠确认或提交等待 |
| `fengqudb::Store` | `fengqudb::StoreWriteState write_state() const` | 控制可靠确认或提交等待 |
| `fengqudb::StoreCommit` | `StoreCommit(StoreCommit &&) noexcept` | 提交事务或查看提交结果 |
| `fengqudb::StoreCommit` | `StoreCommit(fqdb_store_commit *handle)` | 提交事务或查看提交结果 |
| `fengqudb::StoreCommit` | `void cancel() const` | 控制可靠确认或提交等待 |
| `fengqudb::StoreCommit` | `std::optional<CommitResult> poll() const` | 控制可靠确认或提交等待 |
| `fengqudb::StoreCommit` | `fengqudb::CommitResult wait(uint64_t timeout_ms = 0) const` | 控制可靠确认或提交等待 |

</details>

## 10. 自动到期

```cpp
db.put("verification", "1234", expires_at_unix_ms);
```

时间为绝对 Unix 毫秒；0 不过期。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreExpiryInfo purge_expired(const StoreExpiryOptions &options = {})` | 到期数据操作 |
| `fengqudb::StoreValue` | `uint64_t expires_at_ms() const` | 到期数据操作 |

</details>

## 11. 热点读取

```cpp
auto key = db.prepare_key("student/7");
auto value = key.get();
auto reader = db.prepare_reader({"student/7", "student/8"});
auto rows = reader.read();
```

共享字节是只读借用，保持拥有对象存活。读取器的行只保留到下次 read/release；要长时间保存则复制。

<details>
<summary>函数与参数（12项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StorePreparedKey prepare_key(std::string_view key) const` | 复用键、缓冲或共享字节 |
| `fengqudb::Store` | `fengqudb::StoreReader prepare_reader(const std::vector<std::string_view> &keys) const` | 复用键、缓冲或共享字节 |
| `fengqudb::Store` | `fengqudb::StoreReader prepare_reader(std::string_view key) const` | 复用键、缓冲或共享字节 |
| `fengqudb::StorePreparedKey` | `StorePreparedKey(StorePreparedKey &&) noexcept` | 复用键、缓冲或共享字节 |
| `fengqudb::StorePreparedKey` | `std::optional<StoreValue> get() const` | 读取对应的数据或状态 |
| `fengqudb::StorePreparedKey` | `bool get_into(void *destination, uint64_t capacity, uint64_t &required) const` | 读取对应的数据或状态 |
| `fengqudb::StorePreparedKey` | `fengqudb::StorePreparedKey & operator=(StorePreparedKey &&) noexcept` | 复用键、缓冲或共享字节 |
| `fengqudb::StoreReader` | `StoreReader(StoreReader &&) noexcept` | 复用键、缓冲或共享字节 |
| `fengqudb::StoreReader` | `fengqudb::StoreValues acquire()` | 复用键、缓冲或共享字节 |
| `fengqudb::StoreReader` | `fengqudb::StoreReader & operator=(StoreReader &&) noexcept` | 复用键、缓冲或共享字节 |
| `fengqudb::StoreReader` | `fengqudb::StoreReadView read()` | 读取对应的数据或状态 |
| `fengqudb::StoreReader` | `void release()` | 释放对象或借用的资源 |

</details>

## 12. 一致快照

```cpp
auto snapshot = db.snapshot();
auto a = snapshot.get("a");
auto b = snapshot.get("b");
```

<details>
<summary>函数与参数（10项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreSnapshot snapshot() const` | 一致读取或范围遍历 |
| `fengqudb::StoreCursor` | `StoreCursor(StoreCursor &&) noexcept` | 一致读取或范围遍历 |
| `fengqudb::StoreCursor` | `std::optional<StoreRow> next()` | 一致读取或范围遍历 |
| `fengqudb::StoreCursor` | `fengqudb::StoreCursor & operator=(StoreCursor &&) noexcept` | 一致读取或范围遍历 |
| `fengqudb::StoreSnapshot` | `StoreSnapshot(StoreSnapshot &&) noexcept` | 一致读取或范围遍历 |
| `fengqudb::StoreSnapshot` | `std::optional<StoreValue> get(const StorePreparedKey &key) const` | 读取对应的数据或状态 |
| `fengqudb::StoreSnapshot` | `std::optional<StoreValue> get(std::string_view key) const` | 读取对应的数据或状态 |
| `fengqudb::StoreSnapshot` | `fengqudb::StoreValues get_many(const std::vector<std::string_view> &keys) const` | 读取对应的数据或状态 |
| `fengqudb::StoreSnapshot` | `fengqudb::StoreSnapshot & operator=(StoreSnapshot &&) noexcept` | 一致读取或范围遍历 |
| `fengqudb::StoreSnapshot` | `fengqudb::StoreCursor scan(std::string_view begin = {}, std::string_view end = {}, bool reverse = false) const` | 一致读取或范围遍历 |

</details>

## 13. 备份恢复

```cpp
db.backup("backups/full-001");
```

使用新的目标目录。恢复时使用 restore/recover_chain，先关闭要替换的库，不直接复制正在写入的文件。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo backup(std::string_view destination, const StoreMaintenanceOptions &options = {})` | 备份、恢复或历史保留 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo backup_incremental(std::string_view destination, uint64_t from, const StoreMaintenanceOptions &options = {})` | 备份、恢复或历史保留 |
| `fengqudb::Store` | `fengqudb::StoreHistoryInfo history_info() const` | 备份、恢复或历史保留 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo hold_history(std::string_view name, uint64_t from_version, uint64_t timeout_ms = 0)` | 备份、恢复或历史保留 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo recover_chain(std::string_view base, const std::vector<std::string> &archives, std::string_view destination, uint64_t target_version = 0, uint64_t target_unix_ms = 0, const StoreMaintenanceOptions &options = {})` | 备份、恢复或历史保留 |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo release_history(std::string_view name, uint64_t timeout_ms = 0)` | 释放对象或借用的资源 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo restore(std::string_view source, std::string_view destination, const StoreMaintenanceOptions &options = {})` | 备份、恢复或历史保留 |

</details>

## 14. 集合操作

execute 接收 Hash/Set/List/有序集合命令，hash_get 读取 Hash 字段。用于原子增删成员、队列和排名。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreCommandResult execute(const StoreCommand &command, bool force_reliable = false)` | 原生集合操作 |
| `fengqudb::Store` | `std::optional<StoreValue> hash_get(std::string_view scope, std::string_view key) const` | 读取对应的数据或状态 |

</details>

## 15. 清理和整理

```cpp
db.purge_expired();
db.compact();
db.verify();
```

<details>
<summary>函数与参数（6项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `void checkpoint()` | 查看状态或维护数据库 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo compact(const StoreMaintenanceOptions &options = {})` | 查看状态或维护数据库 |
| `fengqudb::Store` | `void configure(const StoreRuntimeOptions &options = {})` | 查看状态或维护数据库 |
| `fengqudb::Store` | `fengqudb::StoreInfo info() const` | 查看状态或维护数据库 |
| `fengqudb::Store` | `fengqudb::StoreRuntimeInfo runtime_info() const` | 查看状态或维护数据库 |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo verify(const StoreMaintenanceOptions &options = {}) const` | 查看状态或维护数据库 |

</details>

## 16. 消费变更

watch 创建变更消费者；处理后确认水位。用于进程内缓存更新和后台任务，句柄由 RAII 管理。

<details>
<summary>函数与参数（12项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreWatch watch(uint64_t from_version, std::string_view prefix = {}, const StoreWatchOptions &options = {})` | 消费变更或定位问题 |
| `fengqudb::StoreChanges` | `StoreChanges(StoreChanges &&) noexcept` | 消费变更或定位问题 |
| `fengqudb::StoreChanges` | `const fqdb_store_change_row * begin() const` | 消费变更或定位问题 |
| `fengqudb::StoreChanges` | `const fqdb_store_change_row * end() const` | 消费变更或定位问题 |
| `fengqudb::StoreChanges` | `const fqdb_store_change_batch_info & info() const` | 消费变更或定位问题 |
| `fengqudb::StoreChanges` | `fengqudb::StoreChanges & operator=(StoreChanges &&) noexcept` | 消费变更或定位问题 |
| `fengqudb::StoreWatch` | `StoreWatch(StoreWatch &&) noexcept` | 消费变更或定位问题 |
| `fengqudb::StoreWatch` | `void ack(uint64_t version)` | 消费变更或定位问题 |
| `fengqudb::StoreWatch` | `void cancel()` | 消费变更或定位问题 |
| `fengqudb::StoreWatch` | `std::optional<StoreChanges> next(uint64_t timeout_ms = 0)` | 消费变更或定位问题 |
| `fengqudb::StoreWatch` | `fengqudb::StoreWatch & operator=(StoreWatch &&) noexcept` | 消费变更或定位问题 |
| `fengqudb::StoreWatchOptions` | `fqdb_store_watch_options native() const noexcept` | 消费变更或定位问题 |

</details>

## 17. 实体编解码

Codec::encode/encode_into 直接访问自己的结构体；decode 返回原结构及未知字段。读改写用 encode(row)，避免丢掉新版本添加的字段。可复用 string 或调用方缓冲，输出不要与输入字段重叠。

<details>
<summary>函数与参数（34项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::query_wire` | `std::string_view bytes(wire::Reader &r, wire::Field f)` | 实体或二进制编解码 |
| `fengqudb::query_wire` | `uint64_t number(wire::Reader &r, wire::Field f)` | 实体或二进制编解码 |
| `fengqudb::query_wire` | `template <class Database> fengqudb::StoreCommandResult run(Database &db, uint32_t operation, std::string_view request)` | 实体或二进制编解码 |
| `fengqudb::query_wire` | `fengqudb::QueryValue value(std::string_view b)` | 实体或二进制编解码 |
| `fengqudb::wire` | `size_t bytes_size(uint32_t field, size_t length) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire` | `int64_t unzigzag(uint64_t value) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire` | `size_t varint_size(uint64_t value) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire` | `uint64_t zigzag(int64_t value) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `Reader(std::string_view bytes, size_t limit = 67108864) noexcept` | 读取对应的数据或状态 |
| `fengqudb::wire::Reader` | `bool bytes(std::string_view &value) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `bool done() const noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `bool fixed(uint64_t &value, unsigned count) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `bool good() const noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `bool next(Field &field) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `size_t position() const noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `bool skip(Field field) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Reader` | `bool varint(uint64_t &value) noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `Writer() noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `Writer(std::string storage)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void bytes_field(uint32_t field, std::string_view value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void clear() noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `const std::string & data() const noexcept` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void double_field(uint32_t field, double value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void fixed(uint64_t value, unsigned count)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void fixed32_field(uint32_t field, uint32_t value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void fixed64_field(uint32_t field, uint64_t value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void float_field(uint32_t field, float value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void message_header(uint32_t field, size_t length)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void reserve(size_t size)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void sint_field(uint32_t field, int64_t value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void tag(uint32_t field, Type type)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `std::string take()` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void uint_field(uint32_t field, uint64_t value)` | 实体或二进制编解码 |
| `fengqudb::wire::Writer` | `void varint(uint64_t value)` | 实体或二进制编解码 |

</details>

## 其他辅助接口

<details>
<summary>函数与参数（35项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb::Buffer` | `Buffer() noexcept` | 辅助操作 |
| `fengqudb::Buffer` | `Buffer(Buffer &&other) noexcept` | 辅助操作 |
| `fengqudb::Buffer` | `Buffer(const Buffer &)` | 辅助操作 |
| `fengqudb::Buffer` | `const char * data() const noexcept` | 辅助操作 |
| `fengqudb::Buffer` | `bool empty() const noexcept` | 辅助操作 |
| `fengqudb::Buffer` | `size_t size() const noexcept` | 辅助操作 |
| `fengqudb::Buffer` | `std::string str() const` | 辅助操作 |
| `fengqudb::Buffer` | `std::string_view view() const noexcept` | 辅助操作 |
| `fengqudb::Buffer` | `~Buffer() noexcept` | 辅助操作 |
| `fengqudb::Error` | `Error(fqdb_status code)` | 辅助操作 |
| `fengqudb::Error` | `fqdb_status code() const noexcept` | 辅助操作 |
| `fengqudb::Error` | `fengqudb::Status status() const noexcept` | 辅助操作 |
| `fengqudb::Store` | `Store(Store &&) noexcept` | 辅助操作 |
| `fengqudb::Store` | `Store(std::string_view directory, const StoreOptions &options = {})` | 辅助操作 |
| `fengqudb::Store` | `void cancel_maintenance()` | 辅助操作 |
| `fengqudb::Store` | `fengqudb::Store open_at(const std::string &root, const std::string &name, const StoreOptions &options = {})` | 辅助操作 |
| `fengqudb::Store` | `std::string path_at(const std::string &root, const std::string &name)` | 辅助操作 |
| `fengqudb::Store` | `fengqudb::StoreReader prepare_project_reader(const std::vector<std::string_view> &keys, const std::vector<uint32_t> &fields) const` | 辅助操作 |
| `fengqudb::Store` | `std::string project(std::string_view key, const std::vector<uint32_t> &fields)` | 辅助操作 |
| `fengqudb::StoreCommand` | `std::string encode() const` | 辅助操作 |
| `fengqudb::StoreCommandResult` | `StoreCommandResult()` | 辅助操作 |
| `fengqudb::StoreCommandResult` | `StoreCommandResult(std::string_view bytes)` | 辅助操作 |
| `fengqudb::StoreExpiryOptions` | `fqdb_store_expiry_options native() const noexcept` | 辅助操作 |
| `fengqudb::StoreMaintenanceOptions` | `fqdb_store_maintenance_options native() const noexcept` | 辅助操作 |
| `fengqudb::StoreOptions` | `fqdb_store_options native() const noexcept` | 辅助操作 |
| `fengqudb::StoreReadView` | `StoreReadView(const fqdb_store_result_row *rows, uint32_t count)` | 辅助操作 |
| `fengqudb::StoreReadView` | `uint32_t size() const noexcept` | 辅助操作 |
| `fengqudb::StoreRuntimeOptions` | `fqdb_store_runtime_options native() const noexcept` | 辅助操作 |
| `fengqudb::StoreValue` | `StoreValue(StoreValue &&) noexcept` | 辅助操作 |
| `fengqudb::StoreValue` | `std::string string() const` | 辅助操作 |
| `fengqudb::StoreValue` | `uint64_t version() const` | 辅助操作 |
| `fengqudb::StoreValue` | `std::string_view view() const` | 辅助操作 |
| `fengqudb::StoreValues` | `StoreValues(StoreValues &&) noexcept` | 辅助操作 |
| `fengqudb::StoreValues` | `uint32_t size() const` | 辅助操作 |
| `fengqudb::StoreWriteOptions` | `fqdb_store_write_options native() const` | 辅助操作 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>fengqudb</summary>

| 声明 |
| --- |
| `class Buffer` |
| `struct CommitResult` |
| `enum class Durability` |
| `class Error` |
| `enum class Operation` |
| `struct QueryAccess` |
| `enum class QueryAggregate` |
| `struct QueryColumn` |
| `struct QueryExpr` |
| `struct QueryField` |
| `struct QueryJoin` |
| `enum class QueryJoinKind` |
| `enum class QueryKind` |
| `enum class QueryOp` |
| `struct QueryOrder` |
| `struct QueryRow` |
| `struct QueryStats` |
| `struct QueryValue` |
| `struct ReadResult` |
| `enum class Status` |
| `class Store` |
| `fqdb_store_atomic_info StoreAtomicInfo` |
| `class StoreBatch` |
| `class StoreChanges` |
| `struct StoreCommand` |
| `struct StoreCommandResult` |
| `class StoreCommit` |
| `class StoreCursor` |
| `fqdb_store_expiry_info StoreExpiryInfo` |
| `struct StoreExpiryOptions` |
| `struct StoreFeatureRow` |
| `fqdb_store_history_info StoreHistoryInfo` |
| `struct StoreI64FieldUpdate` |
| `fqdb_store_index_field StoreIndexField` |
| `struct StoreIndexQuery` |
| `fqdb_store_info StoreInfo` |
| `fqdb_store_maintenance_io StoreMaintenanceIO` |
| `fqdb_store_maintenance_info StoreMaintenanceInfo` |
| `struct StoreMaintenanceOptions` |
| `struct StoreOptions` |
| `class StorePreparedKey` |
| `class StorePreparedQuery` |
| `struct StorePut` |
| `struct StoreQuery` |
| `struct StoreQueryResult` |
| `class StoreReadView` |
| `class StoreReader` |
| `struct StoreResult` |
| `struct StoreRow` |
| `fqdb_store_runtime_info StoreRuntimeInfo` |
| `struct StoreRuntimeOptions` |
| `class StoreSnapshot` |
| `class StoreTransaction` |
| `class StoreValue` |
| `class StoreValues` |
| `class StoreWatch` |
| `struct StoreWatchOptions` |
| `enum class StoreWriteMode` |
| `struct StoreWriteOptions` |
| `fqdb_store_write_state StoreWriteState` |

</details>

<details>
<summary>fengqudb::CommitResult</summary>

| 声明 |
| --- |
| `fengqudb::Status status` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::Durability</summary>

| 声明 |
| --- |
| `fengqudb::Durability buffered = 1` |
| `fengqudb::Durability sync = 0` |

</details>

<details>
<summary>fengqudb::Operation</summary>

| 声明 |
| --- |
| `fengqudb::Operation erase = 2` |
| `fengqudb::Operation put = 1` |

</details>

<details>
<summary>fengqudb::QueryAccess</summary>

| 声明 |
| --- |
| `bool covering = False` |
| `std::string index` |
| `bool needs_scan = False` |
| `bool ordered = False` |
| `std::string reason` |
| `uint64_t sampled_candidates` |
| `uint32_t source` |
| `std::string strategy` |

</details>

<details>
<summary>fengqudb::QueryAggregate</summary>

| 声明 |
| --- |
| `fengqudb::QueryAggregate Avg` |
| `fengqudb::QueryAggregate Count` |
| `fengqudb::QueryAggregate CountAll` |
| `fengqudb::QueryAggregate Max` |
| `fengqudb::QueryAggregate Min` |
| `fengqudb::QueryAggregate None` |
| `fengqudb::QueryAggregate Sum` |

</details>

<details>
<summary>fengqudb::QueryColumn</summary>

| 声明 |
| --- |
| `fengqudb::QueryAggregate aggregate` |
| `bool distinct = False` |
| `fengqudb::QueryField field` |
| `std::string name` |

</details>

<details>
<summary>fengqudb::QueryExpr</summary>

| 声明 |
| --- |
| `std::vector<QueryExpr> children` |
| `fengqudb::QueryField field` |
| `fengqudb::QueryOp op` |
| `std::optional<QueryField> right` |
| `std::vector<QueryValue> values` |

</details>

<details>
<summary>fengqudb::QueryField</summary>

| 声明 |
| --- |
| `std::vector<uint32_t> path` |
| `bool repeated = False` |
| `uint32_t source` |
| `uint32_t type` |

</details>

<details>
<summary>fengqudb::QueryJoin</summary>

| 声明 |
| --- |
| `fengqudb::QueryJoinKind kind` |
| `std::vector<QueryField> left` |
| `fengqudb::QueryExpr on` |
| `std::string prefix` |
| `std::vector<QueryField> right` |

</details>

<details>
<summary>fengqudb::QueryJoinKind</summary>

| 声明 |
| --- |
| `fengqudb::QueryJoinKind Anti` |
| `fengqudb::QueryJoinKind Inner` |
| `fengqudb::QueryJoinKind Left` |
| `fengqudb::QueryJoinKind Semi` |

</details>

<details>
<summary>fengqudb::QueryKind</summary>

| 声明 |
| --- |
| `fengqudb::QueryKind Bool` |
| `fengqudb::QueryKind Bytes` |
| `fengqudb::QueryKind Int` |
| `fengqudb::QueryKind Null` |
| `fengqudb::QueryKind Real` |
| `fengqudb::QueryKind UInt` |

</details>

<details>
<summary>fengqudb::QueryOp</summary>

| 声明 |
| --- |
| `fengqudb::QueryOp And` |
| `fengqudb::QueryOp Contains` |
| `fengqudb::QueryOp EndsWith` |
| `fengqudb::QueryOp Eq` |
| `fengqudb::QueryOp Exists` |
| `fengqudb::QueryOp False` |
| `fengqudb::QueryOp Fuzzy` |
| `fengqudb::QueryOp Ge` |
| `fengqudb::QueryOp Gt` |
| `fengqudb::QueryOp IContains` |
| `fengqudb::QueryOp ILike` |
| `fengqudb::QueryOp IRegex` |
| `fengqudb::QueryOp In` |
| `fengqudb::QueryOp IsNull` |
| `fengqudb::QueryOp Le` |
| `fengqudb::QueryOp Like` |
| `fengqudb::QueryOp Lt` |
| `fengqudb::QueryOp Ne` |
| `fengqudb::QueryOp Not` |
| `fengqudb::QueryOp Or` |
| `fengqudb::QueryOp Regex` |
| `fengqudb::QueryOp StartsWith` |
| `fengqudb::QueryOp True` |

</details>

<details>
<summary>fengqudb::QueryOrder</summary>

| 声明 |
| --- |
| `bool descending = False` |
| `fengqudb::QueryField field` |
| `bool nulls_first = True` |

</details>

<details>
<summary>fengqudb::QueryRow</summary>

| 声明 |
| --- |
| `std::string key` |
| `std::vector<QueryValue> values` |

</details>

<details>
<summary>fengqudb::QueryStats</summary>

| 声明 |
| --- |
| `uint64_t candidates` |
| `bool complete = True` |
| `uint64_t join_probes` |
| `uint64_t matched` |
| `uint64_t payload_reads` |
| `uint64_t peak_memory` |
| `uint64_t result_total` |
| `uint64_t work` |

</details>

<details>
<summary>fengqudb::QueryValue</summary>

| 声明 |
| --- |
| `std::string bytes` |
| `fengqudb::QueryKind kind` |
| `double real_value` |
| `int64_t signed_value` |
| `uint64_t unsigned_value` |

</details>

<details>
<summary>fengqudb::ReadResult</summary>

| 声明 |
| --- |
| `uint64_t required` |
| `fengqudb::Status status` |

</details>

<details>
<summary>fengqudb::Status</summary>

| 声明 |
| --- |
| `fengqudb::Status already_exists = 15` |
| `fengqudb::Status async_required = 21` |
| `fengqudb::Status buffer_too_small = 16` |
| `fengqudb::Status cancelled = 19` |
| `fengqudb::Status commit_unknown = 10` |
| `fengqudb::Status conflict = 7` |
| `fengqudb::Status constraint = 20` |
| `fengqudb::Status corrupt = 5` |
| `fengqudb::Status end = 2` |
| `fengqudb::Status history_lost = 14` |
| `fengqudb::Status internal_error = 13` |
| `fengqudb::Status invalid_argument = 3` |
| `fengqudb::Status io_error = 4` |
| `fengqudb::Status limit = 9` |
| `fengqudb::Status locked = 6` |
| `fengqudb::Status no_memory = 8` |
| `fengqudb::Status not_found = 1` |
| `fengqudb::Status not_modified = 18` |
| `fengqudb::Status ok = 0` |
| `fengqudb::Status recovery_required = 11` |
| `fengqudb::Status timeout = 17` |
| `fengqudb::Status unsupported = 12` |

</details>

<details>
<summary>fengqudb::StoreCommand</summary>

| 声明 |
| --- |
| `int64_t argument` |
| `uint32_t flags` |
| `std::string_view key` |
| `uint64_t limit` |
| `uint32_t operation` |
| `std::string_view scope` |
| `double score` |
| `std::string_view value` |

</details>

<details>
<summary>fengqudb::StoreCommandResult</summary>

| 声明 |
| --- |
| `bool changed = False` |
| `uint64_t count` |
| `std::string cursor` |
| `std::vector<StoreFeatureRow> rows` |
| `std::optional<std::string> value` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::StoreExpiryOptions</summary>

| 声明 |
| --- |
| `uint64_t max_batch_bytes` |
| `uint32_t max_keys` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fengqudb::StoreFeatureRow</summary>

| 声明 |
| --- |
| `std::string key` |
| `double score` |
| `std::string value` |

</details>

<details>
<summary>fengqudb::StoreI64FieldUpdate</summary>

| 声明 |
| --- |
| `int64_t delta` |
| `std::string_view key` |
| `int64_t maximum` |
| `int64_t minimum` |
| `uint64_t offset` |

</details>

<details>
<summary>fengqudb::StoreIndexQuery</summary>

| 声明 |
| --- |
| `std::string equal` |
| `std::vector<uint32_t> fields` |
| `bool include_values = False` |
| `std::optional<std::string> lower` |
| `bool lower_exclusive = False` |
| `bool reverse = False` |
| `std::optional<std::string> upper` |
| `bool upper_inclusive = False` |

</details>

<details>
<summary>fengqudb::StoreMaintenanceOptions</summary>

| 声明 |
| --- |
| `fengqudb::StoreMaintenanceIO * io_stats` |
| `uint64_t max_barrier_bytes` |
| `uint32_t max_catchup_rounds` |
| `uint64_t max_read_bytes` |
| `uint64_t max_write_bytes` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fengqudb::StoreOptions</summary>

| 声明 |
| --- |
| `uint64_t all_transaction_bytes` |
| `uint64_t cache_bytes` |
| `bool create = True` |
| `uint64_t delta_bytes` |
| `fengqudb::Durability durability` |
| `uint64_t max_snapshots` |
| `uint64_t pinned_bytes` |
| `bool segmented = False` |
| `uint64_t transaction_bytes` |
| `uint64_t writer_bytes` |

</details>

<details>
<summary>fengqudb::StorePut</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `std::string_view key` |
| `std::string_view value` |

</details>

<details>
<summary>fengqudb::StoreQuery</summary>

| 声明 |
| --- |
| `std::string after` |
| `bool allow_scan = True` |
| `std::vector<QueryColumn> columns` |
| `bool count_total = False` |
| `bool distinct = False` |
| `std::vector<QueryField> group` |
| `fengqudb::QueryExpr having` |
| `std::string index_hint` |
| `std::vector<QueryJoin> joins` |
| `uint64_t limit` |
| `uint64_t max_candidates` |
| `uint64_t max_work` |
| `uint64_t memory_bytes` |
| `uint64_t offset` |
| `std::vector<QueryOrder> order` |
| `std::string prefix` |
| `uint64_t timeout_ms` |
| `fengqudb::QueryExpr where` |

</details>

<details>
<summary>fengqudb::StoreQueryResult</summary>

| 声明 |
| --- |
| `std::vector<QueryAccess> access` |
| `std::string cursor` |
| `std::vector<QueryRow> rows` |
| `fengqudb::QueryStats stats` |

</details>

<details>
<summary>fengqudb::StoreResult</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `std::optional<std::string_view> value` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::StoreRow</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `std::string_view key` |
| `std::string_view value` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::StoreRuntimeOptions</summary>

| 声明 |
| --- |
| `bool background = False` |
| `uint64_t compact_growth_bytes` |
| `uint64_t compact_min_bytes` |
| `uint32_t expiry_keys` |
| `uint32_t garbage_percent` |
| `uint32_t group_delay_us` |
| `uint32_t group_max` |
| `uint32_t interval_ms` |
| `uint64_t job_max_read_bytes` |
| `uint64_t job_max_write_bytes` |
| `uint64_t job_timeout_ms` |

</details>

<details>
<summary>fengqudb::StoreWatchOptions</summary>

| 声明 |
| --- |
| `uint32_t flags` |
| `uint64_t max_batch_bytes` |
| `uint32_t max_scan_commits` |

</details>

<details>
<summary>fengqudb::StoreWriteMode</summary>

| 声明 |
| --- |
| `fengqudb::StoreWriteMode Performance = 1` |
| `fengqudb::StoreWriteMode Reliable = 0` |

</details>

<details>
<summary>fengqudb::StoreWriteOptions</summary>

| 声明 |
| --- |
| `fengqudb::StoreWriteMode mode` |
| `uint32_t period_ms` |
| `uint64_t threshold_bytes` |

</details>

<details>
<summary>fengqudb::q</summary>

| 声明 |
| --- |
| `template <class T> class Agg` |
| `template <class T> class Col` |
| `struct Cond` |
| `struct Order` |
| `class Prepared` |
| `class Query` |
| `class Rows` |
| `fengqudb::QueryColumn column` |
| `fengqudb::QueryField field` |

</details>

<details>
<summary>fengqudb::q::Agg</summary>

| 声明 |
| --- |
| `fengqudb::QueryColumn column` |

</details>

<details>
<summary>fengqudb::q::Col</summary>

| 声明 |
| --- |
| `fengqudb::QueryField field` |

</details>

<details>
<summary>fengqudb::q::Cond</summary>

| 声明 |
| --- |
| `fengqudb::QueryExpr expr` |
| `std::vector<QueryColumn> refs` |

</details>

<details>
<summary>fengqudb::q::Order</summary>

| 声明 |
| --- |
| `fengqudb::QueryOrder order` |
| `std::optional<QueryColumn> output` |

</details>

<details>
<summary>fengqudb::q::Rows</summary>

| 声明 |
| --- |
| `class Row` |
| `class iterator` |

</details>

<details>
<summary>fengqudb::wire</summary>

| 声明 |
| --- |
| `struct Field` |
| `class Reader` |
| `enum class Type` |
| `class Writer` |

</details>

<details>
<summary>fengqudb::wire::Field</summary>

| 声明 |
| --- |
| `uint32_t number` |
| `fengqudb::wire::Type type` |

</details>

<details>
<summary>fengqudb::wire::Type</summary>

| 声明 |
| --- |
| `fengqudb::wire::Type Bytes = 2` |
| `fengqudb::wire::Type EndGroup = 4` |
| `fengqudb::wire::Type Fixed32 = 5` |
| `fengqudb::wire::Type Fixed64 = 1` |
| `fengqudb::wire::Type StartGroup = 3` |
| `fengqudb::wire::Type Varint = 0` |

</details>
