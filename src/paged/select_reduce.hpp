#pragma once
#include "select_plan.hpp"

namespace fengqudb::paged::select {
struct Encoded {
    Bytes bytes;
    Hold hold;
    explicit Encoded(Memory &m) : hold(m) {}
};
inline size_t atom_size(const Atom &a) {
    return a.kind == Kind::Bytes ? a.text.size() + 24 : 32;
}
inline void atom_message(wire::Writer &w, unsigned field, const Atom &a) {
    wire::Writer v;
    v.reserve(atom_size(a));
    v.uint_field(1, uint32_t(a.kind));
    if (a.kind == Kind::UInt || a.kind == Kind::Bool)
        v.uint_field(2, a.u);
    else if (a.kind == Kind::Int)
        v.sint_field(3, a.i);
    else if (a.kind == Kind::Real)
        v.double_field(4, a.d);
    else if (a.kind == Kind::Bytes)
        v.bytes_field(5, a.text);
    w.bytes_field(field, v.data());
}
template <class Get>
Encoded key(Memory &m, size_t count, Get get, bool reject_null = false, Work *work = nullptr) {
    Encoded out(m);
    size_t size = 32;
    for (size_t i = 0; i < count; ++i) {
        auto a = get(i);
        if (work)
            work->tick(a.kind == Kind::Bytes ? a.text.size() + 1 : 1);
        if (reject_null && a.kind == Kind::Null)
            return out;
        require(atom_size(a) <= SIZE_MAX - size, FQDB_LIMIT);
        size += atom_size(a) + 16;
    }
    out.hold.resize(size * 3);
    wire::Writer w;
    w.reserve(size);
    for (size_t i = 0; i < count; ++i) {
        wire::Writer v;
        canonical(v, get(i));
        w.bytes_field(1, v.data());
    }
    out.bytes = w.take();
    return out;
}
struct Output {
    Text key, identity;
    Vec<Owned> values, sort;
    explicit Output(Memory &m)
        : key(Alloc<char>(m)), identity(Alloc<char>(m)), values(Alloc<Owned>(m)),
          sort(Alloc<Owned>(m)) {}
};
inline int ordering(const Program &p, const Atom &a, const Atom &b, size_t i, Work *work) {
    if (work)
        work->tick(a.kind == Kind::Bytes && b.kind == Kind::Bytes
                       ? std::min(a.text.size(), b.text.size()) + 1
                       : 1);
    const auto &o = p.spec.order[i];
    if (a.kind == Kind::Null || b.kind == Kind::Null) {
        if (a.kind == b.kind)
            return 0;
        return (a.kind == Kind::Null) == o.nulls_first ? -1 : 1;
    }
    int c = atom_compare(a, b);
    return o.descending ? -c : c;
}
template <class A, class B>
int row_compare(const Program &p, A a, B b, std::string_view ai, std::string_view bi,
                Work *work = nullptr) {
    for (size_t i = 0; i < p.order.size(); ++i) {
        int c = ordering(p, a(i), b(i), i, work);
        if (c)
            return c;
    }
    if (work)
        work->tick(std::min(ai.size(), bi.size()) + 1);
    int c = compare(ai, bi);
    return !p.spec.order.empty() && p.spec.order.back().descending ? -c : c;
}
inline int row_compare(const Program &p, const Output &a, const Output &b, Work *work = nullptr) {
    return row_compare(
        p, [&](size_t i) { return a.sort[i].ref(); }, [&](size_t i) { return b.sort[i].ref(); },
        view(a.identity), view(b.identity), work);
}
struct Reducer {
    uint64_t count = 0;
    Owned value;
    double sum = 0, correction = 0, mean = 0;
    std::unique_ptr<Set> distinct;
    explicit Reducer(Memory &m) : value(m) {}
    void add(const QueryColumn &column, const Atom &a, Memory &m, Work &w) {
        if (column.aggregate != QueryAggregate::CountAll && a.kind == Kind::Null)
            return;
        if (column.distinct) {
            auto encoded = key(m, 1, [&](size_t) { return a; }, false, &w);
            if (!distinct) {
                m.take(sizeof(Set) + 64);
                distinct = std::make_unique<Set>(0, Hash{}, std::equal_to<Text>{}, Alloc<Text>(m));
            }
            if (!distinct->insert(text(m, encoded.bytes)).second)
                return;
        }
        require(count != UINT64_MAX, FQDB_LIMIT);
        ++count;
        w.tick();
        switch (column.aggregate) {
        case QueryAggregate::CountAll:
        case QueryAggregate::Count:
            return;
        case QueryAggregate::Min:
        case QueryAggregate::Max:
            w.tick(a.kind == Kind::Bytes ? a.text.size() + 1 : 1);
            if (count == 1 ||
                (column.aggregate == QueryAggregate::Min ? atom_compare(a, value.ref()) < 0
                                                         : atom_compare(a, value.ref()) > 0))
                value = Owned(m, a);
            return;
        case QueryAggregate::Sum:
            require(numeric(a.kind), FQDB_CONSTRAINT);
            if (count == 1) {
                value = Owned(m, a);
                sum = a.kind == Kind::Real ? a.d : 0;
                return;
            }
            if (a.kind == Kind::UInt) {
                require(value.scalar.kind == Kind::UInt && a.u <= UINT64_MAX - value.scalar.u,
                        FQDB_CONSTRAINT);
                value.scalar.u += a.u;
            } else if (a.kind == Kind::Int) {
                require(value.scalar.kind == Kind::Int &&
                            !(a.i > 0 && value.scalar.i > INT64_MAX - a.i) &&
                            !(a.i < 0 && value.scalar.i < INT64_MIN - a.i),
                        FQDB_CONSTRAINT);
                value.scalar.i += a.i;
            } else {
                double y = a.d - correction, t = sum + y;
                require(std::isfinite(y) && std::isfinite(t), FQDB_CONSTRAINT);
                correction = (t - sum) - y;
                sum = t;
                value.scalar.d = sum;
            }
            return;
        case QueryAggregate::Avg: {
            require(numeric(a.kind), FQDB_CONSTRAINT);
            double x = a.kind == Kind::Real ? a.d : a.kind == Kind::Int ? double(a.i) : double(a.u);
            double n = double(count);
            if (count == 1)
                mean = x;
            else if (std::signbit(x) != std::signbit(mean))
                mean = mean * (1 - 1 / n) + x / n;
            else
                mean += (x - mean) / n;
            require(std::isfinite(mean), FQDB_CONSTRAINT);
            return;
        }
        default:
            return;
        }
    }
    Atom result(QueryAggregate op) const {
        if (op == QueryAggregate::CountAll || op == QueryAggregate::Count)
            return {Kind::UInt, count, 0, 0, {}};
        if (!count)
            return {};
        if (op == QueryAggregate::Avg)
            return {Kind::Real, 0, 0, mean, {}};
        return value.ref();
    }
};
struct Group {
    Vec<Owned> keys;
    Vec<Reducer> reducers;
    explicit Group(Memory &m) : keys(Alloc<Owned>(m)), reducers(Alloc<Reducer>(m)) {}
};
} // namespace fengqudb::paged::select
