#include "store_test_support.hpp"
#include "paged/index_build.hpp"
#include "paged/operations.hpp"
#include "paged/query.hpp"

static p::Bytes document(uint64_t id, uint64_t count, size_t size = 4096) {
    fengqudb::wire::Writer out;
    out.uint_field(1, id);
    out.fixed64_field(2, count);
    out.bytes_field(3, p::Bytes(size, 'p'));
    return out.take();
}
static auto open_core(const std::string &path, bool segmented, bool reliable, bool cache = true) {
    auto options = p::defaults();
    options.flags |= segmented ? FQDB_STORE_SEGMENTED : 0;
    if (!cache)
        options.cache_bytes = 0;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    auto policy = core->get_write_policy();
    policy.mode = reliable ? FQDB_WRITE_RELIABLE : FQDB_WRITE_PERFORMANCE;
    core->configure_write(policy);
    return core;
}
static p::IndexRule rule(unsigned field = 1) {
    p::IndexRule value;
    value.name = "by-id";
    value.prefix = "d/";
    value.field = field;
    value.type = field == 1 ? 1 : 7;
    value.flags = 3;
    return value;
}
static p::Value get(const std::shared_ptr<p::Core> &core, const p::Bytes &key) {
    auto view = core->current();
    auto ref = view->find(key);
    check(ref && !ref->deleted(), "record missing");
    return view->generation->value(*ref, view->limit);
}
static auto lookup(p::Transaction &tx, unsigned field, uint64_t id) {
    fengqudb::wire::Writer value;
    if (field == 1)
        value.uint_field(1, id);
    else
        value.fixed64_field(2, id);
    auto encoded = fengqudb::StoreCommand{FQDB_INDEX_LOOKUP, "by-id", {}, value.data()}.encode();
    p::Charge charge;
    auto result = p::query_index(tx, p::FeatureRequest(encoded), charge);
    check(result.rows.size() == 1, "unique lookup lost owner");
    return result.rows[0].key;
}
static auto lookup(const std::shared_ptr<p::Core> &core, unsigned field, uint64_t id) {
    p::Transaction tx(core, 0);
    return lookup(tx, field, id);
}
static void put_atomic(const std::shared_ptr<p::Core> &core, const p::Bytes &key,
                       const p::Bytes &bytes) {
    p::AtomicUpdate update(core, key, p::AtomicKind::put, 0);
    update.value = bytes;
    update.commit(nullptr);
}
static void transfers_and_aliases(bool segmented, bool reliable) {
    Fixture fixture;
    auto path = fixture.name("ownership");
    auto core = open_core(path, segmented, reliable);
    p::Transaction seed(core, 0);
    seed.put("d/a", document(1, 10), 0);
    seed.put("d/b", document(2, 20), 0);
    core->commit(seed);
    auto maintenance = fengqudb::StoreMaintenanceOptions{}.native();
    p::build_index(*core, rule(), false, maintenance);
    auto before = std::make_shared<p::Capture>(core);
    auto old_value = get(core, "d/a");
    auto moves = p::cache_buffer_transfers.load();
    put_atomic(core, "d/a", document(1, 11));
    check(p::cache_buffer_transfers.load() > moves, "indexed automatic put did not transfer");
    check(*old_value == document(1, 10) && *get(core, "d/a") == document(1, 11),
          "automatic transfer changed borrowed old value");
    check(core->transaction_budget->used() == 0, "cache retained transaction charges");
    p::Transaction old(before, 0);
    check(*old.get("d/a") == document(1, 10), "snapshot changed after transfer");
    const auto retained_private = core->transaction_budget->used();

    moves = p::cache_buffer_transfers.load();
    p::FieldUpdates fields(core, 128, 0);
    fields.items.push_back({"d/a", 3, -1, 0, INT64_MAX});
    fields.items.push_back({"d/b", 3, 1, 0, INT64_MAX});
    fields.commit(nullptr, reliable);
    check(p::cache_buffer_transfers.load() >= moves + 2 && *get(core, "d/a") == document(1, 10) &&
              *get(core, "d/b") == document(2, 21),
          "indexed field command lost atomic values");
    check(core->transaction_budget->used() == retained_private,
          "field transfer retained private budget");

    auto private_bytes = document(1, 12);
    p::Transaction explicit_tx(core, 0);
    explicit_tx.put("d/a", private_bytes, 0);
    explicit_tx.savepoint();
    p::ReadCursor cursor(explicit_tx.base, "d/a", "d/b", false, explicit_tx.writes);
    moves = p::cache_buffer_transfers.load();
    core->commit(explicit_tx);
    check(p::cache_buffer_transfers.load() == moves, "user write tree was consumed");
    auto row = cursor.next();
    check(row && std::string_view(static_cast<const char *>(row->value.data), row->value.size) ==
                     private_bytes,
          "cursor's indirectly shared buffer was moved");
    put_atomic(core, "d/a", document(1, 13));
    check(std::string_view(static_cast<const char *>(row->value.data), row->value.size) ==
              private_bytes,
          "subsequent write changed cursor lease");

    auto batch = std::make_shared<p::PreparedBatch>(*core);
    auto budget = std::make_shared<p::Budget>(1 << 20);
    batch->items.push_back(
        {"d/a", p::staged_data(document(1, 14), budget, core->transaction_budget), 0});
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        p::Transaction tx(core, 0);
        batch->apply(tx, batch);
        moves = p::cache_buffer_transfers.load();
        core->commit(tx);
        check(*batch->items[0].value == document(1, 14) &&
                  p::cache_buffer_transfers.load() == moves,
              "reusable batch was consumed");
        put_atomic(core, "d/a", document(1, 15 + repeat));
    }
    auto large = document(1, 30, 20000);
    moves = p::cache_buffer_transfers.load();
    put_atomic(core, "d/a", large);
    check(p::cache_buffer_transfers.load() == moves && *get(core, "d/a") == large,
          "large potentially borrowed payload was moved");
    p::AtomicUpdate shrink(core, "d/a", p::AtomicKind::patch, 0);
    auto small = document(1, 31);
    shrink.value = small;
    shrink.remove = large.size();
    shrink.commit(nullptr);
    check(p::cache_buffer_transfers.load() == moves && *get(core, "d/a") == small,
          "subtraction retained oversized cache buffer");
    core->sync();
    core->verify(maintenance);
    // The live old captures intentionally remain valid through all operations.
    check(*old.get("d/a") == document(1, 10), "old snapshot after large write/shrink");
}
static void query_cache() {
    Fixture fixture;
    auto path = fixture.name("query");
    auto core = open_core(path, false, false);
    p::Transaction seed(core, 0);
    seed.put("d/a", document(1, 10), 0);
    seed.put("d/b", document(2, 20), 0);
    core->commit(seed);
    auto maintenance = fengqudb::StoreMaintenanceOptions{}.native();
    p::build_index(*core, rule(), false, maintenance);
    check(lookup(core, 1, 1) == "d/a", "initial lookup");
    auto cached = std::atomic_load(&core->query_rule_cache);
    check(bool(cached) && cached->charge.bytes() == p::QueryRuleCache::cost(cached->rule),
          "rule cache was not charged");
    check(lookup(core, 1, 2) == "d/b" && std::atomic_load(&core->query_rule_cache) == cached,
          "same schema did not reuse rule");
    p::Transaction old(core, 0);
    p::build_index(*core, rule(), true, maintenance);
    p::build_index(*core, rule(2), false, maintenance);
    check(lookup(core, 2, 20) == "d/b" && lookup(old, 1, 1) == "d/a" &&
              lookup(core, 2, 10) == "d/a",
          "same-name replacement or old snapshot used stale rule");

    auto schema_key = p::catalog_prefix() + p::component("by-id");
    p::Transaction private_tx(core, 0);
    auto private_rule = rule(2);
    private_rule.ready = 1;
    auto encoded = private_rule.encode();
    private_tx.put(schema_key, encoded, 0);
    auto shared_cache = std::atomic_load(&core->query_rule_cache);
    {
        auto value = private_tx.get(schema_key);
        p::QueryRule selection(private_tx, schema_key, value);
        check(selection.get().field == 2, "private schema parse");
    }
    private_rule.field = 1;
    private_rule.type = 1;
    private_tx.patch(schema_key, 0, encoded.size(), private_rule.encode());
    {
        auto value = private_tx.get(schema_key);
        p::QueryRule selection(private_tx, schema_key, value);
        check(selection.get().field == 1, "private schema edit reused old rule");
    }
    check(std::atomic_load(&core->query_rule_cache) == shared_cache,
          "private schema replaced shared plan");
    shared_cache.reset();
    cached.reset();
    std::atomic_store(&core->query_rule_cache, std::shared_ptr<const p::QueryRuleCache>{});
    {
        p::Transaction read(core, 0);
        p::Charge occupy(core->pin_budget, {},
                         core->options.pinned_bytes - core->pin_budget->used());
        check(lookup(read, 2, 10) == "d/a" && !std::atomic_load(&core->query_rule_cache),
              "optional cache budget rejection failed query");
    }
    std::vector<std::future<void>> readers;
    for (unsigned i = 0; i < 4; ++i)
        readers.push_back(std::async(std::launch::async, [core] {
            for (unsigned j = 0; j < 100; ++j)
                check(lookup(core, 2, 10) == "d/a", "concurrent cached lookup");
        }));
    for (auto &reader : readers)
        reader.get();
    auto retained = std::atomic_load(&core->query_rule_cache);
    auto used = core->pin_budget->used();
    auto cost = retained->charge.bytes();
    std::atomic_store(&core->query_rule_cache, std::shared_ptr<const p::QueryRuleCache>{});
    retained.reset();
    check(core->pin_budget->used() == used - cost, "rule cache did not release charge");
    core->sync();
    // Independent Core, cache disabled: identical result without a memo entry.
    auto other = open_core(fixture.name("disabled"), false, true, false);
    p::Transaction other_seed(other, 0);
    other_seed.put("d/a", document(7, 70), 0);
    other->commit(other_seed);
    p::build_index(*other, rule(), false, maintenance);
    check(lookup(other, 1, 7) == "d/a" && !std::atomic_load(&other->query_rule_cache),
          "disabled cache allocated a rule or crossed databases");
}
static void current_view_validation() {
    Fixture fixture;
    auto core = open_core(fixture.name("current-view"), false, false);
    p::Transaction seed(core, 0);
    seed.put("base", "one", 0);
    core->commit(seed);
    p::Transaction unchanged(core, 0);
    check(*unchanged.get("base") == "one", "current view read");
    unchanged.range("a", "c");
    unchanged.put("base", "two", 0);
    core->commit(unchanged);
    p::Transaction stale(core, 0);
    check(!stale.try_get("new"), "missing dependency fixture");
    put_atomic(core, "new", "created");
    stale.put("side", "x", 0);
    bool conflict = false;
    try {
        core->commit(stale);
    } catch (const p::Error &e) {
        conflict = e.status == FQDB_CONFLICT;
    }
    check(conflict, "different-view missing read conflict skipped");
    p::Transaction range(core, 0);
    range.range("k", "l");
    put_atomic(core, "k/1", "phantom");
    range.put("side", "x", 0);
    conflict = false;
    try {
        core->commit(range);
    } catch (const p::Error &e) {
        conflict = e.status == FQDB_CONFLICT;
    }
    check(conflict, "different-view range conflict skipped");
    p::Transaction unrelated(core, 0);
    check(*unrelated.get("base") == "two", "unrelated dependency fixture");
    put_atomic(core, "elsewhere", "changed");
    unrelated.put("base", "three", 0);
    core->commit(unrelated);
    check(*get(core, "base") == "three", "unrelated publication incorrectly rejected");
    core->sync();
}
int main() {
    try {
        for (bool segmented : {false, true})
            for (bool reliable : {false, true})
                transfers_and_aliases(segmented, reliable);
        query_cache();
        current_view_validation();
        std::cout
            << "PASS indexed cache transfer, aliases, capacity, charged rule reuse and snapshots\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
