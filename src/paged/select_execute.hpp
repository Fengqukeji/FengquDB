#pragma once
#include "select_reduce.hpp"

namespace fengqudb::paged::select {
struct CoverScratch {
    std::array<Bytes, 8> values;
    Hold hold;
    explicit CoverScratch(Memory &m) : hold(m) {}
    // 解码索引键里各字段的编码值到 values，返回字段之后的剩余部分。
    // 非唯一索引的剩余部分就是主键的 component 编码，因此无需读取索引条目的值。
    std::string_view decode(const IndexRule &rule, std::string_view root,
                            std::string_view index_key) {
        require(begins(index_key, root));
        size_t allowance = index_key.size() * 4 + 2048;
        if (allowance > hold.bytes)
            hold.resize(allowance);
        auto rest = index_key.substr(root.size());
        for (size_t i = 0; i < rule.count(); ++i) {
            auto n = component_length(rest);
            values[i] = uncomponent(rest.substr(0, n));
            rest.remove_prefix(n);
        }
        return rest;
    }
    void fill(Record &record, const IndexRule &rule, std::string_view index_key,
              std::string_view owner) {
        decode(rule, index_root(rule), index_key);
        assign(record, rule, owner);
    }
    // 把已解码的索引字段写入记录槽位；记录的其余字段保持未设置。
    void assign(Record &record, const IndexRule &rule, std::string_view owner) {
        record.reset(owner, {});
        for (auto id : record.program.slots[record.source]) {
            const auto &f = record.program.fields[id];
            if (f.field.path.empty())
                continue;
            for (size_t i = 0; i < rule.count(); ++i)
                if (f.field.path[0] == rule.part(i).field) {
                    Atom a;
                    auto type = rule.part(i).type;
                    if (type == 4)
                        a = {Kind::Bytes, 0, 0, 0, values[i]};
                    else if (type == 3 || type == 6)
                        a = {Kind::Real, 0, 0, decode_score(values[i]), {}};
                    else {
                        auto n = from_big(values[i]);
                        if (type == 2 || type == 5 || type == 8 || type == 10) {
                            n ^= UINT64_C(1) << 63;
                            a.kind = Kind::Int;
                            std::memcpy(&a.i, &n, 8);
                        } else {
                            a.kind = type == 11 ? Kind::Bool : Kind::UInt;
                            if (type == 11)
                                require(n <= 1);
                            a.u = n;
                        }
                    }
                    record.values[f.slot] = a;
                    break;
                }
        }
    }
};
struct JoinState {
    JoinPlan plan;
    Map<Vec<StoredRecord>> table;
    bool built = false;
    explicit JoinState(Memory &m)
        : table(0, Hash{}, std::equal_to<Text>{},
                Alloc<std::pair<const Text, Vec<StoredRecord>>>(m)) {}
};
struct CachedPlan {
    std::optional<size_t> rule;
    std::vector<Interval> intervals;
    QueryAccess access;
    bool reverse;
};
struct PlanCache {
    std::weak_ptr<const Program> program;
    uint64_t epoch = 0, version = 0;
    std::vector<IndexRule> rules;
    std::vector<CachedPlan> roots;
    std::vector<JoinPlan> joins;
    Charge charge;
    explicit PlanCache(Charge c) : charge(std::move(c)) {}
};
struct Executor {
    Transaction &tx;
    const Program &p;
    Memory &memory;
    Work work;
    Eval eval;
    Transaction read;
    // Request-local B-tree page path: primary-key lookups reuse cached upper pages.
    Charge path_charge;
    std::optional<Table::ReadPath> path;
    std::vector<IndexRule> rules;
    std::vector<bool> prepared;
    std::vector<std::unique_ptr<Plan>> plans;
    std::vector<std::unique_ptr<JoinState>> joins;
    std::array<std::unique_ptr<Record>, 8> records;
    Vec<Output> heap;
    Map<Group> groups;
    Group *last_group = nullptr;
    Set unique, seen;
    Vec<Atom> computed, sort_values;
    Vec<Owned> cursor_sort;
    Text cursor_identity;
    uint64_t signature = 0;
    std::string_view after;
    bool has_cursor = false, stop = false, early = false;
    std::vector<QueryAccess> access;
    Executor(Transaction &t, const Program &program, Memory &m, bool explain_only = false,
             std::string_view after_token = {},
             const std::shared_ptr<const Program> *owner = nullptr,
             std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(),
             uint64_t initial_work = 0)
        : tx(t), p(program), memory(m), work(t, p.spec, started), eval(p, work, m),
          read(t.base, 0, t.budget), heap(Alloc<Output>(m)),
          groups(0, Hash{}, std::equal_to<Text>{}, Alloc<std::pair<const Text, Group>>(m)),
          unique(0, Hash{}, std::equal_to<Text>{}, Alloc<Text>(m)),
          seen(0, Hash{}, std::equal_to<Text>{}, Alloc<Text>(m)), computed(Alloc<Atom>(m)),
          sort_values(Alloc<Atom>(m)), cursor_sort(Alloc<Owned>(m)),
          cursor_identity(Alloc<char>(m)), after(after_token) {
        require(initial_work <= p.spec.max_work, FQDB_LIMIT);
        work.stats.work = initial_work;
        work.check();
        read.writes = t.writes;
        read.advanced = true;
        read.has_deadline = t.has_deadline;
        read.deadline = t.deadline;
        read.track_reads = t.track_reads;
        if (t.base->core->options.cache_bytes) {
            // Same allowance as query_index; near the pin limit use plain lookups.
            constexpr uint64_t allowance = 65536;
            auto &core = *t.base->core;
            try {
                if (core.options.pinned_bytes - core.pin_budget->used() >=
                    allowance + FQDB_MAX_KEY_BYTES + sizeof(Data)) {
                    path_charge = Charge(core.pin_budget, {}, allowance);
                    path.emplace(allowance);
                }
            } catch (const Error &e) {
                if (e.status != FQDB_LIMIT)
                    throw;
            }
        }
        for (unsigned i = 0; i <= p.spec.joins.size(); ++i) {
            memory.take(sizeof(Record) + 64);
            records[i] = std::make_unique<Record>(p, i, m, tx.base->core->pin_budget);
        }
        computed.resize(p.spec.columns.size());
        sort_values.resize(p.order.size());
        signature = p.signature;
        if (!after.empty())
            decode_cursor();
        auto epoch = tx.depend(catalog_epoch()).version;
        auto version = tx.base->view->version;
        bool cacheable = owner && tx.base->core->options.cache_bytes && !tx.writes;
        auto previous =
            std::atomic_load_explicit(&tx.base->core->select_plan_cache, std::memory_order_acquire);
        // 索引规则只取决于目录版本，与查询字面量无关：版本窗口内的缓存条目即使属于另一个
        // Program，也可直接复用其规则而不重读目录；整套计划仍要求是同一个 Program。
        bool rules_fresh = cacheable && previous && previous->epoch == epoch &&
                           version >= previous->version && version - previous->version < 256;
        bool reused = rules_fresh && previous->program.lock().get() == &p;
        const bool borrow_rules = rules_fresh && !reused;
        if (reused) {
            size_t cost = 2048;
            for (auto &r : previous->rules)
                cost += r.prefix.size() * 2 + r.name.size() * 2 +
                        r.parts.size() * sizeof(IndexPart) * 2 + 256;
            memory.take(cost);
            rules = previous->rules;
            prepared.resize(rules.size());
            for (auto &cached : previous->roots) {
                auto plan = std::make_unique<Plan>(m);
                plan->rule = cached.rule;
                plan->access = cached.access;
                plan->reverse = cached.reverse;
                for (auto &i : cached.intervals)
                    plan->add(i.begin, i.end);
                plans.push_back(std::move(plan));
            }
        } else {
            if (borrow_rules) {
                size_t cost = 0;
                for (auto &r : previous->rules)
                    cost += r.prefix.size() * 2 + r.name.size() * 2 +
                            r.parts.size() * sizeof(IndexPart) * 2 + 256;
                memory.take(cost);
                rules = previous->rules;
                prepared.resize(rules.size());
            } else
                load_rules();
            plans = root_plans(p, rules, tx, m, work, !explain_only);
        }
        for (auto &plan : plans) {
            require(explain_only || p.spec.allow_scan || !plan->access.needs_scan,
                    FQDB_UNSUPPORTED);
            access.push_back(plan->access);
        }
        for (unsigned i = 1; i <= p.spec.joins.size(); ++i) {
            memory.take(sizeof(JoinState) + 64);
            auto j = std::make_unique<JoinState>(m);
            j->plan = reused ? previous->joins[i - 1] : join_plan(p, i, rules);
            if (j->plan.access.needs_scan && !explain_only)
                require(p.spec.allow_scan, FQDB_UNSUPPORTED);
            access.push_back(j->plan.access);
            joins.push_back(std::move(j));
        }
        if (cacheable && !reused) {
            try {
                size_t cost = 4096;
                for (auto &r : rules)
                    cost += r.prefix.capacity() * 2 + r.name.capacity() * 2 +
                            r.parts.capacity() * sizeof(IndexPart) * 2 + 256;
                for (auto &plan : plans) {
                    cost += 512 + plan->access.reason.size() * 2;
                    for (auto &i : plan->intervals)
                        cost += i.begin.capacity() * 2 + i.end.capacity() * 2 +
                                sizeof(Interval) * 2 + 128;
                }
                cost += joins.size() * 1024;
                require(cost <= 1048576 && tx.base->core->options.pinned_bytes -
                                                   tx.base->core->pin_budget->used() >=
                                               cost + FQDB_MAX_VALUE_BYTES + 262144,
                        FQDB_LIMIT);
                auto entry =
                    std::make_shared<PlanCache>(Charge(tx.base->core->pin_budget, {}, cost));
                entry->program = *owner;
                entry->epoch = epoch;
                entry->version = version;
                entry->rules = rules;
                for (auto &plan : plans)
                    entry->roots.push_back(
                        {plan->rule, plan->intervals, plan->access, plan->reverse});
                for (auto &j : joins)
                    entry->joins.push_back(j->plan);
                std::shared_ptr<const PlanCache> immutable = entry;
                std::atomic_compare_exchange_strong_explicit(
                    &tx.base->core->select_plan_cache, &previous, immutable,
                    std::memory_order_acq_rel, std::memory_order_acquire);
            } catch (const Error &e) {
                if (e.status != FQDB_LIMIT)
                    throw;
            } catch (const std::bad_alloc &) {
            }
        }
        early = !p.aggregated && !p.spec.distinct && p.spec.joins.empty() && plans.size() == 1 &&
                plans[0]->access.ordered && !p.spec.count_total;
        if (p.spec.distinct)
            for (auto &o : p.spec.order)
                require(o.field.source == 255 ||
                            std::any_of(p.spec.columns.begin(), p.spec.columns.end(),
                                        [&](const auto &c) {
                                            return c.aggregate == QueryAggregate::None &&
                                                   c.field == o.field;
                                        }),
                        FQDB_INVALID_ARGUMENT);
    }
    void decode_cursor() {
        uint64_t seen_fields = 0, hash = 0;
        message(after, [&](auto &r, auto f) {
            if (f.number != 3)
                once(seen_fields, f.number);
            if (f.number == 1)
                require(f.type == wire::Type::Fixed64 && r.fixed(hash, 8), FQDB_INVALID_ARGUMENT);
            else if (f.number == 2)
                cursor_identity.assign(str(r, f));
            else if (f.number == 3) {
                require(cursor_sort.size() < p.order.size(), FQDB_INVALID_ARGUMENT);
                auto v = scalar(str(r, f));
                cursor_sort.emplace_back(memory, Atom::from(v));
            } else
                throw Error(FQDB_INVALID_ARGUMENT);
        });
        require(hash == signature && cursor_sort.size() == p.order.size() &&
                    (seen_fields & (UINT64_C(1) << 2)),
                FQDB_INVALID_ARGUMENT);
        has_cursor = true;
    }
    void load_rules() {
        auto prefix = catalog_prefix();
        tx.range(prefix, prefix_end(prefix));
        ReadCursor cursor(tx.base, prefix, prefix_end(prefix), false, tx.writes, true);
        rules.reserve(64);
        while (auto row = cursor.next()) {
            work.tick();
            require(rules.size() < 64, FQDB_LIMIT);
            memory.take(size_t(row->value.size) * 4 + sizeof(IndexRule) + 256);
            IndexRule rule(std::string_view(static_cast<const char *>(row->value.data),
                                            size_t(row->value.size)));
            tx.depend(Bytes(static_cast<const char *>(row->key.data), size_t(row->key.size)));
            if (rule.ready)
                rules.push_back(std::move(rule));
        }
        prepared.resize(rules.size());
    }
    void prepare(size_t rule) {
        if (!prepared[rule]) {
            work.check();
            prepare_index_rule(read, rules[rule], tx.writes);
            prepared[rule] = true;
            work.check();
        }
    }
    void source(Record &r, std::string_view name) {
        auto value = read.try_get(Bytes(name), nullptr, path ? &*path : nullptr);
        require(bool(value));
        ++work.stats.payload_reads;
        r.reset(name, std::move(value));
        r.parse(work);
    }
    Encoded identity() {
        if (p.spec.distinct)
            return key(
                memory, computed.size(), [&](size_t i) { return computed[i]; }, false, &work);
        Encoded out(memory);
        size_t size = 32;
        for (size_t i = 0; i <= p.spec.joins.size(); ++i) {
            auto r = eval.records[i];
            if (r)
                size += r->key.size() * 2 + 8;
        }
        out.hold.resize(size * 3);
        out.bytes.reserve(size);
        for (size_t i = 0; i <= p.spec.joins.size(); ++i) {
            auto r = eval.records[i];
            work.tick(r ? r->key.size() + 1 : 1);
            out.bytes.push_back(r ? '\1' : '\0');
            if (r)
                out.bytes += component(r->key);
        }
        return out;
    }
    bool better(const Output &a, const Output &b) {
        return row_compare(p, a, b, &work) < 0;
    }
    void keep(std::string_view row_key, std::string_view id) {
        for (size_t i = 0; i < p.order.size(); ++i)
            sort_values[i] = eval.get(p.order[i]);
        if (has_cursor && row_compare(
                              p, [&](size_t i) { return sort_values[i]; },
                              [&](size_t i) { return cursor_sort[i].ref(); }, id,
                              view(cursor_identity), &work) <= 0)
            return;
        if (p.spec.distinct && !unique.insert(text(memory, id)).second)
            return;
        ++work.stats.result_total;
        const auto maximum = p.spec.offset + p.spec.limit + 1;
        auto cmp = [&](const Output &a, const Output &b) { return better(a, b); };
        if (heap.size() == maximum && row_compare(
                                          p, [&](size_t i) { return sort_values[i]; },
                                          [&](size_t i) { return heap.front().sort[i].ref(); }, id,
                                          view(heap.front().identity), &work) >= 0)
            return;
        Output row(memory);
        row.key = text(memory, row_key);
        row.identity = text(memory, id);
        row.values.reserve(computed.size());
        for (auto &a : computed) {
            work.tick(a.kind == Kind::Bytes ? a.text.size() + 1 : 1);
            row.values.emplace_back(memory, a);
        }
        row.sort.reserve(sort_values.size());
        for (auto &a : sort_values)
            row.sort.emplace_back(memory, a);
        if (heap.size() == maximum) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = std::move(row);
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else {
            // 未达上限前不会淘汰任何行，也不读取堆顶：先线性追加，恰好填满时一次建堆。
            heap.push_back(std::move(row));
            if (heap.size() == maximum)
                std::make_heap(heap.begin(), heap.end(), cmp);
        }
        if (early && heap.size() == maximum) {
            stop = true;
            work.stats.complete = false;
        }
    }
    Group &group(Encoded &encoded) {
        auto result = groups.try_emplace(text(memory, encoded.bytes), memory);
        auto &g = result.first->second;
        if (result.second) {
            g.keys.reserve(p.group.size());
            for (auto id : p.group)
                g.keys.emplace_back(memory, eval.get(id));
            g.reducers.reserve(p.columns.size());
            for (size_t i = 0; i < p.columns.size(); ++i)
                g.reducers.emplace_back(memory);
        }
        return g;
    }
    Group &current_group() {
        // unordered_map preserves element references across rehash. Group keys
        // own their bytes, so this cache never borrows the current record.
        if (last_group) {
            bool same = true;
            for (size_t i = 0; i < p.group.size(); ++i) {
                auto a = eval.get(p.group[i]), b = last_group->keys[i].ref();
                work.tick(a.kind == Kind::Bytes && b.kind == Kind::Bytes
                              ? std::min(a.text.size(), b.text.size()) + 1
                              : 1);
                if (atom_compare(a, b)) {
                    same = false;
                    break;
                }
            }
            if (same)
                return *last_group;
        }
        auto encoded = key(
            memory, p.group.size(), [&](size_t i) { return eval.get(p.group[i]); }, false, &work);
        last_group = &group(encoded);
        return *last_group;
    }
    void accept() {
        if (eval.run(p.where) != Truth::True)
            return;
        ++work.stats.matched;
        if (p.aggregated) {
            auto &g = current_group();
            for (size_t i = 0; i < p.columns.size(); ++i) {
                auto &column = p.spec.columns[i];
                if (column.aggregate != QueryAggregate::None)
                    g.reducers[i].add(column,
                                      column.aggregate == QueryAggregate::CountAll
                                          ? Atom{}
                                          : eval.get(p.columns[i]),
                                      memory, work);
            }
            return;
        }
        for (size_t i = 0; i < p.columns.size(); ++i)
            computed[i] = eval.get(p.columns[i]);
        eval.computed = &computed;
        auto id = identity();
        keep(p.spec.distinct ? std::string_view{} : eval.records[0]->key, id.bytes);
        eval.computed = nullptr;
    }
    bool equality(unsigned j) {
        for (size_t i = 0; i < p.left[j].size(); ++i) {
            auto a = eval.get(p.left[j][i]), b = eval.get(p.right[j][i]);
            work.tick(a.kind == Kind::Bytes && b.kind == Kind::Bytes
                          ? std::min(a.text.size(), b.text.size()) + 1
                          : 1);
            if (a.kind == Kind::Null || b.kind == Kind::Null || atom_compare(a, b))
                return false;
        }
        return true;
    }
    void build_hash(unsigned j) {
        auto &join = *joins[j];
        if (join.built)
            return;
        join.built = true;
        auto source_id = j + 1;
        auto &record = *records[source_id];
        auto &spec = p.spec.joins[j];
        auto end = prefix_end(spec.prefix);
        tx.range(spec.prefix, end);
        ReadCursor cursor(read.base, spec.prefix, end, false, read.writes, false, true);
        while (auto row = cursor.next(p.payload[source_id])) {
            work.candidate();
            auto name =
                std::string_view(static_cast<const char *>(row->key.data), size_t(row->key.size));
            record.reset(name, cursor.value_ref(), true);
            if (p.payload[source_id]) {
                ++work.stats.payload_reads;
                record.parse(work);
            }
            eval.records[source_id] = &record;
            auto encoded = key(
                memory, p.right[j].size(), [&](size_t i) { return eval.get(p.right[j][i]); }, true,
                &work);
            if (encoded.bytes.empty())
                continue;
            auto found =
                join.table.try_emplace(text(memory, encoded.bytes), Alloc<StoredRecord>(memory));
            found.first->second.push_back(record.own(memory, work));
        }
        eval.records[source_id] = nullptr;
        record.reset({}, {});
    }
    void walk(unsigned j) {
        if (stop)
            return;
        if (j == joins.size()) {
            accept();
            return;
        }
        work.tick();
        auto &state = *joins[j];
        auto &spec = p.spec.joins[j];
        unsigned source_id = j + 1;
        auto &record = *records[source_id];
        bool matched = false;
        auto consume = [&]() {
            eval.records[source_id] = &record;
            if (!equality(j) || eval.run(p.on[j]) != Truth::True)
                return false;
            matched = true;
            if (spec.kind == QueryJoinKind::Semi) {
                eval.records[source_id] = nullptr;
                walk(j + 1);
                return true;
            }
            if (spec.kind == QueryJoinKind::Anti)
                return true;
            walk(j + 1);
            return stop;
        };
        ++work.stats.join_probes;
        if (state.plan.primary) {
            auto atom = eval.get(p.left[j][0]);
            if (atom.kind == Kind::Bytes && begins(atom.text, spec.prefix) &&
                !system_key(atom.text)) {
                auto value = read.try_get(Bytes(atom.text), nullptr, path ? &*path : nullptr);
                if (value) {
                    ++work.stats.payload_reads;
                    record.reset(atom.text, std::move(value));
                    record.parse(work);
                    consume();
                }
            }
        } else if (state.plan.rule) {
            auto i = *state.plan.rule;
            prepare(i);
            auto &rule = rules[i];
            Bytes begin = index_root(rule);
            bool valid = true;
            for (size_t k = 0; k < state.plan.bindings.size(); ++k) {
                auto a = eval.get(p.left[j][state.plan.bindings[k]]);
                if (a.kind == Kind::Null) {
                    valid = false;
                    break;
                }
                auto part = index_atom(rule.part(k).type, a);
                if (!part) {
                    valid = false;
                    break;
                }
                begin += component(*part);
            }
            if (valid) {
                auto end = prefix_end(begin);
                ReadCursor cursor(read.base, begin, end, false, read.writes, true);
                CoverScratch scratch(memory);
                while (auto row = cursor.next()) {
                    work.candidate();
                    auto name = std::string_view(static_cast<const char *>(row->value.data),
                                                 size_t(row->value.size));
                    if (!begins(name, spec.prefix))
                        continue;
                    if (state.plan.access.covering)
                        scratch.fill(record, rule,
                                     std::string_view(static_cast<const char *>(row->key.data),
                                                      size_t(row->key.size)),
                                     name);
                    else
                        source(record, name);
                    bool exit = consume();
                    record.reset({}, {});
                    if (exit)
                        break;
                }
            }
        } else {
            build_hash(j);
            auto encoded = key(
                memory, p.left[j].size(), [&](size_t i) { return eval.get(p.left[j][i]); }, true,
                &work);
            if (!encoded.bytes.empty()) {
                auto it = state.table.find(text(memory, encoded.bytes));
                if (it != state.table.end())
                    for (auto &row : it->second) {
                        record.reset(view(row.key), {});
                        record.stored = &row;
                        if (consume())
                            break;
                    }
            }
        }
        eval.records[source_id] = nullptr;
        record.reset({}, {});
        if (!stop && ((spec.kind == QueryJoinKind::Left && !matched) ||
                      (spec.kind == QueryJoinKind::Anti && !matched)))
            walk(j + 1);
    }
    void dependencies() {
        for (auto &j : joins)
            if (j->plan.rule) {
                auto &rule = rules[*j->plan.rule];
                auto root = index_root(rule);
                tx.range(root, prefix_end(root));
            }
    }
    void execute() {
        dependencies();
        if (p.aggregated && p.group.empty()) {
            auto encoded = key(memory, 0, [](size_t) { return Atom{}; });
            last_group = &group(encoded);
        }
        auto &record = *records[0];
        bool union_scan = plans.size() > 1;
        CoverScratch scratch(memory);
        for (auto &plan : plans) {
            if (stop)
                break;
            if (plan->rule)
                prepare(*plan->rule);
            for (auto &interval : plan->intervals) {
                if (stop)
                    break;
                tx.range(interval.begin, interval.end);
                if (plan->access.strategy == "index_filter_scan") {
                    // 窄索引过滤扫描：不读业务值，在索引键上判断条件，命中后才回主表。
                    const auto &rule = rules[*plan->rule];
                    const auto root = index_root(rule);
                    ReadCursor cursor(read.base, interval.begin, interval.end, false,
                                      read.writes, true, true);
                    while (!stop) {
                        auto row = cursor.next(false);
                        if (!row)
                            break;
                        work.candidate();
                        auto owner = uncomponent(scratch.decode(
                            rule, root,
                            std::string_view(static_cast<const char *>(row->key.data),
                                             size_t(row->key.size))));
                        if (!begins(owner, p.spec.prefix) || system_key(owner))
                            continue;
                        scratch.assign(record, rule, owner);
                        eval.records[0] = &record;
                        eval.next_root();
                        if (eval.run(p.where) != Truth::True) {
                            record.reset({}, {});
                            continue;
                        }
                        if (!plan->access.covering)
                            source(record, owner);
                        eval.next_root();
                        walk(0);
                        record.reset({}, {});
                    }
                    continue;
                }
                auto begin = interval.begin, end = interval.end;
                if (has_cursor && p.spec.joins.empty() && !p.aggregated && !p.spec.distinct &&
                    plans.size() == 1 && plan->access.ordered) {
                    require(!cursor_identity.empty() && cursor_identity[0] == '\1',
                            FQDB_INVALID_ARGUMENT);
                    Bytes owner;
                    try {
                        owner = uncomponent(view(cursor_identity).substr(1));
                    } catch (const Error &) {
                        throw Error(FQDB_INVALID_ARGUMENT);
                    }
                    Bytes position = owner;
                    if (plan->rule) {
                        const auto &r = rules[*plan->rule];
                        auto clauses = dnf(p.spec.where);
                        auto c = constraints(clauses.empty() ? Clause{} : clauses[0]);
                        position = index_root(r);
                        for (size_t part = 0; part < r.count(); ++part) {
                            auto f = r.part(part);
                            std::optional<Atom> value;
                            auto fixed = c.find(f.field);
                            if (fixed != c.end() && fixed->second.equal &&
                                fixed->second.equal->size() == 1)
                                value = (*fixed->second.equal)[0];
                            else
                                for (size_t k = 0; k < p.spec.order.size(); ++k)
                                    if (p.spec.order[k].field == Field::field(f.field, f.type))
                                        value = cursor_sort[k].ref();
                            require(value.has_value(), FQDB_INVALID_ARGUMENT);
                            auto encoded = index_atom(f.type, *value);
                            require(encoded.has_value(), FQDB_INVALID_ARGUMENT);
                            position += component(*encoded);
                        }
                        if (!(r.flags & 1))
                            position += component(owner);
                    }
                    if (plan->reverse) {
                        if (end.empty() || compare(position, end) < 0)
                            end = std::move(position);
                    } else if (compare(position, begin) > 0)
                        begin = std::move(position);
                    if (!end.empty() && compare(begin, end) > 0)
                        continue;
                }
                ReadCursor cursor(read.base, begin, end, plan->reverse, read.writes,
                                  plan->rule.has_value(), true);
                // 非唯一复合索引的键尾就是 component(主键)，条目的值只是同一主键的副本：
                // 直接从键解出主键，省掉每行一次缓存取值。唯一索引的键不含主键，仍需取值。
                const IndexRule *tail_rule = nullptr;
                Bytes tail_root;
                if (plan->rule && !rules[*plan->rule].parts.empty() &&
                    !(rules[*plan->rule].flags & 1)) {
                    tail_rule = &rules[*plan->rule];
                    tail_root = index_root(*tail_rule);
                }
                while (!stop) {
                    auto row = cursor.next(!tail_rule && (plan->rule.has_value() || p.payload[0]));
                    if (!row)
                        break;
                    work.candidate();
                    const std::string_view index_key(static_cast<const char *>(row->key.data),
                                                     size_t(row->key.size));
                    Bytes tail_owner;
                    std::string_view name;
                    if (tail_rule) {
                        tail_owner = uncomponent(scratch.decode(*tail_rule, tail_root, index_key));
                        name = tail_owner;
                    } else if (plan->rule)
                        name = std::string_view(static_cast<const char *>(row->value.data),
                                                size_t(row->value.size));
                    else
                        name = index_key;
                    if (!begins(name, p.spec.prefix) || system_key(name))
                        continue;
                    if (union_scan && !seen.insert(text(memory, name)).second)
                        continue;
                    if (plan->rule) {
                        if (plan->access.covering) {
                            if (tail_rule)
                                scratch.assign(record, *tail_rule, name);
                            else
                                scratch.fill(record, rules[*plan->rule], index_key, name);
                        } else
                            source(record, name);
                    } else {
                        record.reset(name, cursor.value_ref(), true);
                        if (p.payload[0]) {
                            ++work.stats.payload_reads;
                            record.parse(work);
                        }
                    }
                    eval.records[0] = &record;
                    eval.next_root();
                    if (p.spec.joins.empty() || eval.root_filter(p.where))
                        walk(0);
                    record.reset({}, {});
                }
            }
        }
        eval.records.fill(nullptr);
        if (p.aggregated) {
            for (auto &entry : groups) {
                work.tick();
                auto &g = entry.second;
                eval.group_values = &g.keys;
                for (size_t i = 0; i < p.columns.size(); ++i) {
                    auto &column = p.spec.columns[i];
                    computed[i] = column.aggregate == QueryAggregate::None
                                      ? eval.get(p.columns[i])
                                      : g.reducers[i].result(column.aggregate);
                }
                eval.computed = &computed;
                if (eval.run(p.having) == Truth::True) {
                    auto distinct_key =
                        p.spec.distinct ? key(
                                              memory, computed.size(),
                                              [&](size_t i) { return computed[i]; }, false, &work)
                                        : Encoded(memory);
                    auto id =
                        p.spec.distinct ? std::string_view(distinct_key.bytes) : view(entry.first);
                    keep(p.spec.distinct ? std::string_view{} : view(entry.first), id);
                }
            }
            eval.group_values = nullptr;
            eval.computed = nullptr;
        }
        tx.reads.merge(read.reads);
        work.check();
    }
    Bytes cursor(const Output &row) {
        wire::Writer w;
        w.fixed64_field(1, signature);
        w.bytes_field(2, view(row.identity));
        for (auto &v : row.sort)
            atom_message(w, 3, v.ref());
        require(w.data().size() + p.request.size() + 16 <= 262144, FQDB_LIMIT);
        return w.take();
    }
    Bytes encode(bool explain_only) {
        if (!explain_only)
            execute();
        std::sort(heap.begin(), heap.end(),
                  [&](const Output &a, const Output &b) { return better(a, b); });
        size_t first = std::min<size_t>(heap.size(), size_t(p.spec.offset)),
               last = std::min<size_t>(heap.size(), first + size_t(p.spec.limit));
        bool more = heap.size() > last;
        size_t size = 2048, max_row = 0;
        for (auto &a : access)
            size += a.strategy.size() + a.index.size() + a.reason.size() + 128;
        for (size_t i = first; i < last; ++i) {
            size_t row_size = heap[i].key.size() + 64;
            for (auto &v : heap[i].values)
                row_size += atom_size(v.ref()) + 16;
            size += row_size;
            max_row = std::max(max_row, row_size);
        }
        size_t token = 0;
        if (more && last > first) {
            token = heap[last - 1].identity.size() + 64;
            for (auto &v : heap[last - 1].sort)
                token += atom_size(v.ref()) + 16;
        }
        require(size + token <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
        Hold output(memory);
        output.resize((size + token) * 2 + max_row * 4);
        work.tick(size + token);
        wire::Writer w;
        w.reserve(size + token);
        for (size_t i = first; i < last; ++i) {
            wire::Writer row;
            row.bytes_field(1, view(heap[i].key));
            for (auto &v : heap[i].values)
                atom_message(row, 2, v.ref());
            w.bytes_field(1, row.data());
        }
        for (auto &a : access) {
            wire::Writer v;
            v.uint_field(1, a.source);
            v.bytes_field(2, a.strategy);
            v.bytes_field(3, a.index);
            v.bytes_field(4, a.reason);
            v.uint_field(5, a.covering);
            v.uint_field(6, a.ordered);
            v.uint_field(7, a.needs_scan);
            v.uint_field(8, a.sampled_candidates);
            w.bytes_field(2, v.data());
        }
        work.stats.peak_memory = memory.peak;
        wire::Writer stats;
        stats.uint_field(1, work.stats.candidates);
        stats.uint_field(2, work.stats.matched);
        stats.uint_field(3, work.stats.result_total);
        stats.uint_field(4, work.stats.work);
        stats.uint_field(5, work.stats.peak_memory);
        stats.uint_field(6, work.stats.payload_reads);
        stats.uint_field(7, work.stats.join_probes);
        stats.uint_field(8, work.stats.complete);
        w.bytes_field(3, stats.data());
        if (more && last > first)
            w.bytes_field(4, cursor(heap[last - 1]));
        work.check();
        return w.take();
    }
};
} // namespace fengqudb::paged::select
