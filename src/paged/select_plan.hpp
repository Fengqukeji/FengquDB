#pragma once
#include "select_program.hpp"

namespace fengqudb::paged::select {
inline bool begins(std::string_view a, std::string_view prefix) {
    return a.size() >= prefix.size() && a.substr(0, prefix.size()) == prefix;
}
inline std::optional<Bytes> index_atom(uint32_t type, const Atom &a) {
    wire::Writer w;
    if (type == 11) {
        if (a.kind != Kind::Bool)
            return {};
        w.uint_field(1, a.u);
    } else if (type == 4) {
        if (a.kind != Kind::Bytes || a.text.size() > FQDB_MAX_KEY_BYTES)
            return {};
        w.bytes_field(1, a.text);
    } else if (type == 3 || type == 6) {
        if (!numeric(a.kind))
            return {};
        double d = a.kind == Kind::Real ? a.d : a.kind == Kind::Int ? double(a.i) : double(a.u);
        if (type == 6)
            d = double(float(d));
        Atom real{Kind::Real, 0, 0, d, {}};
        if (!std::isfinite(d) || atom_compare(a, real))
            return {};
        if (type == 3)
            w.double_field(1, d);
        else
            w.float_field(1, float(d));
    } else {
        if (!numeric(a.kind))
            return {};
        bool sign = type == 2 || type == 5 || type == 8 || type == 10;
        uint64_t u = 0;
        int64_t i = 0;
        if (sign) {
            if (a.kind == Kind::UInt) {
                if (a.u > uint64_t(INT64_MAX))
                    return {};
                i = int64_t(a.u);
            } else if (a.kind == Kind::Int)
                i = a.i;
            else {
                if (a.d < -std::ldexp(1.0, 63) || a.d >= std::ldexp(1.0, 63) ||
                    std::floor(a.d) != a.d)
                    return {};
                i = int64_t(a.d);
            }
            if (type == 10 && (i < INT32_MIN || i > INT32_MAX))
                return {};
        } else {
            if (a.kind == Kind::Int) {
                if (a.i < 0)
                    return {};
                u = uint64_t(a.i);
            } else if (a.kind == Kind::UInt)
                u = a.u;
            else {
                if (a.d < 0 || a.d >= std::ldexp(1.0, 64) || std::floor(a.d) != a.d)
                    return {};
                u = uint64_t(a.d);
            }
            if (type == 9 && u > UINT32_MAX)
                return {};
        }
        if (type == 1)
            w.uint_field(1, u);
        else if (type == 2)
            w.uint_field(1, uint64_t(i));
        else if (type == 5)
            w.sint_field(1, i);
        else if (type == 7)
            w.fixed64_field(1, u);
        else if (type == 8)
            w.fixed64_field(1, uint64_t(i));
        else if (type == 9)
            w.fixed32_field(1, uint32_t(u));
        else if (type == 10)
            w.fixed32_field(1, uint32_t(i));
        else
            return {};
    }
    wire::Reader r(w.data());
    wire::Field f;
    require(r.next(f));
    return scalar_index_value(r, f, type);
}
struct Constraint {
    std::optional<std::vector<Atom>> equal;
    std::optional<Atom> low, high, prefix;
    bool low_open = false, high_open = false, present = false, empty = false;
    void values(std::vector<Atom> list) {
        list.erase(
            std::remove_if(list.begin(), list.end(), [](auto &a) { return a.kind == Kind::Null; }),
            list.end());
        std::sort(list.begin(), list.end(),
                  [](const Atom &a, const Atom &b) { return atom_compare(a, b) < 0; });
        list.erase(std::unique(list.begin(), list.end(),
                               [](const Atom &a, const Atom &b) { return !atom_compare(a, b); }),
                   list.end());
        if (equal) {
            std::vector<Atom> both;
            for (auto &a : *equal)
                for (auto &b : list)
                    if (!atom_compare(a, b)) {
                        both.push_back(a);
                        break;
                    }
            equal = std::move(both);
        } else
            equal = std::move(list);
        if (equal->empty())
            empty = true;
        present = true;
    }
    void bound(Atom a, bool lower, bool open) {
        auto &v = lower ? low : high;
        auto &exclusive = lower ? low_open : high_open;
        if (!v || (lower ? atom_compare(a, *v) > 0 : atom_compare(a, *v) < 0)) {
            v = a;
            exclusive = open;
        } else if (!atom_compare(a, *v))
            exclusive = exclusive || open;
        present = true;
    }
    void finish() {
        if (low && high) {
            int c = atom_compare(*low, *high);
            if (c > 0 || (!c && (low_open || high_open)))
                empty = true;
        }
        if (equal) {
            auto &v = *equal;
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](auto &a) {
                                       int l = low ? atom_compare(a, *low) : 1,
                                           h = high ? atom_compare(a, *high) : -1;
                                       return (low && (l < 0 || (!l && low_open))) ||
                                              (high && (h > 0 || (!h && high_open)));
                                   }),
                    v.end());
            if (v.empty())
                empty = true;
        }
    }
};
using Clause = std::vector<const Expr *>;
inline std::vector<Clause> dnf(const Expr &e, unsigned depth = 0) {
    require(depth <= 64, FQDB_LIMIT);
    if (e.op == Op::False)
        return {};
    if (e.op == Op::True)
        return {{}};
    if (e.op == Op::Or) {
        std::vector<Clause> out;
        for (auto &c : e.children) {
            auto n = dnf(c, depth + 1);
            if (out.size() + n.size() > 32)
                return {{}};
            out.insert(out.end(), std::make_move_iterator(n.begin()),
                       std::make_move_iterator(n.end()));
        }
        return out;
    }
    if (e.op == Op::And) {
        std::vector<Clause> out{{}};
        for (auto &c : e.children) {
            auto n = dnf(c, depth + 1);
            if (n.empty())
                return {};
            if (out.size() > 32 / n.size())
                return {{}};
            std::vector<Clause> joined;
            for (auto &a : out)
                for (auto &b : n) {
                    auto v = a;
                    v.insert(v.end(), b.begin(), b.end());
                    joined.push_back(std::move(v));
                }
            out = std::move(joined);
        }
        return out;
    }
    return {{&e}};
}
inline std::map<uint32_t, Constraint> constraints(const Clause &clause) {
    std::map<uint32_t, Constraint> out;
    for (auto e : clause) {
        if (e->field.source || e->field.path.size() != 1 || e->right)
            continue;
        auto op = uint32_t(e->op);
        if ((op < 5 || op > 13 || e->op == Op::IsNull) && e->op != Op::StartsWith &&
            e->op != Op::Like)
            continue;
        auto &c = out[e->field.path[0]];
        if (e->op == Op::StartsWith) {
            if (e->values.size() == 1 && e->values[0].kind == Kind::Bytes) {
                c.present = true;
                c.prefix = Atom::from(e->values[0]);
            }
            continue;
        }
        if (e->op == Op::Like) {
            c.present = true;
            if (e->values.size() == 1 && e->values[0].kind == Kind::Bytes) {
                auto a = Atom::from(e->values[0]);
                auto n = a.text.find_first_of("%_\\");
                if (n == std::string_view::npos)
                    c.values({a});
                else if (n) {
                    a.text = a.text.substr(0, n);
                    c.prefix = a;
                }
            }
            continue;
        }
        if (e->op == Op::Exists || e->op == Op::Ne) {
            c.present = true;
            continue;
        }
        if (e->op == Op::Eq || e->op == Op::In) {
            std::vector<Atom> v;
            for (auto &x : e->values)
                v.push_back(Atom::from(x));
            c.values(std::move(v));
        } else if (e->values.size() == 1) {
            auto a = Atom::from(e->values[0]);
            if (a.kind == Kind::Null)
                c.empty = true;
            else
                c.bound(a, e->op == Op::Gt || e->op == Op::Ge, e->op == Op::Gt || e->op == Op::Lt);
        }
    }
    for (auto &c : out)
        c.second.finish();
    return out;
}
struct Interval {
    Bytes begin, end;
};
struct Plan {
    std::optional<size_t> rule;
    std::vector<Interval> intervals;
    QueryAccess access;
    bool reverse = false;
    unsigned narrow = 0;
    Hold memory;
    explicit Plan(Memory &m) : memory(m) {
        memory.resize(sizeof(Plan) + 128);
    }
    void add(Bytes begin, Bytes end) {
        memory.resize(memory.bytes + begin.capacity() + end.capacity() + sizeof(Interval) * 2 + 64);
        intervals.push_back({std::move(begin), std::move(end)});
    }
};
inline bool covering(const Program &p, unsigned source, const IndexRule &r) {
    for (auto id : p.slots[source]) {
        auto &f = p.fields[id].field;
        if (f.path.empty()) {
            if (f.type == 4)
                continue;
            return false;
        }
        if (f.path.size() != 1)
            return false;
        bool found = false;
        for (size_t i = 0; i < r.count(); ++i)
            if (r.part(i).field == f.path[0] && r.part(i).type == f.type)
                found = true;
        if (!found)
            return false;
    }
    return true;
}
inline bool ordered(const Program &p, const IndexRule &r, const std::map<uint32_t, Constraint> &c,
                    bool &reverse) {
    size_t o = 0;
    bool direction = p.spec.order.empty() ? false : p.spec.order[0].descending;
    reverse = direction;
    for (size_t i = 0; i < r.count(); ++i) {
        auto part = r.part(i);
        auto it = c.find(part.field);
        bool fixed = it != c.end() && it->second.equal && it->second.equal->size() == 1;
        if (fixed) {
            if (o < p.spec.order.size() &&
                p.spec.order[o].field == Field::field(part.field, part.type)) {
                if (p.spec.order[o].descending != direction)
                    return false;
                ++o;
            }
            continue;
        }
        if (o >= p.spec.order.size())
            return false;
        auto &x = p.spec.order[o++];
        if (x.field != Field::field(part.field, part.type) || x.descending != direction)
            return false;
    }
    if (o < p.spec.order.size()) {
        auto &x = p.spec.order[o++];
        if (x.field != Field::key() || x.descending != direction)
            return false;
    }
    return o == p.spec.order.size();
}
inline std::unique_ptr<Plan> index_plan(const Program &p, const IndexRule &r, size_t rule,
                                        const Clause &clause, Memory &m) {
    auto c = constraints(clause);
    auto out = std::make_unique<Plan>(m);
    out->rule = rule;
    out->access.strategy = "index_range";
    out->access.index = r.name;
    out->access.covering = covering(p, 0, r);
    for (auto &part : c)
        if (part.second.empty) {
            out->access.strategy = "empty";
            return out;
        }
    for (size_t i = 0; i < r.count(); ++i) {
        auto part = r.part(i);
        auto it = c.find(part.field);
        if (!(r.flags & 2) && (it == c.end() || !it->second.present))
            return {};
    }
    std::vector<Bytes> prefixes{index_root(r)};
    bool bounded = false;
    for (size_t i = 0; i < r.count(); ++i) {
        auto part = r.part(i);
        auto it = c.find(part.field);
        if (it == c.end())
            break;
        auto &v = it->second;
        // A condition using another physical type cannot bound this index.
        bool same = true;
        for (auto e : clause)
            if (e->field.source == 0 && e->field.path == std::vector<uint32_t>{part.field} &&
                e->field.type != part.type)
                same = false;
        if (!same)
            return {};
        if (v.equal) {
            if (v.equal->size() > 128 / prefixes.size())
                return {};
            std::vector<Bytes> next;
            for (auto &a : prefixes)
                for (auto &value : *v.equal) {
                    auto encoded = index_atom(part.type, value);
                    if (!encoded)
                        return {};
                    auto b = a + component(*encoded);
                    require(b.size() <= max_internal_key_bytes, FQDB_LIMIT);
                    next.push_back(std::move(b));
                }
            prefixes = std::move(next);
            ++out->narrow;
            continue;
        }
        if (v.low || v.high) {
            for (auto &a : prefixes) {
                auto low = a, high = prefix_end(a);
                if (v.low) {
                    auto encoded = index_atom(part.type, *v.low);
                    if (!encoded)
                        return {};
                    low = a + component(*encoded);
                    if (v.low_open)
                        low = prefix_end(low);
                }
                if (v.high) {
                    auto encoded = index_atom(part.type, *v.high);
                    if (!encoded)
                        return {};
                    high = a + component(*encoded);
                    if (!v.high_open)
                        high = prefix_end(high);
                }
                out->add(std::move(low), std::move(high));
            }
            ++out->narrow;
            bounded = true;
        } else if (v.prefix && part.type == 4) {
            for (auto &a : prefixes) {
                auto encoded = component(v.prefix->text);
                encoded.resize(encoded.size() - 2);
                auto low = a + encoded;
                out->add(low, prefix_end(low));
            }
            ++out->narrow;
            bounded = true;
        }
        break;
    }
    if (!bounded)
        for (auto &prefix : prefixes)
            out->add(prefix, prefix_end(prefix));
    out->access.ordered = out->intervals.size() == 1 && ordered(p, r, c, out->reverse);
    bool covering_aggregate = p.aggregated && p.payload[0] && out->access.covering;
    if (!out->narrow && (!out->access.ordered || p.spec.order.empty()) && !covering_aggregate)
        return {};
    if (!out->narrow) {
        out->access.strategy = covering_aggregate ? "covering_index_scan" : "ordered_index";
        out->access.needs_scan = covering_aggregate;
    }
    out->access.reason = "leading equality/range; other conditions stay in the native filter";
    return out;
}
inline uint64_t sample(const Transaction &tx, const Plan &p, Work &w) {
    uint64_t n = 0, visited = 0;
    for (auto &range : p.intervals) {
        ViewCursor c(*tx.base->view, range.begin);
        while (auto row = c.next()) {
            w.tick();
            if (!range.end.empty() && compare(row->key, range.end) >= 0)
                break;
            if (row->value.visible(tx.base->time))
                ++n;
            if (n >= 32 || ++visited >= 128)
                return n;
        }
    }
    return n;
}
inline std::unique_ptr<Plan> primary_plan(const Program &p, const Clause &clause, Memory &m) {
    std::vector<Expr> keys;
    keys.reserve(clause.size());
    for (auto e : clause)
        if (e->field == Field::key() && !e->right && uint32_t(e->op) >= 5 &&
            uint32_t(e->op) <= 17) {
            keys.push_back(*e);
            keys.back().field.path = {1};
        }
    if (keys.empty())
        return {};
    Clause refs;
    for (auto &e : keys)
        refs.push_back(&e);
    auto all = constraints(refs);
    auto found = all.find(1);
    if (found == all.end())
        return {};
    auto &c = found->second;
    if (!c.empty && !c.equal && !c.low && !c.high && !c.prefix)
        return {};
    auto out = std::make_unique<Plan>(m);
    out->access.strategy = c.empty ? "empty" : "primary_key_range";
    out->access.reason =
        "bounded primary-key predicate; other conditions remain in the native filter";
    if (!c.empty) {
        if (c.equal) {
            if (c.equal->size() > 128)
                return {};
            for (auto &a : *c.equal) {
                if (a.kind != Kind::Bytes)
                    return {};
                if (begins(a.text, p.spec.prefix)) {
                    Bytes end(a.text);
                    end.push_back(0);
                    out->add(Bytes(a.text), std::move(end));
                }
            }
            out->access.strategy = "primary_key";
        } else {
            auto low = p.spec.prefix, high = prefix_end(p.spec.prefix);
            auto intersect = [&](Bytes a, Bytes b) {
                if (compare(a, low) > 0)
                    low = std::move(a);
                if (high.empty() || (!b.empty() && compare(b, high) < 0))
                    high = std::move(b);
            };
            if (c.prefix) {
                if (c.prefix->kind != Kind::Bytes)
                    return {};
                intersect(Bytes(c.prefix->text), prefix_end(Bytes(c.prefix->text)));
            }
            if (c.low) {
                if (c.low->kind != Kind::Bytes)
                    return {};
                Bytes b(c.low->text);
                if (c.low_open)
                    b.push_back(0);
                if (compare(b, low) > 0)
                    low = std::move(b);
            }
            if (c.high) {
                if (c.high->kind != Kind::Bytes)
                    return {};
                Bytes b(c.high->text);
                if (!c.high_open)
                    b.push_back(0);
                if (high.empty() || compare(b, high) < 0)
                    high = std::move(b);
            }
            if (high.empty() || compare(low, high) < 0)
                out->add(std::move(low), std::move(high));
        }
    }
    out->reverse = p.spec.order.size() == 1 && p.spec.order[0].field == Field::key() &&
                   p.spec.order[0].descending;
    out->access.ordered = (p.spec.order.empty() ||
                           (p.spec.order.size() == 1 && p.spec.order[0].field == Field::key())) &&
                          (!out->reverse || out->intervals.size() <= 1);
    return out;
}
inline std::vector<std::unique_ptr<Plan>> root_plans(const Program &p,
                                                     const std::vector<IndexRule> &rules,
                                                     Transaction &tx, Memory &m, Work &w,
                                                     bool enforce_scan = true) {
    auto branches = dnf(p.spec.where);
    std::vector<std::unique_ptr<Plan>> out;
    bool hint_found = p.spec.index_hint.empty();
    for (auto &r : rules)
        if (r.name == p.spec.index_hint)
            hint_found = true;
    require(hint_found, FQDB_NOT_FOUND);
    for (auto &clause : branches) {
        std::unique_ptr<Plan> best;
        if (p.spec.index_hint.empty())
            best = primary_plan(p, clause, m);
        if (best) {
            out.push_back(std::move(best));
            continue;
        }
        for (size_t i = 0; i < rules.size(); ++i) {
            auto &r = rules[i];
            if (!begins(p.spec.prefix, r.prefix) ||
                (!p.spec.index_hint.empty() && r.name != p.spec.index_hint))
                continue;
            auto plan = index_plan(p, r, i, clause, m);
            if (!plan)
                continue;
            plan->access.sampled_candidates = sample(tx, *plan, w);
            auto score = [](const Plan &x) {
                return std::make_tuple(x.access.sampled_candidates, -int(x.narrow),
                                       !x.access.ordered, !x.access.covering);
            };
            if (!best || score(*plan) < score(*best))
                best = std::move(plan);
        }
        if (!best) {
            auto c = constraints(clause);
            bool empty = false;
            for (auto &v : c)
                empty = empty || v.second.empty;
            best = std::make_unique<Plan>(m);
            best->access.strategy = empty ? "empty" : "prefix_scan";
            best->access.needs_scan = !empty;
            best->access.ordered = p.spec.order.empty();
            best->access.reason =
                empty ? "contradictory conditions"
                      : "no complete compatible index bounds; scan the source prefix";
            if (!empty)
                best->add(p.spec.prefix, prefix_end(p.spec.prefix));
            if (!empty) {
                require(p.spec.index_hint.empty(), FQDB_INVALID_ARGUMENT);
                require(!enforce_scan || p.spec.allow_scan, FQDB_UNSUPPORTED);
                out.clear();
                out.push_back(std::move(best));
                return out;
            }
        }
        require(!enforce_scan || p.spec.allow_scan || !best->access.needs_scan, FQDB_UNSUPPORTED);
        out.push_back(std::move(best));
    }
    return out;
}
struct JoinPlan {
    std::optional<size_t> rule;
    bool primary = false;
    std::vector<unsigned> bindings;
    QueryAccess access;
};
inline JoinPlan join_plan(const Program &p, unsigned source, const std::vector<IndexRule> &rules) {
    const auto &j = p.spec.joins[source - 1];
    JoinPlan best;
    best.access.source = source;
    best.access.strategy = "hash_join";
    best.access.needs_scan = true;
    best.access.reason = "no compatible equality index; build a bounded hash table once";
    if (j.right.size() == 1 && j.right[0].path.empty() && j.right[0].type == 4) {
        best.primary = true;
        best.access.strategy = "primary_key_join";
        best.access.needs_scan = false;
        best.access.reason = "right record key is the equality key";
        return best;
    }
    for (size_t i = 0; i < rules.size(); ++i) {
        auto &r = rules[i];
        if (!begins(j.prefix, r.prefix))
            continue;
        std::vector<unsigned> bindings;
        bool complete = true;
        for (size_t k = 0; k < r.count(); ++k) {
            auto part = r.part(k);
            auto it = std::find(j.right.begin(), j.right.end(),
                                Field::field(part.field, part.type, source));
            if (it == j.right.end()) {
                complete = false;
                break;
            }
            bindings.push_back(unsigned(it - j.right.begin()));
        }
        if (bindings.empty() || (!complete && !(r.flags & 2)))
            continue;
        if (!best.rule || bindings.size() > best.bindings.size()) {
            best.rule = i;
            best.bindings = std::move(bindings);
            best.access.strategy = "index_join";
            best.access.index = r.name;
            best.access.needs_scan = false;
            best.access.covering = covering(p, source, r);
            best.access.reason = "probe the right equality index for each left row";
        }
    }
    return best;
}
} // namespace fengqudb::paged::select
