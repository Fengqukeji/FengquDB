#pragma once
#include "select_fuzzy.hpp"

namespace fengqudb::paged::select {
struct Trie {
    struct Edge {
        uint32_t number;
        unsigned child;
    };
    struct Node {
        int slot = -1;
        uint32_t type = 0;
        std::vector<Edge> edges;
    };
    std::vector<Node> nodes{1};
    void add(const Field &f, unsigned slot) {
        if (f.path.empty())
            return;
        unsigned current = 0;
        for (auto number : f.path) {
            auto &edges = nodes[current].edges;
            auto p = std::find_if(edges.begin(), edges.end(),
                                  [&](auto &e) { return e.number == number; });
            if (p != edges.end())
                current = p->child;
            else {
                unsigned child = unsigned(nodes.size());
                edges.push_back({number, child});
                nodes.emplace_back();
                current = child;
            }
        }
        auto &n = nodes[current];
        require(n.slot < 0 || n.type == f.type, FQDB_INVALID_ARGUMENT);
        n.slot = int(slot);
        n.type = f.type;
    }
    void finish() {
        for (auto &n : nodes) {
            require(n.edges.empty() || n.slot < 0 || n.type == 4, FQDB_INVALID_ARGUMENT);
            std::sort(n.edges.begin(), n.edges.end(),
                      [](auto &a, auto &b) { return a.number < b.number; });
        }
    }
};
struct Predicate {
    Op op;
    unsigned field = 0, right = 0;
    bool has_right = false, root_only = true, has_null = false;
    std::vector<unsigned> children;
    std::vector<Atom> values;
    std::optional<Pattern> pattern;
    std::vector<size_t> prefix;
    std::vector<uint32_t> fuzzy_points;
    unsigned distance = 0;
};
struct Program {
    Spec spec;
    std::string request;
    Charge charge;
    uint64_t reservation;
    uint64_t signature = 1469598103934665603ull;
    uint64_t compile_steps = 0;
    Work *compiling = nullptr;
    std::unique_ptr<Memory> compiled_memory;
    struct FieldInfo {
        Field field;
        unsigned slot;
    };
    std::vector<FieldInfo> fields;
    std::array<std::vector<unsigned>, 8> slots;
    std::array<Trie, 8> tries;
    std::vector<Predicate> predicates;
    unsigned where = 0, having = 0;
    std::vector<unsigned> on, columns, order, group;
    std::vector<std::vector<unsigned>> left, right;
    bool aggregated = false;
    std::array<bool, 8> hidden{}, payload{};
    Program(std::string_view bytes, Charge allocation, uint64_t reserved, Transaction &tx,
            std::chrono::steady_clock::time_point started)
        : spec(specification(bytes)), request(bytes), charge(std::move(allocation)),
          reservation(reserved) {
        compiled_memory =
            std::make_unique<Memory>(std::move(charge), spec.memory_bytes, reservation);
        Work compiler(tx, spec, started);
        compiling = &compiler;
        compiler.check();
        auto prefix = [](std::string_view v) {
            require(v.size() <= FQDB_MAX_KEY_BYTES && !system_key(v), FQDB_INVALID_ARGUMENT);
        };
        prefix(spec.prefix);
        for (size_t i = 0; i < spec.joins.size(); ++i) {
            auto &j = spec.joins[i];
            prefix(j.prefix);
            require(!j.left.empty() && j.left.size() == j.right.size(), FQDB_INVALID_ARGUMENT);
            hidden[i + 1] = j.kind == QueryJoinKind::Semi || j.kind == QueryJoinKind::Anti;
        }
        aggregated = !spec.group.empty() ||
                     std::any_of(spec.columns.begin(), spec.columns.end(),
                                 [](auto &c) { return c.aggregate != QueryAggregate::None; });
        for (auto &f : spec.group)
            group.push_back(intern(f, false));
        for (auto &c : spec.columns) {
            require(c.name.size() <= 256, FQDB_INVALID_ARGUMENT);
            require(!c.distinct || (c.aggregate != QueryAggregate::None &&
                                    c.aggregate != QueryAggregate::CountAll),
                    FQDB_INVALID_ARGUMENT);
            columns.push_back(c.aggregate == QueryAggregate::CountAll ? UINT32_MAX
                                                                      : intern(c.field, false));
            if (c.aggregate == QueryAggregate::Sum || c.aggregate == QueryAggregate::Avg)
                require(c.field.type && c.field.type != 4 && c.field.type != 11,
                        FQDB_INVALID_ARGUMENT);
            if (aggregated && c.aggregate == QueryAggregate::None)
                require(std::find(spec.group.begin(), spec.group.end(), c.field) !=
                            spec.group.end(),
                        FQDB_INVALID_ARGUMENT);
        }
        for (auto &o : spec.order) {
            if (aggregated && o.field.source != 255)
                require(std::find(spec.group.begin(), spec.group.end(), o.field) !=
                            spec.group.end(),
                        FQDB_INVALID_ARGUMENT);
            order.push_back(intern(o.field, true));
        }
        where = compile(spec.where, false, unsigned(spec.joins.size()), false);
        require(aggregated || spec.having.op == Op::True, FQDB_INVALID_ARGUMENT);
        having = compile(spec.having, true, unsigned(spec.joins.size()), false);
        for (size_t i = 0; i < spec.joins.size(); ++i) {
            auto &j = spec.joins[i];
            std::vector<unsigned> a, b;
            for (size_t k = 0; k < j.left.size(); ++k) {
                require(j.left[k].source <= i && j.right[k].source == i + 1, FQDB_INVALID_ARGUMENT);
                a.push_back(intern(j.left[k], false));
                b.push_back(intern(j.right[k], false, true));
                auto category = [](const Field &f) {
                    return f.path.empty() || f.type == 4 ? 1 : f.type == 11 ? 2 : 3;
                };
                require(category(j.left[k]) == category(j.right[k]), FQDB_INVALID_ARGUMENT);
            }
            left.push_back(std::move(a));
            right.push_back(std::move(b));
            on.push_back(compile(j.on, false, unsigned(i + 1), true));
        }
        for (auto &t : tries)
            t.finish();
        reservation = compiled_memory->retained();
        auto logical = spec;
        logical.after.clear();
        logical.limit = 1;
        logical.offset = 0;
        logical.memory_bytes = 0;
        logical.max_candidates = 0;
        logical.max_work = 0;
        logical.timeout_ms = 0;
        logical.allow_scan = false;
        logical.count_total = false;
        logical.index_hint.clear();
        auto canonical = logical.encode();
        for (unsigned char c : canonical) {
            signature ^= c;
            signature *= 1099511628211ull;
        }
        compiler.check();
        compile_steps = compiler.stats.work;
        compiling = nullptr;
    }
    unsigned intern(const Field &f, bool output, bool allow_hidden = false) {
        if (f.source == 255)
            require(output && f.type == 0 && f.path.size() == 1 && f.path[0] <= spec.columns.size(),
                    FQDB_INVALID_ARGUMENT);
        else {
            require(f.source <= spec.joins.size() && (allow_hidden || !hidden[f.source]),
                    FQDB_INVALID_ARGUMENT);
            require(f.path.empty() ? (f.type == 0 || f.type == 4) : (f.type >= 1 && f.type <= 11),
                    FQDB_INVALID_ARGUMENT);
        }
        for (unsigned i = 0; i < fields.size(); ++i) {
            auto &old = fields[i].field;
            if (old == f)
                return i;
            if (!old.path.empty() && old.source == f.source && old.path == f.path)
                require(old.type == f.type, FQDB_INVALID_ARGUMENT);
        }
        require(fields.size() < 256, FQDB_LIMIT);
        unsigned id = unsigned(fields.size()), slot = 0;
        if (f.source != 255) {
            slot = unsigned(slots[f.source].size());
            slots[f.source].push_back(id);
            tries[f.source].add(f, slot);
            if (!f.path.empty() || f.type == 0)
                payload[f.source] = true;
        }
        fields.push_back({f, slot});
        return id;
    }
    unsigned compile(const Expr &e, bool output, unsigned max_source, bool allow_hidden) {
        if (compiling)
            compiling->tick();
        Predicate p;
        p.op = e.op;
        auto op = uint32_t(e.op);
        if (op < 5) {
            require(e.values.empty() && !e.right, FQDB_INVALID_ARGUMENT);
            require(e.op != Op::Not || e.children.size() == 1, FQDB_INVALID_ARGUMENT);
            require((e.op == Op::And || e.op == Op::Or || e.op == Op::Not) || e.children.empty(),
                    FQDB_INVALID_ARGUMENT);
            for (auto &c : e.children) {
                auto id = compile(c, output, max_source, allow_hidden);
                p.root_only = p.root_only && predicates[id].root_only;
                p.children.push_back(id);
            }
        } else {
            require(e.children.empty() &&
                        (e.field.source == 255 ? output : e.field.source <= max_source),
                    FQDB_INVALID_ARGUMENT);
            if (output && e.field.source != 255)
                require(std::find(spec.group.begin(), spec.group.end(), e.field) !=
                            spec.group.end(),
                        FQDB_INVALID_ARGUMENT);
            if (allow_hidden && e.field.source != 255)
                require(!hidden[e.field.source] || e.field.source == max_source,
                        FQDB_INVALID_ARGUMENT);
            p.field = intern(e.field, output, allow_hidden);
            p.root_only = e.field.source == 0;
            if (e.right) {
                require(op >= 5 && op <= 10 && e.values.empty() &&
                            (e.right->source == 255 ? output : e.right->source <= max_source),
                        FQDB_INVALID_ARGUMENT);
                p.has_right = true;
                if (output && e.right->source != 255)
                    require(std::find(spec.group.begin(), spec.group.end(), *e.right) !=
                                spec.group.end(),
                            FQDB_INVALID_ARGUMENT);
                if (allow_hidden && e.right->source != 255)
                    require(!hidden[e.right->source] || e.right->source == max_source,
                            FQDB_INVALID_ARGUMENT);
                p.right = intern(*e.right, output, allow_hidden);
                p.root_only = p.root_only && e.right->source == 0;
            } else if (e.op == Op::Exists || e.op == Op::IsNull)
                require(e.values.empty(), FQDB_INVALID_ARGUMENT);
            else if (e.op != Op::In)
                require(e.values.size() == (e.op == Op::Fuzzy ? 2u : 1u), FQDB_INVALID_ARGUMENT);
            for (auto &v : e.values) {
                if (v.kind == Kind::Null)
                    p.has_null = true;
                else
                    p.values.push_back(Atom::from(v));
            }
            if (e.op == Op::In) {
                std::sort(p.values.begin(), p.values.end(),
                          [](auto &a, auto &b) { return atom_compare(a, b) < 0; });
                p.values.erase(
                    std::unique(p.values.begin(), p.values.end(),
                                [](auto &a, auto &b) { return atom_compare(a, b) == 0; }),
                    p.values.end());
            }
            if (op >= 14) {
                require(e.values.size() == (e.op == Op::Fuzzy ? 2u : 1u) &&
                            e.values[0].kind == Kind::Bytes && !e.right,
                        FQDB_INVALID_ARGUMENT);
                auto pattern = e.values[0].bytes;
                if (e.op == Op::Fuzzy) {
                    auto &v = e.values[1];
                    require((v.kind == Kind::UInt && v.unsigned_value <= 8) ||
                                (v.kind == Kind::Int && v.signed_value >= 0 && v.signed_value <= 8),
                            FQDB_INVALID_ARGUMENT);
                    p.distance = unsigned(v.kind == Kind::UInt ? v.unsigned_value
                                                               : uint64_t(v.signed_value));
                    p.fuzzy_points = fuzzy_pattern(pattern);
                }
                if (e.op == Op::Regex || e.op == Op::IRegex || e.op == Op::Like ||
                    e.op == Op::ILike)
                    p.pattern.emplace(*compiled_memory, pattern,
                                      e.op == Op::IRegex || e.op == Op::ILike,
                                      e.op == Op::Like || e.op == Op::ILike, compiling);
                if (e.op == Op::Contains || e.op == Op::IContains) {
                    p.prefix.resize(pattern.size());
                    for (size_t i = 1, j = 0; i < pattern.size(); ++i) {
                        if (compiling)
                            compiling->tick();
                        auto eq = [&](size_t a, size_t b) {
                            return e.op == Op::IContains
                                       ? fold(static_cast<unsigned char>(pattern[a])) ==
                                             fold(static_cast<unsigned char>(pattern[b]))
                                       : pattern[a] == pattern[b];
                        };
                        while (j && !eq(i, j))
                            j = p.prefix[j - 1];
                        if (eq(i, j))
                            ++j;
                        p.prefix[i] = j;
                    }
                }
            }
        }
        auto id = unsigned(predicates.size());
        predicates.push_back(std::move(p));
        return id;
    }
};
inline Atom read_scalar(wire::Reader &r, wire::Field f, uint32_t type,
                        std::optional<std::string_view> bytes = {}) {
    Atom a;
    uint64_t n = 0;
    if (type == 4) {
        a.kind = Kind::Bytes;
        if (bytes)
            a.text = *bytes;
        else {
            require(f.type == wire::Type::Bytes && r.bytes(a.text), FQDB_CONSTRAINT);
        }
        return a;
    }
    auto expected = type == 1 || type == 2 || type == 5 || type == 11 ? wire::Type::Varint
                    : type == 6 || type == 9 || type == 10            ? wire::Type::Fixed32
                                                                      : wire::Type::Fixed64;
    require(f.type == expected && (expected == wire::Type::Varint
                                       ? r.varint(n)
                                       : r.fixed(n, expected == wire::Type::Fixed32 ? 4 : 8)),
            FQDB_CONSTRAINT);
    if (type == 1 || type == 7 || type == 9) {
        a.kind = Kind::UInt;
        a.u = n;
    } else if (type == 2 || type == 5 || type == 8 || type == 10) {
        a.kind = Kind::Int;
        if (type == 5)
            a.i = wire::unzigzag(n);
        else if (type == 10) {
            auto bits = uint32_t(n);
            int32_t i;
            std::memcpy(&i, &bits, 4);
            a.i = i;
        } else
            std::memcpy(&a.i, &n, 8);
    } else if (type == 11) {
        require(n <= 1, FQDB_CONSTRAINT);
        a.kind = Kind::Bool;
        a.u = n;
    } else {
        a.kind = Kind::Real;
        if (type == 3)
            std::memcpy(&a.d, &n, 8);
        else {
            auto bits = uint32_t(n);
            float v;
            std::memcpy(&v, &bits, 4);
            a.d = v;
        }
        require(std::isfinite(a.d), FQDB_CONSTRAINT);
    }
    return a;
}
struct StoredRecord {
    Text key;
    Vec<Owned> fields;
    StoredRecord(Memory &m, std::string_view k) : key(text(m, k)), fields(Alloc<Owned>(m)) {}
};
struct Record {
    const Program &program;
    unsigned source;
    Value bytes;
    std::string_view key;
    Vec<Atom> values;
    const StoredRecord *stored = nullptr;
    Charge pins;
    Record(const Program &p, unsigned source, Memory &m, std::shared_ptr<Budget> pin_budget)
        : program(p), source(source), values(Alloc<Atom>(m)), pins(std::move(pin_budget), {}, 0) {
        values.resize(p.slots[source].size());
    }
    void reset(std::string_view k, Value v, bool already_pinned = false) {
        pins.resize(v && !already_pinned ? sizeof(Data) + v->capacity() + 64 : 0);
        std::fill(values.begin(), values.end(), Atom{});
        bytes = std::move(v);
        key = k;
        stored = nullptr;
    }
    void scan(std::string_view input, unsigned at, Work &w) {
        wire::Reader r(input);
        wire::Field f;
        auto &node = program.tries[source].nodes[at];
        while (r.next(f)) {
            w.tick();
            auto p = std::lower_bound(node.edges.begin(), node.edges.end(), f.number,
                                      [](const auto &e, uint32_t n) { return e.number < n; });
            if (p == node.edges.end() || p->number != f.number) {
                require(r.skip(f), FQDB_CONSTRAINT);
                continue;
            }
            const auto &child = program.tries[source].nodes[p->child];
            if (!child.edges.empty()) {
                std::string_view nested;
                require(f.type == wire::Type::Bytes && r.bytes(nested), FQDB_CONSTRAINT);
                if (child.slot >= 0)
                    values[size_t(child.slot)] = {Kind::Bytes, 0, 0, 0, nested};
                scan(nested, p->child, w);
            } else {
                require(child.slot >= 0);
                values[size_t(child.slot)] = read_scalar(r, f, child.type);
            }
        }
        require(r.done(), FQDB_CONSTRAINT);
    }
    void parse(Work &w) {
        if (bytes && !program.tries[source].nodes[0].edges.empty())
            scan(*bytes, 0, w);
    }
    Atom get(unsigned id) const {
        auto &info = program.fields[id];
        if (stored)
            return stored->fields[info.slot].ref();
        if (info.field.path.empty())
            return {Kind::Bytes, 0, 0, 0,
                    info.field.type == 4 ? key
                    : bytes              ? std::string_view(*bytes)
                                         : std::string_view{}};
        return values[info.slot];
    }
    StoredRecord own(Memory &m, Work &w) const {
        StoredRecord r(m, key);
        r.fields.reserve(program.slots[source].size());
        for (auto id : program.slots[source]) {
            auto a = get(id);
            w.tick(a.kind == Kind::Bytes ? a.text.size() + 1 : 1);
            r.fields.emplace_back(m, get(id));
        }
        return r;
    }
};
enum class Truth { False, True, Unknown };
struct Eval {
    const Program &p;
    Work &w;
    Pattern::Scratch scratch;
    std::array<Record *, 8> records{};
    const Vec<Owned> *columns = nullptr, *group_values = nullptr;
    const Vec<Atom> *computed = nullptr;
    Vec<uint64_t> cached_epoch;
    Vec<Truth> cached_truth;
    uint64_t epoch = 1;
    Eval(const Program &p, Work &w, Memory &m)
        : p(p), w(w), scratch(m), cached_epoch(Alloc<uint64_t>(m)), cached_truth(Alloc<Truth>(m)) {
        if (!p.spec.joins.empty()) {
            cached_epoch.resize(p.predicates.size());
            cached_truth.resize(p.predicates.size());
        }
    }
    void next_root() {
        if (++epoch == 0) {
            std::fill(cached_epoch.begin(), cached_epoch.end(), 0);
            epoch = 1;
        }
    }
    Atom get(unsigned id) const {
        auto &f = p.fields[id];
        if (f.field.source == 255) {
            if (computed) {
                require(f.field.path[0] <= computed->size());
                return (*computed)[f.field.path[0] - 1];
            }
            require(columns && f.field.path[0] <= columns->size());
            return (*columns)[f.field.path[0] - 1].ref();
        }
        if (group_values) {
            auto it = std::find(p.group.begin(), p.group.end(), id);
            require(it != p.group.end());
            return (*group_values)[size_t(it - p.group.begin())].ref();
        }
        auto r = records[f.field.source];
        return r ? r->get(id) : Atom{};
    }
    Truth run(unsigned id) {
        bool cache =
            !cached_epoch.empty() && p.predicates[id].root_only && records[0] && !group_values;
        if (cache && cached_epoch[id] == epoch)
            return cached_truth[id];
        auto result = evaluate(id);
        if (cache) {
            cached_epoch[id] = epoch;
            cached_truth[id] = result;
        }
        return result;
    }
    Truth evaluate(unsigned id) {
        w.tick();
        const auto &e = p.predicates[id];
        if (e.op == Op::True)
            return Truth::True;
        if (e.op == Op::False)
            return Truth::False;
        if (e.op == Op::And || e.op == Op::Or) {
            bool unknown = false;
            for (auto child : e.children) {
                auto result = run(child);
                if (e.op == Op::And && result == Truth::False)
                    return result;
                if (e.op == Op::Or && result == Truth::True)
                    return result;
                unknown = unknown || result == Truth::Unknown;
            }
            return unknown ? Truth::Unknown : e.op == Op::And ? Truth::True : Truth::False;
        }
        if (e.op == Op::Not) {
            auto r = run(e.children[0]);
            return r == Truth::Unknown ? r : r == Truth::True ? Truth::False : Truth::True;
        }
        auto a = get(e.field);
        if (e.op == Op::Exists)
            return a.kind != Kind::Null ? Truth::True : Truth::False;
        if (e.op == Op::IsNull)
            return a.kind == Kind::Null ? Truth::True : Truth::False;
        if (e.op == Op::In && e.values.empty() && !e.has_null)
            return Truth::False;
        if (a.kind == Kind::Null)
            return Truth::Unknown;
        bool result = false;
        if (e.op == Op::In) {
            auto i =
                std::lower_bound(e.values.begin(), e.values.end(), a, [&](auto &v, const Atom &n) {
                    w.tick(v.kind == Kind::Bytes && n.kind == Kind::Bytes
                               ? std::min(v.text.size(), n.text.size()) + 1
                               : 1);
                    return atom_compare(v, n) < 0;
                });
            if (i != e.values.end() && atom_compare(*i, a) == 0)
                return Truth::True;
            return e.has_null ? Truth::Unknown : Truth::False;
        }
        auto b = e.has_right ? get(e.right) : e.values.empty() ? Atom{} : e.values[0];
        if (b.kind == Kind::Null)
            return Truth::Unknown;
        require((numeric(a.kind) && numeric(b.kind)) || a.kind == b.kind, FQDB_CONSTRAINT);
        if (uint32_t(e.op) <= 10 && a.kind == Kind::Bytes)
            w.tick(std::min(a.text.size(), b.text.size()) + 1);
        if (uint32_t(e.op) <= 10) {
            auto c = atom_compare(a, b);
            result = e.op == Op::Eq   ? c == 0
                     : e.op == Op::Ne ? c != 0
                     : e.op == Op::Lt ? c < 0
                     : e.op == Op::Le ? c <= 0
                     : e.op == Op::Gt ? c > 0
                                      : c >= 0;
        } else {
            require(a.kind == Kind::Bytes && b.kind == Kind::Bytes, FQDB_CONSTRAINT);
            if (e.op == Op::Fuzzy)
                result = fuzzy(a.text, e.fuzzy_points, e.distance, w, scratch);
            else if (e.pattern)
                result = e.pattern->matches(a.text, w, scratch);
            else if (e.op == Op::StartsWith || e.op == Op::EndsWith) {
                w.tick(b.text.size());
                result = a.text.size() >= b.text.size() &&
                         (e.op == Op::StartsWith
                              ? a.text.substr(0, b.text.size()) == b.text
                              : a.text.substr(a.text.size() - b.text.size()) == b.text);
            } else {
                result = b.text.empty();
                for (size_t i = 0, j = 0; !result && i < a.text.size(); ++i) {
                    w.tick();
                    auto eq = [&](unsigned char x, unsigned char y) {
                        return e.op == Op::IContains ? fold(x) == fold(y) : x == y;
                    };
                    while (j && !eq(static_cast<unsigned char>(a.text[i]),
                                    static_cast<unsigned char>(b.text[j])))
                        j = e.prefix[j - 1];
                    if (eq(static_cast<unsigned char>(a.text[i]),
                           static_cast<unsigned char>(b.text[j])))
                        ++j;
                    if (j == b.text.size())
                        result = true;
                }
            }
        }
        return result ? Truth::True : Truth::False;
    }
    bool root_filter(unsigned id) {
        const auto &e = p.predicates[id];
        if (e.root_only)
            return run(id) == Truth::True;
        if (e.op == Op::And)
            for (auto c : e.children)
                if (!root_filter(c))
                    return false;
        return true;
    }
};
} // namespace fengqudb::paged::select
