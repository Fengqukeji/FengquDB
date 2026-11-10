#pragma once
#include "query.hpp"
#include "fengqudb_select.hpp"
#include <unordered_map>
#include <unordered_set>

namespace fengqudb::paged::select {
using Kind = QueryKind;
using Op = QueryOp;
using Field = QueryField;
using Expr = QueryExpr;
using Scalar = QueryValue;
using Spec = StoreQuery;
inline uint64_t num(wire::Reader &r, wire::Field f) {
    uint64_t n = 0;
    require(f.type == wire::Type::Varint && r.varint(n), FQDB_INVALID_ARGUMENT);
    return n;
}
inline std::string_view str(wire::Reader &r, wire::Field f) {
    std::string_view v;
    require(f.type == wire::Type::Bytes && r.bytes(v), FQDB_INVALID_ARGUMENT);
    return v;
}
template <class F> void message(std::string_view bytes, F &&read) {
    wire::Reader r(bytes);
    wire::Field f;
    while (r.next(f))
        read(r, f);
    require(r.done(), FQDB_INVALID_ARGUMENT);
}
inline void once(uint64_t &seen, unsigned n) {
    require(n && n < 64 && !(seen & (UINT64_C(1) << n)), FQDB_INVALID_ARGUMENT);
    seen |= UINT64_C(1) << n;
}
inline Field field(std::string_view bytes) {
    Field v;
    v.type = 0;
    uint64_t seen = 0;
    message(bytes, [&](auto &r, auto f) {
        if (f.number != 3)
            once(seen, f.number);
        if (f.number == 1) {
            auto n = num(r, f);
            require(n <= 255, FQDB_INVALID_ARGUMENT);
            v.source = uint32_t(n);
        } else if (f.number == 2) {
            auto n = num(r, f);
            require(n <= 11, FQDB_INVALID_ARGUMENT);
            v.type = uint32_t(n);
        } else if (f.number == 3) {
            auto append = [&](uint64_t n) {
                require(n && n <= 0x1fffffff && v.path.size() < 8, FQDB_INVALID_ARGUMENT);
                v.path.push_back(uint32_t(n));
            };
            if (f.type == wire::Type::Bytes) {
                wire::Reader p(str(r, f));
                uint64_t n = 0;
                while (!p.done()) {
                    require(p.varint(n), FQDB_INVALID_ARGUMENT);
                    append(n);
                }
            } else
                append(num(r, f));
        } else
            throw Error(FQDB_INVALID_ARGUMENT);
    });
    return v;
}
inline Scalar scalar(std::string_view bytes) {
    Scalar v;
    uint64_t seen = 0;
    message(bytes, [&](auto &r, auto f) {
        once(seen, f.number);
        if (f.number == 1) {
            auto n = num(r, f);
            require(n <= 5, FQDB_INVALID_ARGUMENT);
            v.kind = Kind(n);
        } else if (f.number == 2)
            v.unsigned_value = num(r, f);
        else if (f.number == 3)
            v.signed_value = wire::unzigzag(num(r, f));
        else if (f.number == 4) {
            uint64_t n = 0;
            require(f.type == wire::Type::Fixed64 && r.fixed(n, 8), FQDB_INVALID_ARGUMENT);
            std::memcpy(&v.real_value, &n, 8);
        } else if (f.number == 5)
            v.bytes.assign(str(r, f));
        else
            throw Error(FQDB_INVALID_ARGUMENT);
    });
    const unsigned expected = v.kind == Kind::UInt || v.kind == Kind::Bool ? 2
                              : v.kind == Kind::Int                        ? 3
                              : v.kind == Kind::Real                       ? 4
                              : v.kind == Kind::Bytes                      ? 5
                                                                           : 0;
    require(!(seen & ~((UINT64_C(1) << 1) | (expected ? UINT64_C(1) << expected : 0))),
            FQDB_INVALID_ARGUMENT);
    require(v.kind != Kind::Real || std::isfinite(v.real_value), FQDB_INVALID_ARGUMENT);
    require(v.kind != Kind::Bool || v.unsigned_value <= 1, FQDB_INVALID_ARGUMENT);
    return v;
}
inline Expr expression(std::string_view bytes, unsigned depth, unsigned &nodes) {
    require(depth <= 64 && ++nodes <= 1024, FQDB_LIMIT);
    Expr e;
    uint64_t seen = 0;
    message(bytes, [&](auto &r, auto f) {
        if (f.number != 3 && f.number != 4)
            once(seen, f.number);
        if (f.number == 1) {
            auto n = num(r, f);
            require(n <= 22, FQDB_INVALID_ARGUMENT);
            e.op = Op(n);
        } else if (f.number == 2)
            e.field = field(str(r, f));
        else if (f.number == 3) {
            require(e.values.size() < 4096, FQDB_LIMIT);
            e.values.push_back(scalar(str(r, f)));
        } else if (f.number == 4)
            e.children.push_back(expression(str(r, f), depth + 1, nodes));
        else if (f.number == 5)
            e.right = field(str(r, f));
        else
            throw Error(FQDB_INVALID_ARGUMENT);
    });
    return e;
}
inline Spec specification(std::string_view bytes) {
    require(bytes.size() <= 262144, FQDB_LIMIT);
    Spec s;
    uint64_t seen = 0;
    unsigned nodes = 0;
    s.limit = s.memory_bytes = s.max_candidates = s.max_work = s.timeout_ms = 0;
    s.allow_scan = false;
    message(bytes, [&](auto &r, auto f) {
        if (f.number != 3 && f.number != 4 && f.number != 5 && f.number != 7)
            once(seen, f.number);
        if (f.number == 1)
            s.prefix.assign(str(r, f));
        else if (f.number == 2)
            s.where = expression(str(r, f), 0, nodes);
        else if (f.number == 3) {
            require(s.columns.size() < 256, FQDB_LIMIT);
            QueryColumn c;
            uint64_t z = 0;
            message(str(r, f), [&](auto &a, auto p) {
                once(z, p.number);
                if (p.number == 1)
                    c.field = field(str(a, p));
                else if (p.number == 2) {
                    auto n = num(a, p);
                    require(n <= 6, FQDB_INVALID_ARGUMENT);
                    c.aggregate = QueryAggregate(n);
                } else if (p.number == 3)
                    c.name.assign(str(a, p));
                else if (p.number == 4) {
                    auto n = num(a, p);
                    require(n <= 1, FQDB_INVALID_ARGUMENT);
                    c.distinct = n != 0;
                } else
                    throw Error(FQDB_INVALID_ARGUMENT);
            });
            s.columns.push_back(std::move(c));
        } else if (f.number == 4) {
            require(s.order.size() < 16, FQDB_LIMIT);
            QueryOrder o;
            o.nulls_first = false;
            uint64_t z = 0;
            message(str(r, f), [&](auto &a, auto p) {
                once(z, p.number);
                if (p.number == 1)
                    o.field = field(str(a, p));
                else if (p.number == 2 || p.number == 3) {
                    auto n = num(a, p);
                    require(n <= 1, FQDB_INVALID_ARGUMENT);
                    (p.number == 2 ? o.descending : o.nulls_first) = n != 0;
                } else
                    throw Error(FQDB_INVALID_ARGUMENT);
            });
            s.order.push_back(std::move(o));
        } else if (f.number == 5) {
            require(s.group.size() < 16, FQDB_LIMIT);
            s.group.push_back(field(str(r, f)));
        } else if (f.number == 6)
            s.having = expression(str(r, f), 0, nodes);
        else if (f.number == 7) {
            require(s.joins.size() < 7, FQDB_LIMIT);
            QueryJoin j;
            uint64_t z = 0;
            message(str(r, f), [&](auto &a, auto p) {
                if (p.number != 2 && p.number != 3)
                    once(z, p.number);
                if (p.number == 1)
                    j.prefix.assign(str(a, p));
                else if (p.number == 2 || p.number == 3) {
                    auto &v = p.number == 2 ? j.left : j.right;
                    require(v.size() < 8, FQDB_LIMIT);
                    v.push_back(field(str(a, p)));
                } else if (p.number == 4) {
                    auto n = num(a, p);
                    require(n <= 3, FQDB_INVALID_ARGUMENT);
                    j.kind = QueryJoinKind(n);
                } else if (p.number == 5)
                    j.on = expression(str(a, p), 0, nodes);
                else
                    throw Error(FQDB_INVALID_ARGUMENT);
            });
            s.joins.push_back(std::move(j));
        } else if (f.number >= 8 && f.number <= 16) {
            auto n = num(r, f);
            switch (f.number) {
            case 8:
                s.limit = n;
                break;
            case 9:
                s.offset = n;
                break;
            case 10:
                s.memory_bytes = n;
                break;
            case 11:
                s.max_candidates = n;
                break;
            case 12:
                s.max_work = n;
                break;
            case 13:
                s.timeout_ms = n;
                break;
            case 14:
            case 15:
            case 16:
                require(n <= 1, FQDB_INVALID_ARGUMENT);
                (f.number == 14   ? s.allow_scan
                 : f.number == 15 ? s.count_total
                                  : s.distinct) = n != 0;
                break;
            }
        } else if (f.number == 17)
            s.index_hint.assign(str(r, f));
        else if (f.number == 18)
            s.after.assign(str(r, f));
        else
            throw Error(FQDB_INVALID_ARGUMENT);
    });
    require(s.limit && s.limit <= 4096 && s.offset <= 65536 && s.memory_bytes >= 1048576 &&
                s.memory_bytes <= FQDB_MAX_TRANSACTION_BYTES && s.max_candidates && s.max_work &&
                s.timeout_ms && s.timeout_ms <= 60000 && s.index_hint.size() <= 128,
            FQDB_INVALID_ARGUMENT);
    if (s.columns.empty())
        s.columns.push_back({Field::record(), QueryAggregate::None, {}, false});
    return s;
}
struct Atom {
    Kind kind = Kind::Null;
    uint64_t u = 0;
    int64_t i = 0;
    double d = 0;
    std::string_view text;
    static Atom from(const Scalar &v) {
        return {v.kind, v.unsigned_value, v.signed_value, v.real_value, v.bytes};
    }
};
inline bool numeric(Kind k) {
    return k == Kind::UInt || k == Kind::Int || k == Kind::Real;
}
inline int cmp_u_real(uint64_t u, double d) {
    if (d < 0)
        return 1;
    if (d >= std::ldexp(1.0, 64))
        return -1;
    auto whole = static_cast<uint64_t>(d);
    if (u != whole)
        return u < whole ? -1 : 1;
    return d > std::floor(d) ? -1 : 0;
}
inline int cmp_i_real(int64_t i, double d) {
    if (d < -std::ldexp(1.0, 63))
        return 1;
    if (d >= std::ldexp(1.0, 63))
        return -1;
    auto whole = static_cast<int64_t>(d);
    if (i != whole)
        return i < whole ? -1 : 1;
    auto w = static_cast<double>(whole);
    return d > w ? -1 : d < w ? 1 : 0;
}
inline int atom_compare(const Atom &a, const Atom &b) {
    if (numeric(a.kind) && numeric(b.kind)) {
        if (a.kind == Kind::Real) {
            if (b.kind == Kind::Real)
                return a.d < b.d ? -1 : a.d > b.d ? 1 : 0;
            return b.kind == Kind::UInt ? -cmp_u_real(b.u, a.d) : -cmp_i_real(b.i, a.d);
        }
        if (b.kind == Kind::Real)
            return a.kind == Kind::UInt ? cmp_u_real(a.u, b.d) : cmp_i_real(a.i, b.d);
        if (a.kind == Kind::Int && b.kind == Kind::Int)
            return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
        if (a.kind == Kind::UInt && b.kind == Kind::UInt)
            return a.u < b.u ? -1 : a.u > b.u ? 1 : 0;
        if (a.kind == Kind::Int) {
            if (a.i < 0)
                return -1;
            auto u = uint64_t(a.i);
            return u < b.u ? -1 : u > b.u ? 1 : 0;
        }
        return -atom_compare(b, a);
    }
    if (a.kind != b.kind)
        return uint32_t(a.kind) < uint32_t(b.kind) ? -1 : 1;
    if (a.kind == Kind::Bytes)
        return compare(a.text, b.text);
    if (a.kind == Kind::Bool)
        return a.u < b.u ? -1 : a.u > b.u ? 1 : 0;
    return 0;
}
// Equality keys normalize integral doubles and nonnegative signed integers.
// Hash joins, DISTINCT and GROUP BY therefore agree with numeric predicates.
inline void canonical(wire::Writer &w, const Atom &a) {
    if (a.kind == Kind::Int && a.i >= 0) {
        w.uint_field(1, 1);
        w.uint_field(2, uint64_t(a.i));
    } else if (a.kind == Kind::Real && a.d >= 0 && a.d < std::ldexp(1.0, 64) &&
               std::floor(a.d) == a.d) {
        w.uint_field(1, 1);
        w.uint_field(2, uint64_t(a.d));
    } else if (a.kind == Kind::Real && a.d < 0 && a.d >= -std::ldexp(1.0, 63) &&
               std::floor(a.d) == a.d) {
        w.uint_field(1, 2);
        w.sint_field(3, int64_t(a.d));
    } else {
        w.uint_field(1, uint32_t(a.kind));
        if (a.kind == Kind::UInt || a.kind == Kind::Bool)
            w.uint_field(2, a.u);
        else if (a.kind == Kind::Int)
            w.sint_field(3, a.i);
        else if (a.kind == Kind::Real)
            w.double_field(4, a.d);
        else if (a.kind == Kind::Bytes)
            w.bytes_field(5, a.text);
    }
}
} // namespace fengqudb::paged::select
