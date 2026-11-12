#ifndef FENGQUDB_SELECT_HPP
#define FENGQUDB_SELECT_HPP
#include "fengqudb_store.hpp"
#include <cmath>
#include <type_traits>

namespace fengqudb {
enum class QueryKind : uint32_t { Null, UInt, Int, Real, Bytes, Bool };
struct QueryValue {
    QueryKind kind = QueryKind::Null;
    uint64_t unsigned_value = 0;
    int64_t signed_value = 0;
    double real_value = 0;
    std::string bytes;
    static QueryValue uint(uint64_t v) {
        QueryValue r;
        r.kind = QueryKind::UInt;
        r.unsigned_value = v;
        return r;
    }
    static QueryValue integer(int64_t v) {
        QueryValue r;
        r.kind = QueryKind::Int;
        r.signed_value = v;
        return r;
    }
    static QueryValue real(double v) {
        QueryValue r;
        r.kind = QueryKind::Real;
        r.real_value = v;
        return r;
    }
    static QueryValue text(std::string v) {
        QueryValue r;
        r.kind = QueryKind::Bytes;
        r.bytes = std::move(v);
        return r;
    }
    static QueryValue boolean(bool v) {
        QueryValue r;
        r.kind = QueryKind::Bool;
        r.unsigned_value = v;
        return r;
    }
    std::string encode() const {
        wire::Writer w;
        w.uint_field(1, uint32_t(kind));
        switch (kind) {
        case QueryKind::Null:
            break;
        case QueryKind::UInt:
        case QueryKind::Bool:
            w.uint_field(2, unsigned_value);
            break;
        case QueryKind::Int:
            w.sint_field(3, signed_value);
            break;
        case QueryKind::Real:
            if (!std::isfinite(real_value))
                throw std::invalid_argument("non-finite query number");
            w.double_field(4, real_value);
            break;
        case QueryKind::Bytes:
            w.bytes_field(5, bytes);
            break;
        }
        return w.take();
    }
};
// Physical types 1..10 match define_compound_index; 11 is protobuf bool.
// Empty path: type 4 is the record key; type 0 is its complete binary value.
struct QueryField {
    uint32_t source = 0, type = 4;
    std::vector<uint32_t> path;
    // true 表示 path 指向 protobuf repeated 字段（可经过 repeated 嵌套消息）：
    // 条件在任一元素满足时为真，只能用于主源 WHERE 的叶子条件，且不会使用磁盘索引。
    bool repeated = false;
    static QueryField key(uint32_t source = 0) {
        return {source, 4, {}};
    }
    static QueryField record(uint32_t source = 0) {
        return {source, 0, {}};
    }
    static QueryField column(uint32_t n) {
        return {255, 0, {n + 1}};
    }
    static QueryField field(uint32_t number, uint32_t type, uint32_t source = 0) {
        return {source, type, {number}};
    }
    // 返回同一路径的 repeated 版本，例如 F::field(8, 1).any() 或带嵌套路径的字段。
    QueryField any() const {
        auto copy = *this;
        copy.repeated = true;
        return copy;
    }
    bool operator==(const QueryField &o) const {
        return source == o.source && type == o.type && path == o.path && repeated == o.repeated;
    }
    bool operator!=(const QueryField &o) const {
        return !(*this == o);
    }
    std::string encode() const {
        wire::Writer w;
        w.uint_field(1, source);
        w.uint_field(2, type);
        for (auto n : path)
            w.uint_field(3, n);
        if (repeated)
            w.uint_field(4, 1);
        return w.take();
    }
};
enum class QueryOp : uint32_t {
    True,
    False,
    And,
    Or,
    Not,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    In,
    Exists,
    IsNull,
    Contains,
    StartsWith,
    EndsWith,
    Like,
    Regex,
    IContains,
    ILike,
    IRegex,
    Fuzzy
};
struct QueryExpr {
    QueryOp op = QueryOp::True;
    QueryField field;
    std::vector<QueryValue> values;
    std::vector<QueryExpr> children;
    std::optional<QueryField> right;
    static QueryExpr all(std::vector<QueryExpr> c) {
        QueryExpr e;
        e.op = QueryOp::And;
        e.children = std::move(c);
        return e;
    }
    static QueryExpr any(std::vector<QueryExpr> c) {
        QueryExpr e;
        e.op = QueryOp::Or;
        e.children = std::move(c);
        return e;
    }
    static QueryExpr negate(QueryExpr c) {
        QueryExpr e;
        e.op = QueryOp::Not;
        e.children.push_back(std::move(c));
        return e;
    }
    static QueryExpr compare(QueryField f, QueryOp op, QueryValue value) {
        QueryExpr e;
        e.field = std::move(f);
        e.op = op;
        e.values.push_back(std::move(value));
        return e;
    }
    static QueryExpr compare_fields(QueryField f, QueryOp op, QueryField right) {
        QueryExpr e;
        e.field = std::move(f);
        e.op = op;
        e.right = std::move(right);
        return e;
    }
    static QueryExpr one_of(QueryField f, std::vector<QueryValue> values) {
        QueryExpr e;
        e.field = std::move(f);
        e.op = QueryOp::In;
        e.values = std::move(values);
        return e;
    }
    static QueryExpr test(QueryField f, QueryOp op) {
        QueryExpr e;
        e.field = std::move(f);
        e.op = op;
        return e;
    }
    std::string encode(unsigned depth = 0) const {
        if (depth > 64)
            throw std::invalid_argument("query expression too deep");
        wire::Writer w;
        w.uint_field(1, uint32_t(op));
        if (uint32_t(op) >= 5)
            w.bytes_field(2, field.encode());
        for (const auto &v : values)
            w.bytes_field(3, v.encode());
        for (const auto &c : children)
            w.bytes_field(4, c.encode(depth + 1));
        if (right)
            w.bytes_field(5, right->encode());
        return w.take();
    }
};
enum class QueryAggregate : uint32_t { None, CountAll, Count, Sum, Min, Max, Avg };
struct QueryColumn {
    QueryField field;
    QueryAggregate aggregate = QueryAggregate::None;
    std::string name;
    bool distinct = false;
    std::string encode() const {
        wire::Writer w;
        w.bytes_field(1, field.encode());
        w.uint_field(2, uint32_t(aggregate));
        w.bytes_field(3, name);
        w.uint_field(4, distinct);
        return w.take();
    }
};
struct QueryOrder {
    QueryField field;
    bool descending = false, nulls_first = true;
    std::string encode() const {
        wire::Writer w;
        w.bytes_field(1, field.encode());
        w.uint_field(2, descending);
        w.uint_field(3, nulls_first);
        return w.take();
    }
};
enum class QueryJoinKind : uint32_t { Inner, Left, Semi, Anti };
struct QueryJoin {
    std::string prefix;
    std::vector<QueryField> left, right;
    QueryJoinKind kind = QueryJoinKind::Inner;
    QueryExpr on;
    std::string encode() const {
        wire::Writer w;
        w.bytes_field(1, prefix);
        for (auto &f : left)
            w.bytes_field(2, f.encode());
        for (auto &f : right)
            w.bytes_field(3, f.encode());
        w.uint_field(4, uint32_t(kind));
        w.bytes_field(5, on.encode());
        return w.take();
    }
};
struct StoreQuery {
    std::string prefix;
    QueryExpr where, having;
    std::vector<QueryColumn> columns;
    std::vector<QueryOrder> order;
    std::vector<QueryField> group;
    std::vector<QueryJoin> joins;
    uint64_t limit = 100, offset = 0, memory_bytes = 16ull << 20, max_candidates = 1000000,
             max_work = 100000000, timeout_ms = 1000;
    bool allow_scan = true, count_total = false, distinct = false;
    std::string index_hint, after;
    std::string encode() const {
        wire::Writer w;
        w.bytes_field(1, prefix);
        w.bytes_field(2, where.encode());
        for (auto &c : columns)
            w.bytes_field(3, c.encode());
        for (auto &o : order)
            w.bytes_field(4, o.encode());
        for (auto &f : group)
            w.bytes_field(5, f.encode());
        w.bytes_field(6, having.encode());
        for (auto &j : joins)
            w.bytes_field(7, j.encode());
        w.uint_field(8, limit);
        w.uint_field(9, offset);
        w.uint_field(10, memory_bytes);
        w.uint_field(11, max_candidates);
        w.uint_field(12, max_work);
        w.uint_field(13, timeout_ms);
        w.uint_field(14, allow_scan);
        w.uint_field(15, count_total);
        w.uint_field(16, distinct);
        w.bytes_field(17, index_hint);
        w.bytes_field(18, after);
        return w.take();
    }
};
struct QueryRow {
    std::string key;
    std::vector<QueryValue> values;
};
struct QueryAccess {
    uint32_t source = 0;
    std::string strategy, index, reason;
    bool covering = false, ordered = false, needs_scan = false;
    uint64_t sampled_candidates = 0;
};
struct QueryStats {
    uint64_t candidates = 0, matched = 0, result_total = 0, work = 0, peak_memory = 0,
             payload_reads = 0, join_probes = 0;
    bool complete = true;
};
namespace query_wire {
inline uint64_t number(wire::Reader &r, wire::Field f) {
    uint64_t v = 0;
    if (f.type != wire::Type::Varint || !r.varint(v))
        throw Error(FQDB_CORRUPT);
    return v;
}
inline std::string_view bytes(wire::Reader &r, wire::Field f) {
    std::string_view v;
    if (f.type != wire::Type::Bytes || !r.bytes(v))
        throw Error(FQDB_CORRUPT);
    return v;
}
inline QueryValue value(std::string_view b) {
    QueryValue v;
    wire::Reader r(b);
    wire::Field f;
    while (r.next(f)) {
        if (f.number == 1)
            v.kind = QueryKind(number(r, f));
        else if (f.number == 2)
            v.unsigned_value = number(r, f);
        else if (f.number == 3)
            v.signed_value = wire::unzigzag(number(r, f));
        else if (f.number == 4) {
            uint64_t n = 0;
            if (f.type != wire::Type::Fixed64 || !r.fixed(n, 8))
                throw Error(FQDB_CORRUPT);
            std::memcpy(&v.real_value, &n, 8);
        } else if (f.number == 5)
            v.bytes = std::string(bytes(r, f));
        else if (!r.skip(f))
            throw Error(FQDB_CORRUPT);
    }
    if (!r.done() || uint32_t(v.kind) > 5 ||
        (v.kind == QueryKind::Real && !std::isfinite(v.real_value)))
        throw Error(FQDB_CORRUPT);
    return v;
}
} // namespace query_wire
struct StoreQueryResult {
    std::vector<QueryRow> rows;
    std::vector<QueryAccess> access;
    QueryStats stats;
    std::string cursor;
    explicit StoreQueryResult(std::string_view bytes) {
        wire::Reader r(bytes);
        wire::Field f;
        while (r.next(f)) {
            if (f.number == 1) {
                QueryRow row;
                wire::Reader a(query_wire::bytes(r, f));
                wire::Field p;
                while (a.next(p)) {
                    if (p.number == 1)
                        row.key = std::string(query_wire::bytes(a, p));
                    else if (p.number == 2)
                        row.values.push_back(query_wire::value(query_wire::bytes(a, p)));
                    else if (!a.skip(p))
                        throw Error(FQDB_CORRUPT);
                }
                if (!a.done())
                    throw Error(FQDB_CORRUPT);
                rows.push_back(std::move(row));
            } else if (f.number == 2) {
                QueryAccess x;
                wire::Reader a(query_wire::bytes(r, f));
                wire::Field p;
                while (a.next(p)) {
                    if (p.number >= 2 && p.number <= 4) {
                        auto v = query_wire::bytes(a, p);
                        (p.number == 2 ? x.strategy : p.number == 3 ? x.index : x.reason).assign(v);
                    } else if (p.number == 1)
                        x.source = uint32_t(query_wire::number(a, p));
                    else if (p.number == 5)
                        x.covering = query_wire::number(a, p) != 0;
                    else if (p.number == 6)
                        x.ordered = query_wire::number(a, p) != 0;
                    else if (p.number == 7)
                        x.needs_scan = query_wire::number(a, p) != 0;
                    else if (p.number == 8)
                        x.sampled_candidates = query_wire::number(a, p);
                    else if (!a.skip(p))
                        throw Error(FQDB_CORRUPT);
                }
                if (!a.done())
                    throw Error(FQDB_CORRUPT);
                access.push_back(std::move(x));
            } else if (f.number == 3) {
                wire::Reader a(query_wire::bytes(r, f));
                wire::Field p;
                while (a.next(p)) {
                    if (p.number >= 1 && p.number <= 8) {
                        auto v = query_wire::number(a, p);
                        switch (p.number) {
                        case 1:
                            stats.candidates = v;
                            break;
                        case 2:
                            stats.matched = v;
                            break;
                        case 3:
                            stats.result_total = v;
                            break;
                        case 4:
                            stats.work = v;
                            break;
                        case 5:
                            stats.peak_memory = v;
                            break;
                        case 6:
                            stats.payload_reads = v;
                            break;
                        case 7:
                            stats.join_probes = v;
                            break;
                        case 8:
                            stats.complete = v != 0;
                            break;
                        }
                    } else if (!a.skip(p))
                        throw Error(FQDB_CORRUPT);
                }
                if (!a.done())
                    throw Error(FQDB_CORRUPT);
            } else if (f.number == 4)
                cursor = std::string(query_wire::bytes(r, f));
            else if (!r.skip(f))
                throw Error(FQDB_CORRUPT);
        }
        if (!r.done())
            throw Error(FQDB_CORRUPT);
    }
};
// Both Store and StoreTransaction expose execute(). Keep the core C ABI shared
// with every SDK; no C++ objects cross the library boundary.
namespace query_wire {
// Stores (including subclasses) use the read-only snapshot entry; transactions keep the
// original path so queries see private writes and join conflict checks.
template <class Database>
StoreCommandResult run(Database &db, uint32_t operation, std::string_view request) {
    StoreCommand command{operation, {}, {}, request};
    if constexpr (std::is_base_of<Store, Database>::value)
        return db.select_command(command);
    else
        return db.execute(command);
}
} // namespace query_wire
class StorePreparedQuery {
    std::string encoded_;

  public:
    explicit StorePreparedQuery(const StoreQuery &q) : encoded_(q.encode()) {}
    template <class Database>
    StoreQueryResult execute(Database &db, bool explain_only = false) const {
        auto result =
            query_wire::run(db, explain_only ? FQDB_SELECT_EXPLAIN : FQDB_SELECT, encoded_);
        if (!result.value)
            throw Error(FQDB_CORRUPT);
        return StoreQueryResult(*result.value);
    }
};
template <class Database> StoreQueryResult select(Database &db, const StoreQuery &q) {
    auto request = q.encode();
    auto result = query_wire::run(db, FQDB_SELECT, request);
    if (!result.value)
        throw Error(FQDB_CORRUPT);
    return StoreQueryResult(*result.value);
}
template <class Database> StoreQueryResult explain(Database &db, const StoreQuery &q) {
    auto request = q.encode();
    auto result = query_wire::run(db, FQDB_SELECT_EXPLAIN, request);
    if (!result.value)
        throw Error(FQDB_CORRUPT);
    return StoreQueryResult(*result.value);
}
} // namespace fengqudb
#endif
