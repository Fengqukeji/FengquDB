#ifndef FENGQUDB_QUERY_HPP
#define FENGQUDB_QUERY_HPP
// Typed, concise C++ query builder over StoreQuery. Declare each protobuf field
// once with its physical type, then write conditions with ordinary operators:
//
//   namespace q = fengqudb::q;
//   auto id = q::u64(1), level = q::u64(3);
//   auto cls = q::text(2);
//   auto gold = q::s64(4);
//   auto top = q::from("p/").where(cls == "mage" && level.between(20, 60) && gold >= 1000)
//                  .select(id, gold).order_by(gold.desc(), id).limit(20);
//   for (auto row : q::run(db, top))
//       use(row[id], row[gold]);  // uint64_t, int64_t
//
// The builder only produces the existing request encoding; execution, budgets
// and semantics are those documented in docs/STORE_SELECT.md. A Store runs on a
// read-only snapshot; a StoreTransaction sees its private writes.
// Comments in public headers stay ASCII: consumers may compile without /utf-8.
#include "fengqudb_select.hpp"
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace fengqudb::q {
template <class T> class Col;
template <class T> class Agg;

// A boolean condition. `refs` lists aggregates referenced by HAVING conditions;
// they are resolved to output-column references when the query is built.
struct Cond {
    QueryExpr expr;
    std::vector<QueryColumn> refs;
};
struct Order {
    QueryOrder order;
    std::optional<QueryColumn> output; // set when ordering by an aggregate
};

namespace detail {
constexpr uint32_t pending_source = 254;
template <class T> constexpr bool supported() {
    return std::is_same<T, uint64_t>::value || std::is_same<T, int64_t>::value ||
           std::is_same<T, double>::value || std::is_same<T, bool>::value ||
           std::is_same<T, std::string>::value;
}
template <class T> QueryValue literal(const T &v) {
    if constexpr (std::is_same<T, std::string>::value)
        return QueryValue::text(v);
    else if constexpr (std::is_same<T, bool>::value)
        return QueryValue::boolean(v);
    else if constexpr (std::is_same<T, uint64_t>::value)
        return QueryValue::uint(v);
    else if constexpr (std::is_same<T, int64_t>::value)
        return QueryValue::integer(v);
    else
        return QueryValue::real(v);
}
// Renumber pending aggregate references when two conditions are merged.
inline void shift(QueryExpr &e, size_t by) {
    if (!by)
        return;
    auto fix = [&](QueryField &f) {
        if (f.source == pending_source)
            f.path[0] += static_cast<uint32_t>(by);
    };
    fix(e.field);
    if (e.right)
        fix(*e.right);
    for (auto &c : e.children)
        shift(c, by);
}
inline Cond combine(QueryOp op, Cond a, Cond b) {
    shift(b.expr, a.refs.size());
    a.refs.insert(a.refs.end(), b.refs.begin(), b.refs.end());
    QueryExpr out;
    if (a.expr.op == op)
        out = std::move(a.expr);
    else {
        out.op = op;
        out.children.push_back(std::move(a.expr));
    }
    if (b.expr.op == op)
        for (auto &c : b.expr.children)
            out.children.push_back(std::move(c));
    else
        out.children.push_back(std::move(b.expr));
    return {std::move(out), std::move(a.refs)};
}
inline bool same(const QueryColumn &a, const QueryColumn &b) {
    return a.field == b.field && a.aggregate == b.aggregate && a.distinct == b.distinct;
}
template <class T> std::optional<T> convert(const QueryValue &v) {
    if (v.kind == QueryKind::Null)
        return std::nullopt;
    if constexpr (std::is_same<T, std::string>::value) {
        if (v.kind == QueryKind::Bytes)
            return v.bytes;
    } else if constexpr (std::is_same<T, bool>::value) {
        if (v.kind == QueryKind::Bool || v.kind == QueryKind::UInt)
            return v.unsigned_value != 0;
    } else if constexpr (std::is_same<T, uint64_t>::value) {
        if (v.kind == QueryKind::UInt || v.kind == QueryKind::Bool)
            return v.unsigned_value;
        if (v.kind == QueryKind::Int && v.signed_value >= 0)
            return static_cast<uint64_t>(v.signed_value);
    } else if constexpr (std::is_same<T, int64_t>::value) {
        if (v.kind == QueryKind::Int)
            return v.signed_value;
        if (v.kind == QueryKind::UInt && v.unsigned_value <= uint64_t(INT64_MAX))
            return static_cast<int64_t>(v.unsigned_value);
    } else {
        if (v.kind == QueryKind::Real)
            return v.real_value;
        if (v.kind == QueryKind::Int)
            return static_cast<double>(v.signed_value);
        if (v.kind == QueryKind::UInt)
            return static_cast<double>(v.unsigned_value);
    }
    throw Error(FQDB_CONSTRAINT); // result type differs from the declared column type
}
} // namespace detail

inline Cond operator&&(Cond a, Cond b) {
    return detail::combine(QueryOp::And, std::move(a), std::move(b));
}
inline Cond operator||(Cond a, Cond b) {
    return detail::combine(QueryOp::Or, std::move(a), std::move(b));
}
inline Cond operator!(Cond a) {
    return {QueryExpr::negate(std::move(a.expr)), std::move(a.refs)};
}

// A typed field of one source (0 = main prefix, 1..7 = joins in order).
template <class T> class Col {
    static_assert(detail::supported<T>(), "column type must be uint64_t, int64_t, double, bool "
                                          "or std::string");
    Cond test(QueryOp op, const T &v) const {
        return {QueryExpr::compare(field, op, detail::literal(v)), {}};
    }
    Cond test(QueryOp op, const Col &o) const {
        return {QueryExpr::compare_fields(field, op, o.field), {}};
    }
    Cond text_test(QueryOp op, const std::string &pattern) const {
        static_assert(std::is_same<T, std::string>::value, "string matching needs a text column");
        return {QueryExpr::compare(field, op, QueryValue::text(pattern)), {}};
    }

  public:
    QueryField field;
    Col(uint32_t number, uint32_t type, uint32_t source = 0)
        : field(QueryField::field(number, type, source)) {}
    explicit Col(QueryField f) : field(std::move(f)) {}
    // 同一路径的 repeated 版本：条件在任一元素满足时为真（只能用于主源 WHERE 叶子条件）。
    Col any() const { return Col(field.any()); }
    Cond operator==(const T &v) const { return test(QueryOp::Eq, v); }
    Cond operator!=(const T &v) const { return test(QueryOp::Ne, v); }
    Cond operator<(const T &v) const { return test(QueryOp::Lt, v); }
    Cond operator<=(const T &v) const { return test(QueryOp::Le, v); }
    Cond operator>(const T &v) const { return test(QueryOp::Gt, v); }
    Cond operator>=(const T &v) const { return test(QueryOp::Ge, v); }
    Cond operator==(const Col &o) const { return test(QueryOp::Eq, o); }
    Cond operator!=(const Col &o) const { return test(QueryOp::Ne, o); }
    Cond operator<(const Col &o) const { return test(QueryOp::Lt, o); }
    Cond operator<=(const Col &o) const { return test(QueryOp::Le, o); }
    Cond operator>(const Col &o) const { return test(QueryOp::Gt, o); }
    Cond operator>=(const Col &o) const { return test(QueryOp::Ge, o); }
    Cond between(const T &low, const T &high) const { return *this >= low && *this <= high; }
    Cond in(std::initializer_list<T> values) const { return in(std::vector<T>(values)); }
    Cond in(const std::vector<T> &values) const {
        std::vector<QueryValue> encoded;
        encoded.reserve(values.size());
        for (const auto &v : values)
            encoded.push_back(detail::literal(v));
        return {QueryExpr::one_of(field, std::move(encoded)), {}};
    }
    Cond is_null() const { return {QueryExpr::test(field, QueryOp::IsNull), {}}; }
    Cond exists() const { return {QueryExpr::test(field, QueryOp::Exists), {}}; }
    Cond contains(const std::string &s) const { return text_test(QueryOp::Contains, s); }
    Cond starts_with(const std::string &s) const { return text_test(QueryOp::StartsWith, s); }
    Cond ends_with(const std::string &s) const { return text_test(QueryOp::EndsWith, s); }
    Cond like(const std::string &s) const { return text_test(QueryOp::Like, s); }
    Cond regex(const std::string &s) const { return text_test(QueryOp::Regex, s); }
    Cond icontains(const std::string &s) const { return text_test(QueryOp::IContains, s); }
    Cond ilike(const std::string &s) const { return text_test(QueryOp::ILike, s); }
    // Whole-field UTF-8 edit distance, 0..8.
    Cond fuzzy(const std::string &s, uint64_t distance) const {
        static_assert(std::is_same<T, std::string>::value, "fuzzy matching needs a text column");
        QueryExpr e = QueryExpr::compare(field, QueryOp::Fuzzy, QueryValue::text(s));
        e.values.push_back(QueryValue::uint(distance));
        return {std::move(e), {}};
    }
    Order asc(bool nulls_first = true) const { return {{field, false, nulls_first}, {}}; }
    Order desc(bool nulls_first = true) const { return {{field, true, nulls_first}, {}}; }
};

// Field factories: the name is the protobuf wire type, the value is the C++ type.
inline Col<uint64_t> u64(uint32_t n, uint32_t source = 0) { return {n, 1, source}; }
inline Col<int64_t> i64(uint32_t n, uint32_t source = 0) { return {n, 2, source}; }
inline Col<double> f64(uint32_t n, uint32_t source = 0) { return {n, 3, source}; }
inline Col<std::string> text(uint32_t n, uint32_t source = 0) { return {n, 4, source}; }
inline Col<int64_t> s64(uint32_t n, uint32_t source = 0) { return {n, 5, source}; }
inline Col<double> f32(uint32_t n, uint32_t source = 0) { return {n, 6, source}; }
inline Col<uint64_t> fixed64(uint32_t n, uint32_t source = 0) { return {n, 7, source}; }
inline Col<int64_t> sfixed64(uint32_t n, uint32_t source = 0) { return {n, 8, source}; }
inline Col<uint64_t> fixed32(uint32_t n, uint32_t source = 0) { return {n, 9, source}; }
inline Col<int64_t> sfixed32(uint32_t n, uint32_t source = 0) { return {n, 10, source}; }
inline Col<bool> boolean(uint32_t n, uint32_t source = 0) { return {n, 11, source}; }
// The record key of a source; a nested field path such as {8, 1} is also allowed.
inline Col<std::string> key(uint32_t source = 0) { return Col<std::string>(QueryField::key(source)); }
template <class T> Col<T> nested(std::vector<uint32_t> path, uint32_t type, uint32_t source = 0) {
    return Col<T>(QueryField{source, type, std::move(path)});
}

// An aggregate output column. Comparisons are valid only inside having().
template <class T> class Agg {
    Cond test(QueryOp op, const T &v) const {
        QueryField pending{detail::pending_source, 0, {0}};
        return {QueryExpr::compare(pending, op, detail::literal(v)), {column}};
    }

  public:
    QueryColumn column;
    Cond operator==(const T &v) const { return test(QueryOp::Eq, v); }
    Cond operator!=(const T &v) const { return test(QueryOp::Ne, v); }
    Cond operator<(const T &v) const { return test(QueryOp::Lt, v); }
    Cond operator<=(const T &v) const { return test(QueryOp::Le, v); }
    Cond operator>(const T &v) const { return test(QueryOp::Gt, v); }
    Cond operator>=(const T &v) const { return test(QueryOp::Ge, v); }
    Order asc(bool nulls_first = true) const { return {{{}, false, nulls_first}, column}; }
    Order desc(bool nulls_first = true) const { return {{{}, true, nulls_first}, column}; }
};
namespace detail {
template <class R, class T> Agg<R> aggregate(const Col<T> &c, QueryAggregate a, bool distinct) {
    Agg<R> out;
    out.column.field = c.field;
    out.column.aggregate = a;
    out.column.distinct = distinct;
    return out;
}
} // namespace detail
inline Agg<uint64_t> count() {
    Agg<uint64_t> out;
    out.column.field = QueryField::key();
    out.column.aggregate = QueryAggregate::CountAll;
    return out;
}
template <class T> Agg<uint64_t> count(const Col<T> &c) {
    return detail::aggregate<uint64_t>(c, QueryAggregate::Count, false);
}
template <class T> Agg<uint64_t> count_distinct(const Col<T> &c) {
    return detail::aggregate<uint64_t>(c, QueryAggregate::Count, true);
}
template <class T> Agg<T> sum(const Col<T> &c) {
    return detail::aggregate<T>(c, QueryAggregate::Sum, false);
}
template <class T> Agg<double> avg(const Col<T> &c) {
    return detail::aggregate<double>(c, QueryAggregate::Avg, false);
}
template <class T> Agg<T> min(const Col<T> &c) {
    return detail::aggregate<T>(c, QueryAggregate::Min, false);
}
template <class T> Agg<T> max(const Col<T> &c) {
    return detail::aggregate<T>(c, QueryAggregate::Max, false);
}

class Rows;
class Prepared;
class Query {
    StoreQuery q_;
    std::vector<Order> order_;
    Cond having_;
    template <class T> static QueryColumn column_of(const Col<T> &c) {
        QueryColumn out;
        out.field = c.field;
        return out;
    }
    template <class T> static QueryColumn column_of(const Agg<T> &a) { return a.column; }
    static Order order_of(const Order &o) { return o; }
    template <class T> static Order order_of(const Col<T> &c) { return c.asc(); }
    uint32_t output_index(const QueryColumn &c) const {
        for (size_t i = 0; i < q_.columns.size(); ++i)
            if (detail::same(q_.columns[i], c))
                return static_cast<uint32_t>(i);
        throw Error(FQDB_INVALID_ARGUMENT); // order/having aggregate must also be selected
    }
    void resolve(QueryExpr &e, const std::vector<QueryColumn> &refs) const {
        auto fix = [&](QueryField &f) {
            if (f.source == detail::pending_source)
                f = QueryField::column(output_index(refs.at(f.path[0])));
        };
        fix(e.field);
        if (e.right)
            fix(*e.right);
        for (auto &c : e.children)
            resolve(c, refs);
    }

  public:
    explicit Query(std::string prefix) { q_.prefix = std::move(prefix); }
    Query &where(Cond c) {
        if (!c.refs.empty())
            throw Error(FQDB_INVALID_ARGUMENT); // aggregates belong in having()
        q_.where = std::move(c.expr);
        return *this;
    }
    template <class... Items> Query &select(const Items &...items) {
        q_.columns = {column_of(items)...};
        return *this;
    }
    template <class... T> Query &group_by(const Col<T> &...cols) {
        q_.group = {cols.field...};
        return *this;
    }
    Query &having(Cond c) {
        having_ = std::move(c);
        return *this;
    }
    template <class... Items> Query &order_by(const Items &...items) {
        order_ = {order_of(items)...};
        return *this;
    }
    // Equality join on one key; the right column must use source = join number.
    template <class T>
    Query &join(std::string prefix, const Col<T> &left, const Col<T> &right,
                QueryJoinKind kind = QueryJoinKind::Inner) {
        QueryJoin j;
        j.prefix = std::move(prefix);
        j.left = {left.field};
        j.right = {right.field};
        j.kind = kind;
        q_.joins.push_back(std::move(j));
        return *this;
    }
    Query &join(QueryJoin j) {
        q_.joins.push_back(std::move(j));
        return *this;
    }
    Query &limit(uint64_t n) {
        q_.limit = n;
        return *this;
    }
    Query &offset(uint64_t n) {
        q_.offset = n;
        return *this;
    }
    // Continue after the cursor returned by Rows::cursor().
    Query &after(std::string cursor) {
        q_.after = std::move(cursor);
        return *this;
    }
    Query &distinct(bool on = true) {
        q_.distinct = on;
        return *this;
    }
    Query &count_total(bool on = true) {
        q_.count_total = on;
        return *this;
    }
    Query &allow_scan(bool on) {
        q_.allow_scan = on;
        return *this;
    }
    Query &index(std::string name) {
        q_.index_hint = std::move(name);
        return *this;
    }
    Query &memory(uint64_t bytes) {
        q_.memory_bytes = bytes;
        return *this;
    }
    Query &timeout_ms(uint64_t ms) {
        q_.timeout_ms = ms;
        return *this;
    }
    Query &max_candidates(uint64_t n) {
        q_.max_candidates = n;
        return *this;
    }
    // The equivalent low-level request.
    StoreQuery build() const {
        StoreQuery out = q_;
        out.having = having_.expr;
        resolve(out.having, having_.refs);
        out.order.clear();
        for (const auto &o : order_) {
            auto item = o.order;
            if (o.output)
                item.field = QueryField::column(output_index(*o.output));
            out.order.push_back(std::move(item));
        }
        return out;
    }
    const std::vector<QueryColumn> &columns() const noexcept { return q_.columns; }
    Prepared prepare() const;
};
inline Query from(std::string prefix) { return Query(std::move(prefix)); }

// Result rows with typed access by the same Col/Agg objects used to select them.
class Rows {
    std::vector<QueryColumn> columns_;
    StoreQueryResult result_;
    size_t index(const QueryColumn &c) const {
        for (size_t i = 0; i < columns_.size(); ++i)
            if (detail::same(columns_[i], c))
                return i;
        throw Error(FQDB_INVALID_ARGUMENT); // column was not selected
    }
    template <class T> static QueryColumn plain(const Col<T> &c) {
        QueryColumn out;
        out.field = c.field;
        return out;
    }

  public:
    Rows(std::vector<QueryColumn> columns, StoreQueryResult result)
        : columns_(std::move(columns)), result_(std::move(result)) {}
    class Row {
        const Rows *owner_;
        const QueryRow *row_;

      public:
        Row(const Rows *owner, const QueryRow *row) : owner_(owner), row_(row) {}
        // std::nullopt for a missing field (SQL NULL).
        template <class T> std::optional<T> get(const Col<T> &c) const {
            return detail::convert<T>(row_->values.at(owner_->index(plain(c))));
        }
        template <class T> std::optional<T> get(const Agg<T> &a) const {
            return detail::convert<T>(row_->values.at(owner_->index(a.column)));
        }
        // Throws Error(NOT_FOUND) for NULL; use get() when the field is optional.
        template <class T> T operator[](const Col<T> &c) const { return require(get(c)); }
        template <class T> T operator[](const Agg<T> &a) const { return require(get(a)); }
        const std::string &key() const noexcept { return row_->key; }

      private:
        template <class T> static T require(std::optional<T> v) {
            if (!v)
                throw Error(FQDB_NOT_FOUND);
            return std::move(*v);
        }
    };
    class iterator {
        const Rows *owner_;
        size_t at_;

      public:
        iterator(const Rows *owner, size_t at) : owner_(owner), at_(at) {}
        Row operator*() const { return {owner_, &owner_->result_.rows[at_]}; }
        iterator &operator++() {
            ++at_;
            return *this;
        }
        bool operator!=(const iterator &o) const { return at_ != o.at_; }
        bool operator==(const iterator &o) const { return at_ == o.at_; }
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, result_.rows.size()}; }
    Row operator[](size_t i) const { return {this, &result_.rows.at(i)}; }
    size_t size() const noexcept { return result_.rows.size(); }
    bool empty() const noexcept { return result_.rows.empty(); }
    // Non-empty when another page exists; pass it to Query::after().
    const std::string &cursor() const noexcept { return result_.cursor; }
    const QueryStats &stats() const noexcept { return result_.stats; }
    const std::vector<QueryAccess> &access() const noexcept { return result_.access; }
    const StoreQueryResult &raw() const noexcept { return result_; }
};

// Encodes the request once; reuse it for repeated execution of the same query.
class Prepared {
    StorePreparedQuery prepared_;
    std::vector<QueryColumn> columns_;

  public:
    explicit Prepared(const Query &q) : prepared_(q.build()), columns_(q.columns()) {}
    template <class Database> Rows run(Database &db) const {
        return Rows(columns_, prepared_.execute(db));
    }
};
inline Prepared Query::prepare() const { return Prepared(*this); }

template <class Database> Rows run(Database &db, const Query &q) {
    return Rows(q.columns(), select(db, q.build()));
}
template <class Database> StoreQueryResult explain(Database &db, const Query &q) {
    return fengqudb::explain(db, q.build());
}
} // namespace fengqudb::q
#endif
