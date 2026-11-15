# C++ API and examples

[Getting started](../languages/cpp.md) · [简体中文](../../api/cpp.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read: find data by key

```cpp
if (auto value = db.get("student/7")) {
    auto row = Codec::decode(value->view());
    auto grade = row.value.score;
}
```

A missing record returns an empty optional. Codec in this page refers to StudentFqCodec generated in the getting-started guide.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `std::optional<StoreValue> get(std::string_view key) const` | Read data or status |

</details>

## 2. Insert or save

```cpp
Student student{7, "Alice", 95};
db.put(Codec::key(student), Codec::encode(student));
db.put("name", "Alice"); // Store a single string directly as UTF-8
```

put overwrites the previous value at the same key.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, bool force_reliable = false)` | Save or set data |

</details>

## 3. Updates and atomic operations

```cpp
if (auto bytes = db.get("student/7")) {
    auto row = Codec::decode(bytes->view());
    row.value.score = 96;
    db.put(Codec::key(row.value), Codec::encode(row));
}
db.add_i64("login-count", 1);
```

add_i64 uses eight-byte little-endian integers; do not apply it to encoded entities. compare_exchange updates by version, put_if_absent inserts only when missing, and patch changes raw byte ranges.

<details>
<summary>Functions and parameters (8)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo add_i64(std::string_view key, int64_t delta, int64_t minimum, int64_t maximum, uint64_t timeout_ms = 0, bool force_reliable = false)` | Conditional update or partial modification |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo add_i64_fields(const fqdb_store_i64_field_update *items, uint32_t count, uint64_t timeout_ms = 0, bool force_reliable = false)` | Conditional update or partial modification |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo add_i64_fields(const std::vector<StoreI64FieldUpdate> &items, uint64_t timeout_ms = 0, bool force_reliable = false)` | Conditional update or partial modification |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo compare_exchange(std::string_view key, uint64_t expected, std::string_view value, uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0, bool force_reliable = false)` | Conditional update or partial modification |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo delete_if_version(std::string_view key, uint64_t expected, uint64_t timeout_ms = 0, bool force_reliable = false)` | Delete a record or member |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo patch(std::string_view key, uint64_t offset, uint64_t remove, std::string_view insert, bool force_reliable = false)` | Conditional update or partial modification |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo put_if_absent(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0, bool force_reliable = false)` | Save or set data |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo put_if_changed(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0, bool force_reliable = false)` | Save or set data |

</details>

## 4. Delete

```cpp
db.erase("student/7");
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo erase(std::string_view key, bool force_reliable = false)` | Delete a record or member |

</details>

## 5. Batch operations

```cpp
db.put_many({{"a", "one"}, {"b", "two"}});
auto values = db.get_many({"a", "b"});
```

put_many is one atomic batch write.

<details>
<summary>Functions and parameters (6)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `uint64_t delete_many(const std::vector<std::string_view> &keys, bool force_reliable = false)` | Delete a record or member |
| `fengqudb::Store` | `fengqudb::StoreValues get_many(const std::vector<std::string_view> &keys) const` | Read data or status |
| `fengqudb::Store` | `fengqudb::StoreBatch prepare_batch(const std::vector<StorePut> &items) const` | Process a batch |
| `fengqudb::Store` | `uint64_t put_batch(const StoreBatch &batch, bool force_reliable = false)` | Save or set data |
| `fengqudb::Store` | `uint64_t put_many(const std::vector<StorePut> &items, bool force_reliable = false)` | Save or set data |
| `fengqudb::StoreBatch` | `StoreBatch(StoreBatch &&) noexcept` | Process a batch |

</details>

## 6. Transactions: commit changes together

```cpp
auto tx = db.transaction();
tx.put("bag/item", "sword");
tx.erase("reward/pending");
auto result = tx.commit();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));
```

Destroying an uncommitted transaction discards its changes. On CONFLICT, create a new transaction, read again, and recompute.

<details>
<summary>Functions and parameters (22)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreTransaction transaction(uint64_t timeout_ms = 0)` | Transaction operation |
| `fengqudb::StoreTransaction` | `StoreTransaction(StoreTransaction &&) noexcept` | Transaction operation |
| `fengqudb::StoreTransaction` | `void cancel()` | Transaction operation |
| `fengqudb::StoreTransaction` | `fengqudb::CommitResult commit(bool force_reliable = false)` | Commit a transaction or inspect its result |
| `fengqudb::StoreTransaction` | `void delete_many(const std::vector<std::string_view> &keys)` | Delete a record or member |
| `fengqudb::StoreTransaction` | `void erase(std::string_view key)` | Delete a record or member |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCommandResult execute(const StoreCommand &command)` | Transaction operation |
| `fengqudb::StoreTransaction` | `void expire(std::string_view key, uint64_t expires_at_ms)` | Transaction operation |
| `fengqudb::StoreTransaction` | `std::optional<std::string> get(std::string_view key)` | Read data or status |
| `fengqudb::StoreTransaction` | `fengqudb::StoreValues get_many(const std::vector<std::string_view> &keys)` | Read data or status |
| `fengqudb::StoreTransaction` | `fengqudb::StoreTransaction & operator=(StoreTransaction &&) noexcept` | Transaction operation |
| `fengqudb::StoreTransaction` | `void patch(std::string_view key, uint64_t offset, uint64_t remove, std::string_view insert)` | Transaction operation |
| `fengqudb::StoreTransaction` | `std::string project(std::string_view key, const std::vector<uint32_t> &fields)` | Transaction operation |
| `fengqudb::StoreTransaction` | `void put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0)` | Save or set data |
| `fengqudb::StoreTransaction` | `void put_batch(const StoreBatch &batch)` | Save or set data |
| `fengqudb::StoreTransaction` | `void put_many(const std::vector<StorePut> &items)` | Save or set data |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCommandResult query_index(std::string_view name, const StoreIndexQuery &query = {}, std::string_view cursor = {}, uint64_t limit = 256)` | Transaction operation |
| `fengqudb::StoreTransaction` | `void release(uint64_t id)` | Release an object or borrowed resource |
| `fengqudb::StoreTransaction` | `void rollback_to(uint64_t id)` | Roll back to a savepoint |
| `fengqudb::StoreTransaction` | `uint64_t savepoint()` | Create or manage a transaction savepoint |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCursor scan(std::string_view begin = {}, std::string_view end = {}, bool reverse = false)` | Transaction operation |
| `fengqudb::StoreTransaction` | `fengqudb::StoreCommit submit(bool force_reliable = false)` | Transaction operation |

</details>

## 7. Filtering, sorting, and aggregation

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

Combine conditions, aggregates, groups, and equality joins. prepare reuses a query; explain shows its execution plan.

<details>
<summary>Functions and parameters (190)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `template <class Database> fengqudb::StoreQueryResult explain(Database &db, const StoreQuery &q)` | Inspect the query plan |
| `fengqudb` | `template <class Database> fengqudb::StoreQueryResult select(Database &db, const StoreQuery &q)` | Build a query or read its results |
| `fengqudb::Buffer` | `fengqudb::Buffer & operator=(Buffer &&other) noexcept` | Build a query or read its results |
| `fengqudb::Buffer` | `fengqudb::Buffer & operator=(const Buffer &)` | Build a query or read its results |
| `fengqudb::CommitResult` | `operator bool() const noexcept` | Build a query or read its results |
| `fengqudb::QueryColumn` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr all(std::vector<QueryExpr> c)` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr any(std::vector<QueryExpr> c)` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr compare(QueryField f, QueryOp op, QueryValue value)` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr compare_fields(QueryField f, QueryOp op, QueryField right)` | Build a query or read its results |
| `fengqudb::QueryExpr` | `std::string encode(unsigned depth = 0) const` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr negate(QueryExpr c)` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr one_of(QueryField f, std::vector<QueryValue> values)` | Build a query or read its results |
| `fengqudb::QueryExpr` | `fengqudb::QueryExpr test(QueryField f, QueryOp op)` | Build a query or read its results |
| `fengqudb::QueryField` | `fengqudb::QueryField any() const` | Build a query or read its results |
| `fengqudb::QueryField` | `fengqudb::QueryField column(uint32_t n)` | Build a query or read its results |
| `fengqudb::QueryField` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::QueryField` | `fengqudb::QueryField field(uint32_t number, uint32_t type, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::QueryField` | `fengqudb::QueryField key(uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::QueryField` | `bool operator!=(const QueryField &o) const` | Build a query or read its results |
| `fengqudb::QueryField` | `bool operator==(const QueryField &o) const` | Build a query or read its results |
| `fengqudb::QueryField` | `fengqudb::QueryField record(uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::QueryJoin` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::QueryOrder` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::QueryValue` | `fengqudb::QueryValue boolean(bool v)` | Build a query or read its results |
| `fengqudb::QueryValue` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::QueryValue` | `fengqudb::QueryValue integer(int64_t v)` | Build a query or read its results |
| `fengqudb::QueryValue` | `fengqudb::QueryValue real(double v)` | Build a query or read its results |
| `fengqudb::QueryValue` | `fengqudb::QueryValue text(std::string v)` | Build a query or read its results |
| `fengqudb::QueryValue` | `fengqudb::QueryValue uint(uint64_t v)` | Build a query or read its results |
| `fengqudb::ReadResult` | `operator bool() const noexcept` | Build a query or read its results |
| `fengqudb::Store` | `fengqudb::Store & operator=(Store &&) noexcept` | Build a query or read its results |
| `fengqudb::Store` | `fengqudb::StoreCommandResult select_command(const StoreCommand &command) const` | Build a query or read its results |
| `fengqudb::StoreBatch` | `fengqudb::StoreBatch & operator=(StoreBatch &&) noexcept` | Build a query or read its results |
| `fengqudb::StoreCommit` | `fengqudb::StoreCommit & operator=(StoreCommit &&) noexcept` | Build a query or read its results |
| `fengqudb::StoreIndexQuery` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::StorePreparedQuery` | `StorePreparedQuery(const StoreQuery &q)` | Build a query or read its results |
| `fengqudb::StorePreparedQuery` | `template <class Database> fengqudb::StoreQueryResult execute(Database &db, bool explain_only = false) const` | Build a query or read its results |
| `fengqudb::StoreQuery` | `std::string encode() const` | Build a query or read its results |
| `fengqudb::StoreQueryResult` | `StoreQueryResult(std::string_view bytes)` | Build a query or read its results |
| `fengqudb::StoreReadView` | `fengqudb::StoreResult operator[](uint32_t index) const` | Build a query or read its results |
| `fengqudb::StoreValue` | `fengqudb::StoreValue & operator=(StoreValue &&) noexcept` | Build a query or read its results |
| `fengqudb::StoreValues` | `fengqudb::StoreValues & operator=(StoreValues &&) noexcept` | Build a query or read its results |
| `fengqudb::StoreValues` | `fengqudb::StoreResult operator[](uint32_t index) const` | Build a query or read its results |
| `fengqudb::q` | `Col(QueryField f)` | Build a query or read its results |
| `fengqudb::q` | `Col(uint32_t number, uint32_t type, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Col<bool> any() const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Col<double> any() const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Col<long> any() const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Col<std::basic_string<char>> any() const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Col<unsigned long> any() const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Order asc(bool nulls_first = true) const` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Agg<double> avg(const Col<T> &c)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond between(const T &low, const T &high) const` | Build a query or read its results |
| `fengqudb::q` | `Col<bool> boolean(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond contains(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `Agg<uint64_t> count()` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Agg<uint64_t> count(const Col<T> &c)` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Agg<uint64_t> count_distinct(const Col<T> &c)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Order desc(bool nulls_first = true) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond ends_with(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond exists() const` | Build a query or read its results |
| `fengqudb::q` | `template <class Database> fengqudb::StoreQueryResult explain(Database &db, const Query &q)` | Inspect the query plan |
| `fengqudb::q` | `Col<double> f32(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `Col<double> f64(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `Col<uint64_t> fixed32(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `Col<uint64_t> fixed64(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Query from(std::string prefix)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond fuzzy(const std::string &s, uint64_t distance) const` | Build a query or read its results |
| `fengqudb::q` | `Col<int64_t> i64(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond icontains(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond ilike(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond in(const std::vector<T> &values) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond in(std::initializer_list<T> values) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond is_null() const` | Build a query or read its results |
| `fengqudb::q` | `Col<std::string> key(uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond like(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Agg<T> max(const Col<T> &c)` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Agg<T> min(const Col<T> &c)` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Col<T> nested(std::vector<uint32_t> path, uint32_t type, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator!(Cond a)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator!=(const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator!=(const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator&&(Cond a, Cond b)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator<(const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator<(const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator<=(const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator<=(const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator==(const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator==(const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator>(const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator>(const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator>=(const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator>=(const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond operator\|\|(Cond a, Cond b)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Prepared prepare() const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond regex(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `template <class Database> fengqudb::q::Rows run(Database &db, const Query &q)` | Build a query or read its results |
| `fengqudb::q` | `Col<int64_t> s64(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `Col<int64_t> sfixed32(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `Col<int64_t> sfixed64(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond starts_with(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q` | `template <class T> Agg<T> sum(const Col<T> &c)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond test(QueryOp op, const Col &o) const` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond test(QueryOp op, const T &v) const` | Build a query or read its results |
| `fengqudb::q` | `Col<std::string> text(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q` | `fengqudb::q::Cond text_test(QueryOp op, const std::string &pattern) const` | Build a query or read its results |
| `fengqudb::q` | `Col<uint64_t> u64(uint32_t n, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Order asc(bool nulls_first = true) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Order desc(bool nulls_first = true) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator!=(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator<(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator<=(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator==(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator>(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Agg` | `fengqudb::q::Cond operator>=(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `Col<T>(QueryField f)` | Build a query or read its results |
| `fengqudb::q::Col` | `Col<T>(uint32_t number, uint32_t type, uint32_t source = 0)` | Build a query or read its results |
| `fengqudb::q::Col` | `Col<T> any() const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Order asc(bool nulls_first = true) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond between(const T &low, const T &high) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond contains(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Order desc(bool nulls_first = true) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond ends_with(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond exists() const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond fuzzy(const std::string &s, uint64_t distance) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond icontains(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond ilike(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond in(const std::vector<T> &values) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond in(std::initializer_list<T> values) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond is_null() const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond like(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator!=(const Col &o) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator!=(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<(const Col &o) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<=(const Col &o) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator<=(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator==(const Col &o) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator==(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>(const Col &o) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>=(const Col &o) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond operator>=(const T &v) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond regex(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Col` | `fengqudb::q::Cond starts_with(const std::string &s) const` | Build a query or read its results |
| `fengqudb::q::Prepared` | `Prepared(const Query &q)` | Build a query or read its results |
| `fengqudb::q::Prepared` | `template <class Database> fengqudb::q::Rows run(Database &db) const` | Build a query or read its results |
| `fengqudb::q::Query` | `Query(std::string prefix)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & after(std::string cursor)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & allow_scan(bool on)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::StoreQuery build() const` | Build a query or read its results |
| `fengqudb::q::Query` | `const std::vector<QueryColumn> & columns() const noexcept` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & count_total(bool on = true)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & distinct(bool on = true)` | Build a query or read its results |
| `fengqudb::q::Query` | `template <class... T> fengqudb::q::Query & group_by(const Col<T> &...cols)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & having(Cond c)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & index(std::string name)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & join(QueryJoin j)` | Build a query or read its results |
| `fengqudb::q::Query` | `template <class T> fengqudb::q::Query & join(std::string prefix, const Col<T> &left, const Col<T> &right, QueryJoinKind kind = QueryJoinKind::Inner)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & limit(uint64_t n)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & max_candidates(uint64_t n)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & memory(uint64_t bytes)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & offset(uint64_t n)` | Build a query or read its results |
| `fengqudb::q::Query` | `template <class... Items> fengqudb::q::Query & order_by(const Items &...items)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Prepared prepare() const` | Build a query or read its results |
| `fengqudb::q::Query` | `template <class... Items> fengqudb::q::Query & select(const Items &...items)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & timeout_ms(uint64_t ms)` | Build a query or read its results |
| `fengqudb::q::Query` | `fengqudb::q::Query & where(Cond c)` | Build a query or read its results |
| `fengqudb::q::Rows` | `Rows(std::vector<QueryColumn> columns, StoreQueryResult result)` | Build a query or read its results |
| `fengqudb::q::Rows` | `const std::vector<QueryAccess> & access() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows` | `fengqudb::q::Rows::iterator begin() const` | Build a query or read its results |
| `fengqudb::q::Rows` | `const std::string & cursor() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows` | `bool empty() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows` | `fengqudb::q::Rows::iterator end() const` | Build a query or read its results |
| `fengqudb::q::Rows` | `fengqudb::q::Rows::Row operator[](size_t i) const` | Build a query or read its results |
| `fengqudb::q::Rows` | `const fengqudb::StoreQueryResult & raw() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows` | `size_t size() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows` | `const fengqudb::QueryStats & stats() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows::Row` | `Row(const Rows *owner, const QueryRow *row)` | Build a query or read its results |
| `fengqudb::q::Rows::Row` | `template <class T> std::optional<T> get(const Agg<T> &a) const` | Read data or status |
| `fengqudb::q::Rows::Row` | `template <class T> std::optional<T> get(const Col<T> &c) const` | Read data or status |
| `fengqudb::q::Rows::Row` | `const std::string & key() const noexcept` | Build a query or read its results |
| `fengqudb::q::Rows::Row` | `template <class T> T operator[](const Agg<T> &a) const` | Build a query or read its results |
| `fengqudb::q::Rows::Row` | `template <class T> T operator[](const Col<T> &c) const` | Build a query or read its results |
| `fengqudb::q::Rows::iterator` | `iterator(const Rows *owner, size_t at)` | Build a query or read its results |
| `fengqudb::q::Rows::iterator` | `bool operator!=(const iterator &o) const` | Build a query or read its results |
| `fengqudb::q::Rows::iterator` | `fengqudb::q::Rows::Row operator*() const` | Build a query or read its results |
| `fengqudb::q::Rows::iterator` | `fengqudb::q::Rows::iterator & operator++()` | Build a query or read its results |
| `fengqudb::q::Rows::iterator` | `bool operator==(const iterator &o) const` | Build a query or read its results |

</details>

## 8. Indexes for frequent field queries

```cpp
db.define_index("student-score", "student/", Codec::Field_score, 5);
```

Field type 5 means a signed ZigZag integer. See the parameters in this section for other types and index options.

<details>
<summary>Functions and parameters (4)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo define_compound_index(std::string_view name, std::string_view prefix, const std::vector<StoreIndexField> &fields, uint32_t flags = 0, const StoreMaintenanceOptions &options = {})` | Define or use an index |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo define_index(std::string_view name, std::string_view prefix, uint32_t field, uint32_t type, uint32_t flags = 0, const StoreMaintenanceOptions &options = {})` | Define or use an index |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo drop_index(std::string_view name, const StoreMaintenanceOptions &options = {})` | Define or use an index |
| `fengqudb::Store` | `fengqudb::StoreCommandResult query_index(std::string_view name, const StoreIndexQuery &query = {}, std::string_view cursor = {}, uint64_t limit = 256)` | Define or use an index |

</details>

## 9. Reliable writes

The default performance mode uses 50 ms / 1 MiB and returns after applying the write in memory. Pass the final reliability flag for critical writes:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```cpp
db.put("payment/receipt", "paid", 0, true);
db.sync();
auto pending = db.submit_put("payment/next", "paid", 0, true);
auto result = pending.wait();
```

A UI thread can retain the receipt and poll later. On COMMIT_UNKNOWN, reconcile by business operation ID instead of blindly replaying a payment.

<details>
<summary>Functions and parameters (11)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `void configure_write(const StoreWriteOptions &options)` | Control durability or wait for a commit |
| `fengqudb::Store` | `void reset_write_options()` | Control durability or wait for a commit |
| `fengqudb::Store` | `fengqudb::StoreCommit submit_put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0, bool force_reliable = false)` | Control durability or wait for a commit |
| `fengqudb::Store` | `void sync()` | Control durability or wait for a commit |
| `fengqudb::Store` | `fengqudb::StoreWriteOptions write_options() const` | Control durability or wait for a commit |
| `fengqudb::Store` | `fengqudb::StoreWriteState write_state() const` | Control durability or wait for a commit |
| `fengqudb::StoreCommit` | `StoreCommit(StoreCommit &&) noexcept` | Commit a transaction or inspect its result |
| `fengqudb::StoreCommit` | `StoreCommit(fqdb_store_commit *handle)` | Commit a transaction or inspect its result |
| `fengqudb::StoreCommit` | `void cancel() const` | Control durability or wait for a commit |
| `fengqudb::StoreCommit` | `std::optional<CommitResult> poll() const` | Control durability or wait for a commit |
| `fengqudb::StoreCommit` | `fengqudb::CommitResult wait(uint64_t timeout_ms = 0) const` | Control durability or wait for a commit |

</details>

## 10. Automatic expiry

```cpp
db.put("verification", "1234", expires_at_unix_ms);
```

Time is an absolute Unix timestamp in milliseconds; 0 means no expiry.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreExpiryInfo purge_expired(const StoreExpiryOptions &options = {})` | Manage expiring data |
| `fengqudb::StoreValue` | `uint64_t expires_at_ms() const` | Manage expiring data |

</details>

## 11. Hot-path reads

```cpp
auto key = db.prepare_key("student/7");
auto value = key.get();
auto reader = db.prepare_reader({"student/7", "student/8"});
auto rows = reader.read();
```

Shared bytes are borrowed and read-only; keep their owner alive. Reader rows remain valid only until the next read/release. Copy them if they must live longer.

<details>
<summary>Functions and parameters (12)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StorePreparedKey prepare_key(std::string_view key) const` | Reuse keys, buffers, or shared bytes |
| `fengqudb::Store` | `fengqudb::StoreReader prepare_reader(const std::vector<std::string_view> &keys) const` | Reuse keys, buffers, or shared bytes |
| `fengqudb::Store` | `fengqudb::StoreReader prepare_reader(std::string_view key) const` | Reuse keys, buffers, or shared bytes |
| `fengqudb::StorePreparedKey` | `StorePreparedKey(StorePreparedKey &&) noexcept` | Reuse keys, buffers, or shared bytes |
| `fengqudb::StorePreparedKey` | `std::optional<StoreValue> get() const` | Read data or status |
| `fengqudb::StorePreparedKey` | `bool get_into(void *destination, uint64_t capacity, uint64_t &required) const` | Read data or status |
| `fengqudb::StorePreparedKey` | `fengqudb::StorePreparedKey & operator=(StorePreparedKey &&) noexcept` | Reuse keys, buffers, or shared bytes |
| `fengqudb::StoreReader` | `StoreReader(StoreReader &&) noexcept` | Reuse keys, buffers, or shared bytes |
| `fengqudb::StoreReader` | `fengqudb::StoreValues acquire()` | Reuse keys, buffers, or shared bytes |
| `fengqudb::StoreReader` | `fengqudb::StoreReader & operator=(StoreReader &&) noexcept` | Reuse keys, buffers, or shared bytes |
| `fengqudb::StoreReader` | `fengqudb::StoreReadView read()` | Read data or status |
| `fengqudb::StoreReader` | `void release()` | Release an object or borrowed resource |

</details>

## 12. Consistent snapshots

```cpp
auto snapshot = db.snapshot();
auto a = snapshot.get("a");
auto b = snapshot.get("b");
```

<details>
<summary>Functions and parameters (10)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreSnapshot snapshot() const` | Consistent reads or range iteration |
| `fengqudb::StoreCursor` | `StoreCursor(StoreCursor &&) noexcept` | Consistent reads or range iteration |
| `fengqudb::StoreCursor` | `std::optional<StoreRow> next()` | Consistent reads or range iteration |
| `fengqudb::StoreCursor` | `fengqudb::StoreCursor & operator=(StoreCursor &&) noexcept` | Consistent reads or range iteration |
| `fengqudb::StoreSnapshot` | `StoreSnapshot(StoreSnapshot &&) noexcept` | Consistent reads or range iteration |
| `fengqudb::StoreSnapshot` | `std::optional<StoreValue> get(const StorePreparedKey &key) const` | Read data or status |
| `fengqudb::StoreSnapshot` | `std::optional<StoreValue> get(std::string_view key) const` | Read data or status |
| `fengqudb::StoreSnapshot` | `fengqudb::StoreValues get_many(const std::vector<std::string_view> &keys) const` | Read data or status |
| `fengqudb::StoreSnapshot` | `fengqudb::StoreSnapshot & operator=(StoreSnapshot &&) noexcept` | Consistent reads or range iteration |
| `fengqudb::StoreSnapshot` | `fengqudb::StoreCursor scan(std::string_view begin = {}, std::string_view end = {}, bool reverse = false) const` | Consistent reads or range iteration |

</details>

## 13. Backup and restore

```cpp
db.backup("backups/full-001");
```

Use a new destination directory. Restore with restore/recover_chain after closing the database being replaced. Do not copy files while they are being written.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo backup(std::string_view destination, const StoreMaintenanceOptions &options = {})` | Backup, restore, or history retention |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo backup_incremental(std::string_view destination, uint64_t from, const StoreMaintenanceOptions &options = {})` | Backup, restore, or history retention |
| `fengqudb::Store` | `fengqudb::StoreHistoryInfo history_info() const` | Backup, restore, or history retention |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo hold_history(std::string_view name, uint64_t from_version, uint64_t timeout_ms = 0)` | Backup, restore, or history retention |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo recover_chain(std::string_view base, const std::vector<std::string> &archives, std::string_view destination, uint64_t target_version = 0, uint64_t target_unix_ms = 0, const StoreMaintenanceOptions &options = {})` | Backup, restore, or history retention |
| `fengqudb::Store` | `fengqudb::StoreAtomicInfo release_history(std::string_view name, uint64_t timeout_ms = 0)` | Release an object or borrowed resource |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo restore(std::string_view source, std::string_view destination, const StoreMaintenanceOptions &options = {})` | Backup, restore, or history retention |

</details>

## 14. Collection operations

execute accepts Hash/Set/List/sorted-set commands; hash_get reads a Hash field. Use them for atomic member changes, queues, and rankings.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreCommandResult execute(const StoreCommand &command, bool force_reliable = false)` | Native collection operation |
| `fengqudb::Store` | `std::optional<StoreValue> hash_get(std::string_view scope, std::string_view key) const` | Read data or status |

</details>

## 15. Cleanup and compaction

```cpp
db.purge_expired();
db.compact();
db.verify();
```

<details>
<summary>Functions and parameters (6)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `void checkpoint()` | Inspect status or maintain the database |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo compact(const StoreMaintenanceOptions &options = {})` | Inspect status or maintain the database |
| `fengqudb::Store` | `void configure(const StoreRuntimeOptions &options = {})` | Inspect status or maintain the database |
| `fengqudb::Store` | `fengqudb::StoreInfo info() const` | Inspect status or maintain the database |
| `fengqudb::Store` | `fengqudb::StoreRuntimeInfo runtime_info() const` | Inspect status or maintain the database |
| `fengqudb::Store` | `fengqudb::StoreMaintenanceInfo verify(const StoreMaintenanceOptions &options = {}) const` | Inspect status or maintain the database |

</details>

## 16. Consume changes

watch creates a change consumer; acknowledge its watermark after processing. Use it to update in-process caches and trigger background tasks. Handles use RAII.

<details>
<summary>Functions and parameters (12)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Store` | `fengqudb::StoreWatch watch(uint64_t from_version, std::string_view prefix = {}, const StoreWatchOptions &options = {})` | Consume changes or diagnose issues |
| `fengqudb::StoreChanges` | `StoreChanges(StoreChanges &&) noexcept` | Consume changes or diagnose issues |
| `fengqudb::StoreChanges` | `const fqdb_store_change_row * begin() const` | Consume changes or diagnose issues |
| `fengqudb::StoreChanges` | `const fqdb_store_change_row * end() const` | Consume changes or diagnose issues |
| `fengqudb::StoreChanges` | `const fqdb_store_change_batch_info & info() const` | Consume changes or diagnose issues |
| `fengqudb::StoreChanges` | `fengqudb::StoreChanges & operator=(StoreChanges &&) noexcept` | Consume changes or diagnose issues |
| `fengqudb::StoreWatch` | `StoreWatch(StoreWatch &&) noexcept` | Consume changes or diagnose issues |
| `fengqudb::StoreWatch` | `void ack(uint64_t version)` | Consume changes or diagnose issues |
| `fengqudb::StoreWatch` | `void cancel()` | Consume changes or diagnose issues |
| `fengqudb::StoreWatch` | `std::optional<StoreChanges> next(uint64_t timeout_ms = 0)` | Consume changes or diagnose issues |
| `fengqudb::StoreWatch` | `fengqudb::StoreWatch & operator=(StoreWatch &&) noexcept` | Consume changes or diagnose issues |
| `fengqudb::StoreWatchOptions` | `fqdb_store_watch_options native() const noexcept` | Consume changes or diagnose issues |

</details>

## 17. Entity encoding and decoding

Codec::encode/encode_into accesses your struct directly; decode returns the original struct and unknown fields. Use encode(row) when rewriting a record to preserve fields added by newer versions. Reuse a string or caller-owned buffer; output must not overlap input fields.

<details>
<summary>Functions and parameters (34)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::query_wire` | `std::string_view bytes(wire::Reader &r, wire::Field f)` | Encode or decode entities or bytes |
| `fengqudb::query_wire` | `uint64_t number(wire::Reader &r, wire::Field f)` | Encode or decode entities or bytes |
| `fengqudb::query_wire` | `template <class Database> fengqudb::StoreCommandResult run(Database &db, uint32_t operation, std::string_view request)` | Encode or decode entities or bytes |
| `fengqudb::query_wire` | `fengqudb::QueryValue value(std::string_view b)` | Encode or decode entities or bytes |
| `fengqudb::wire` | `size_t bytes_size(uint32_t field, size_t length) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire` | `int64_t unzigzag(uint64_t value) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire` | `size_t varint_size(uint64_t value) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire` | `uint64_t zigzag(int64_t value) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `Reader(std::string_view bytes, size_t limit = 67108864) noexcept` | Read data or status |
| `fengqudb::wire::Reader` | `bool bytes(std::string_view &value) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `bool done() const noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `bool fixed(uint64_t &value, unsigned count) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `bool good() const noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `bool next(Field &field) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `size_t position() const noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `bool skip(Field field) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Reader` | `bool varint(uint64_t &value) noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `Writer() noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `Writer(std::string storage)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void bytes_field(uint32_t field, std::string_view value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void clear() noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `const std::string & data() const noexcept` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void double_field(uint32_t field, double value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void fixed(uint64_t value, unsigned count)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void fixed32_field(uint32_t field, uint32_t value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void fixed64_field(uint32_t field, uint64_t value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void float_field(uint32_t field, float value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void message_header(uint32_t field, size_t length)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void reserve(size_t size)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void sint_field(uint32_t field, int64_t value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void tag(uint32_t field, Type type)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `std::string take()` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void uint_field(uint32_t field, uint64_t value)` | Encode or decode entities or bytes |
| `fengqudb::wire::Writer` | `void varint(uint64_t value)` | Encode or decode entities or bytes |

</details>

## Other utilities

<details>
<summary>Functions and parameters (35)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb::Buffer` | `Buffer() noexcept` | Utility operation |
| `fengqudb::Buffer` | `Buffer(Buffer &&other) noexcept` | Utility operation |
| `fengqudb::Buffer` | `Buffer(const Buffer &)` | Utility operation |
| `fengqudb::Buffer` | `const char * data() const noexcept` | Utility operation |
| `fengqudb::Buffer` | `bool empty() const noexcept` | Utility operation |
| `fengqudb::Buffer` | `size_t size() const noexcept` | Utility operation |
| `fengqudb::Buffer` | `std::string str() const` | Utility operation |
| `fengqudb::Buffer` | `std::string_view view() const noexcept` | Utility operation |
| `fengqudb::Buffer` | `~Buffer() noexcept` | Utility operation |
| `fengqudb::Error` | `Error(fqdb_status code)` | Utility operation |
| `fengqudb::Error` | `fqdb_status code() const noexcept` | Utility operation |
| `fengqudb::Error` | `fengqudb::Status status() const noexcept` | Utility operation |
| `fengqudb::Store` | `Store(Store &&) noexcept` | Utility operation |
| `fengqudb::Store` | `Store(std::string_view directory, const StoreOptions &options = {})` | Utility operation |
| `fengqudb::Store` | `void cancel_maintenance()` | Utility operation |
| `fengqudb::Store` | `fengqudb::Store open_at(const std::string &root, const std::string &name, const StoreOptions &options = {})` | Utility operation |
| `fengqudb::Store` | `std::string path_at(const std::string &root, const std::string &name)` | Utility operation |
| `fengqudb::Store` | `fengqudb::StoreReader prepare_project_reader(const std::vector<std::string_view> &keys, const std::vector<uint32_t> &fields) const` | Utility operation |
| `fengqudb::Store` | `std::string project(std::string_view key, const std::vector<uint32_t> &fields)` | Utility operation |
| `fengqudb::StoreCommand` | `std::string encode() const` | Utility operation |
| `fengqudb::StoreCommandResult` | `StoreCommandResult()` | Utility operation |
| `fengqudb::StoreCommandResult` | `StoreCommandResult(std::string_view bytes)` | Utility operation |
| `fengqudb::StoreExpiryOptions` | `fqdb_store_expiry_options native() const noexcept` | Utility operation |
| `fengqudb::StoreMaintenanceOptions` | `fqdb_store_maintenance_options native() const noexcept` | Utility operation |
| `fengqudb::StoreOptions` | `fqdb_store_options native() const noexcept` | Utility operation |
| `fengqudb::StoreReadView` | `StoreReadView(const fqdb_store_result_row *rows, uint32_t count)` | Utility operation |
| `fengqudb::StoreReadView` | `uint32_t size() const noexcept` | Utility operation |
| `fengqudb::StoreRuntimeOptions` | `fqdb_store_runtime_options native() const noexcept` | Utility operation |
| `fengqudb::StoreValue` | `StoreValue(StoreValue &&) noexcept` | Utility operation |
| `fengqudb::StoreValue` | `std::string string() const` | Utility operation |
| `fengqudb::StoreValue` | `uint64_t version() const` | Utility operation |
| `fengqudb::StoreValue` | `std::string_view view() const` | Utility operation |
| `fengqudb::StoreValues` | `StoreValues(StoreValues &&) noexcept` | Utility operation |
| `fengqudb::StoreValues` | `uint32_t size() const` | Utility operation |
| `fengqudb::StoreWriteOptions` | `fqdb_store_write_options native() const` | Utility operation |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>fengqudb</summary>

| Declaration |
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

| Declaration |
| --- |
| `fengqudb::Status status` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::Durability</summary>

| Declaration |
| --- |
| `fengqudb::Durability buffered = 1` |
| `fengqudb::Durability sync = 0` |

</details>

<details>
<summary>fengqudb::Operation</summary>

| Declaration |
| --- |
| `fengqudb::Operation erase = 2` |
| `fengqudb::Operation put = 1` |

</details>

<details>
<summary>fengqudb::QueryAccess</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `fengqudb::QueryAggregate aggregate` |
| `bool distinct = False` |
| `fengqudb::QueryField field` |
| `std::string name` |

</details>

<details>
<summary>fengqudb::QueryExpr</summary>

| Declaration |
| --- |
| `std::vector<QueryExpr> children` |
| `fengqudb::QueryField field` |
| `fengqudb::QueryOp op` |
| `std::optional<QueryField> right` |
| `std::vector<QueryValue> values` |

</details>

<details>
<summary>fengqudb::QueryField</summary>

| Declaration |
| --- |
| `std::vector<uint32_t> path` |
| `bool repeated = False` |
| `uint32_t source` |
| `uint32_t type` |

</details>

<details>
<summary>fengqudb::QueryJoin</summary>

| Declaration |
| --- |
| `fengqudb::QueryJoinKind kind` |
| `std::vector<QueryField> left` |
| `fengqudb::QueryExpr on` |
| `std::string prefix` |
| `std::vector<QueryField> right` |

</details>

<details>
<summary>fengqudb::QueryJoinKind</summary>

| Declaration |
| --- |
| `fengqudb::QueryJoinKind Anti` |
| `fengqudb::QueryJoinKind Inner` |
| `fengqudb::QueryJoinKind Left` |
| `fengqudb::QueryJoinKind Semi` |

</details>

<details>
<summary>fengqudb::QueryKind</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `bool descending = False` |
| `fengqudb::QueryField field` |
| `bool nulls_first = True` |

</details>

<details>
<summary>fengqudb::QueryRow</summary>

| Declaration |
| --- |
| `std::string key` |
| `std::vector<QueryValue> values` |

</details>

<details>
<summary>fengqudb::QueryStats</summary>

| Declaration |
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

| Declaration |
| --- |
| `std::string bytes` |
| `fengqudb::QueryKind kind` |
| `double real_value` |
| `int64_t signed_value` |
| `uint64_t unsigned_value` |

</details>

<details>
<summary>fengqudb::ReadResult</summary>

| Declaration |
| --- |
| `uint64_t required` |
| `fengqudb::Status status` |

</details>

<details>
<summary>fengqudb::Status</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `uint64_t max_batch_bytes` |
| `uint32_t max_keys` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fengqudb::StoreFeatureRow</summary>

| Declaration |
| --- |
| `std::string key` |
| `double score` |
| `std::string value` |

</details>

<details>
<summary>fengqudb::StoreI64FieldUpdate</summary>

| Declaration |
| --- |
| `int64_t delta` |
| `std::string_view key` |
| `int64_t maximum` |
| `int64_t minimum` |
| `uint64_t offset` |

</details>

<details>
<summary>fengqudb::StoreIndexQuery</summary>

| Declaration |
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

| Declaration |
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

| Declaration |
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

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `std::string_view key` |
| `std::string_view value` |

</details>

<details>
<summary>fengqudb::StoreQuery</summary>

| Declaration |
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

| Declaration |
| --- |
| `std::vector<QueryAccess> access` |
| `std::string cursor` |
| `std::vector<QueryRow> rows` |
| `fengqudb::QueryStats stats` |

</details>

<details>
<summary>fengqudb::StoreResult</summary>

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `std::optional<std::string_view> value` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::StoreRow</summary>

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `std::string_view key` |
| `std::string_view value` |
| `uint64_t version` |

</details>

<details>
<summary>fengqudb::StoreRuntimeOptions</summary>

| Declaration |
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

| Declaration |
| --- |
| `uint32_t flags` |
| `uint64_t max_batch_bytes` |
| `uint32_t max_scan_commits` |

</details>

<details>
<summary>fengqudb::StoreWriteMode</summary>

| Declaration |
| --- |
| `fengqudb::StoreWriteMode Performance = 1` |
| `fengqudb::StoreWriteMode Reliable = 0` |

</details>

<details>
<summary>fengqudb::StoreWriteOptions</summary>

| Declaration |
| --- |
| `fengqudb::StoreWriteMode mode` |
| `uint32_t period_ms` |
| `uint64_t threshold_bytes` |

</details>

<details>
<summary>fengqudb::q</summary>

| Declaration |
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

| Declaration |
| --- |
| `fengqudb::QueryColumn column` |

</details>

<details>
<summary>fengqudb::q::Col</summary>

| Declaration |
| --- |
| `fengqudb::QueryField field` |

</details>

<details>
<summary>fengqudb::q::Cond</summary>

| Declaration |
| --- |
| `fengqudb::QueryExpr expr` |
| `std::vector<QueryColumn> refs` |

</details>

<details>
<summary>fengqudb::q::Order</summary>

| Declaration |
| --- |
| `fengqudb::QueryOrder order` |
| `std::optional<QueryColumn> output` |

</details>

<details>
<summary>fengqudb::q::Rows</summary>

| Declaration |
| --- |
| `class Row` |
| `class iterator` |

</details>

<details>
<summary>fengqudb::wire</summary>

| Declaration |
| --- |
| `struct Field` |
| `class Reader` |
| `enum class Type` |
| `class Writer` |

</details>

<details>
<summary>fengqudb::wire::Field</summary>

| Declaration |
| --- |
| `uint32_t number` |
| `fengqudb::wire::Type type` |

</details>

<details>
<summary>fengqudb::wire::Type</summary>

| Declaration |
| --- |
| `fengqudb::wire::Type Bytes = 2` |
| `fengqudb::wire::Type EndGroup = 4` |
| `fengqudb::wire::Type Fixed32 = 5` |
| `fengqudb::wire::Type Fixed64 = 1` |
| `fengqudb::wire::Type StartGroup = 3` |
| `fengqudb::wire::Type Varint = 0` |

</details>
