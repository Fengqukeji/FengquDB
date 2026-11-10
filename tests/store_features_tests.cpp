#include "strict_store.hpp"
#include "store_test_support.hpp"
#include "paged/features.hpp"
#include <atomic>
#include <map>
#include <set>
#include <deque>
#include <random>
#include <fstream>

using fengqudb::StoreCommand;
template <class F> static fqdb_status native_status(F &&operation) {
    try {
        operation();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    }
}
static void contiguous_list_ranges() {
    Fixture fixture;
    auto path = fixture.name("list-ranges");
    {
        Store db(path);
        auto seed = db.transaction();
        for (unsigned i = 0; i < 80; ++i)
            seed.execute({FQDB_LIST_PUSH_RIGHT, "list", {}, std::to_string(i)});
        for (unsigned i = 0; i < 16; ++i)
            seed.execute({FQDB_LIST_PUSH_RIGHT, "big", {}, std::string(131072, 'v')});
        commit(seed);
        db.checkpoint();
        auto read = [&](int64_t start, uint64_t limit) {
            return db.execute({FQDB_LIST_RANGE, "list", {}, {}, start, 0, limit});
        };
        for (auto start : {0, 7, -64, -16, -1}) {
            auto rows = read(start, 64);
            const auto first = start < 0 ? 80 + start : start;
            check(rows.count == 80 && rows.rows.size() == std::min(64, 80 - first),
                  "list range count/tail clipping");
            for (size_t i = 0; i < rows.rows.size(); ++i)
                check(rows.rows[i].value == std::to_string(first + i), "list range order");
        }
        auto tx = db.transaction();
        auto mark = tx.savepoint();
        tx.execute({FQDB_LIST_SET, "list", {}, "private", 10});
        tx.execute({FQDB_LIST_PUSH_LEFT, "list", {}, "head"});
        auto held = tx.execute({FQDB_LIST_RANGE, "list", {}, {}, 0, 0, 64});
        check(held.rows[0].value == "head" && held.rows[11].value == "private",
              "list range omitted private writes");
        tx.execute({FQDB_LIST_POP_LEFT, "list"});
        tx.rollback_to(mark);
        check(tx.execute({FQDB_LIST_RANGE, "list", {}, {}, 0, 0, 64}).rows[10].value == "10" &&
                  held.rows[11].value == "private",
              "list range result/savepoint ownership");
        commit(tx);
        auto dependent = db.transaction();
        dependent.execute({FQDB_LIST_RANGE, "list", {}, {}, 8, 0, 32});
        db.execute({FQDB_LIST_SET, "list", {}, "concurrent", 20});
        dependent.put("derived", "must conflict");
        check(dependent.commit().status == fengqudb::Status::conflict,
              "list range lost in-range update dependency");
        auto separate = db.transaction();
        separate.execute({FQDB_LIST_RANGE, "list", {}, {}, 8, 0, 32});
        db.execute({FQDB_LIST_SET, "list", {}, "outside", 70});
        separate.put("derived", "accepted");
        commit(separate);
        db.verify();
    }
    auto opts = p::defaults();
    auto core = std::make_shared<p::Core>(path, opts);
    core->open();
    core->write_policy.mode = false;
    auto execute = [](p::Transaction &tx, const StoreCommand &command) {
        auto encoded = command.encode();
        return p::execute_feature(tx, p::FeatureRequest(encoded));
    };
    {
        p::Transaction tx(core, 0);
        execute(tx, {FQDB_LIST_SET, "list", {}, "private", 10});
        auto held = execute(tx, {FQDB_LIST_RANGE, "list", {}, {}, 0, 0, 64});
        auto key = p::system_prefix() + "C" + p::component("list") + "L" +
                   p::big_endian((UINT64_C(1) << 63) + 10);
        tx.patch(key, 0, 7, "changed");
        check(p::feature_bytes(held.rows[10].value) == "private" && *tx.get(key) == "changed",
              "borrowed list range result changed after private binary patch");
    }
    {
        p::Transaction tx(core, 0);
        const auto pins = core->pin_budget->used();
        // The snapshot already exists; prevent even the transient scan's base
        // reservation. Point reads must still succeed as before the fast path.
        p::Charge fill(core->pin_budget, {}, opts.pinned_bytes - pins - 1);
        auto rows = execute(tx, {FQDB_LIST_RANGE, "list", {}, {}, 0, 0, 64});
        check(rows.rows.size() == 64 && tx.ranges.empty() && tx.reads.size() >= 65,
              "list range failed to fall back on tight pin budget");
    }
    {
        p::Transaction tx(core, 0);
        p::Charge fill(core->pin_budget, {}, opts.pinned_bytes - core->pin_budget->used() - 65536);
        auto rows = execute(tx, {FQDB_LIST_RANGE, "big", {}, {}, 0, 0, 16});
        check(rows.rows.size() == 16 && p::feature_bytes(rows.rows[0].value).size() == 131072 &&
                  tx.ranges.empty(),
              "large list payload failed to fall back after cursor creation");
    }
    {
        p::Transaction tx(core, 0);
        for (unsigned i = 0; i < 1024; ++i)
            tx.range("a", "b");
        auto rows = execute(tx, {FQDB_LIST_RANGE, "list", {}, {}, 0, 0, 64});
        check(rows.rows.size() == 64 && tx.ranges.size() == 1024 && tx.reads.size() >= 65,
              "list range failed to fall back on exhausted range slots");
    }
    {
        p::Transaction tx(core, 0);
        auto prefix = p::system_prefix() + "C" + p::component("list") + "L";
        tx.erase(prefix + p::big_endian((UINT64_C(1) << 63) + 30));
        check(native_status([&] { execute(tx, {FQDB_LIST_RANGE, "list", {}, {}, 0, 0, 64}); }) ==
                  FQDB_CORRUPT,
              "list range silently skipped a corrupt hole");
    }
}

static void cursor_pin_lifecycle() {
    Fixture fixture;
    auto core = std::make_shared<p::Core>(fixture.name("cursor-pins"), p::defaults());
    core->open();
    core->write_policy.mode = false;
    {
        p::Transaction tx(core, 0);
        tx.put("a", std::string(4096, 'a'), 0);
        tx.put("b", std::string(4096, 'b'), 0);
        tx.put("c", std::string(131072, 'c'), 0);
        core->commit(tx);
    }
    auto capture = std::make_shared<p::Capture>(core);
    const auto before = core->pin_budget->used();
    {
        p::ReadCursor cursor(capture, "a", "z", false);
        const auto base = core->pin_budget->used();
        check(cursor.next().has_value(), "cursor first row");
        const auto small = core->pin_budget->used();
        check(small > base && cursor.next().has_value() && core->pin_budget->used() == small,
              "equal-size cursor pins changed");
        check(cursor.next().has_value() && core->pin_budget->used() > small,
              "large cursor payload not charged");
        check(!cursor.next() && core->pin_budget->used() == base && !cursor.next(),
              "EOF retained cursor payload pins");
    }
    check(core->pin_budget->used() == before, "cursor leaked pins on destruction");
    {
        p::ReadCursor cursor(capture, "a", "b", false);
        const auto base = core->pin_budget->used();
        check(cursor.next().has_value() && !cursor.next() && core->pin_budget->used() == base,
              "range-bound EOF retained payload pins");
    }
    {
        p::ReadCursor cursor(capture, "a", "z", false);
        const auto base = core->pin_budget->used();
        check(cursor.next().has_value() && cursor.next().has_value(), "cursor before error");
        const auto retained = core->pin_budget->used() - base;
        p::Charge fill(core->pin_budget, {},
                       core->options.pinned_bytes - core->pin_budget->used() - 8192);
        const auto filled = core->pin_budget->used();
        check(native_status([&] { cursor.next(); }) == FQDB_LIMIT &&
                  core->pin_budget->used() == filled - retained &&
                  native_status([&] { cursor.next(); }) == FQDB_INVALID_ARGUMENT,
              "failed cursor retained pins or continued after error");
    }
    check(core->pin_budget->used() == before, "failed cursor leaked pins");
}

static void retained_result_ownership() {
    Fixture fixture;
    auto core = std::make_shared<p::Core>(fixture.name("result-ownership"), p::defaults());
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    p::Transaction tx(core, 0);
    auto execute = [&](uint32_t op, std::string_view key = {}, std::string_view value = {}) {
        auto request = StoreCommand{op, "retained", key, value}.encode();
        return p::execute_feature(tx, p::FeatureRequest(request));
    };
    auto decode = [](const p::FeatureResult &result) {
        auto buffer = result.encode();
        std::unique_ptr<void, decltype(&std::free)> owner(buffer.data, &std::free);
        return fengqudb::StoreCommandResult(
            {static_cast<const char *>(buffer.data), static_cast<size_t>(buffer.size)});
    };
    const std::string original(65536, 'x');
    execute(FQDB_HASH_SET, "a", original);
    execute(FQDB_HASH_SET, "b", "");
    auto scalar = execute(FQDB_HASH_GET, "a");
    auto empty = execute(FQDB_HASH_GET, "b");
    auto rows = execute(FQDB_HASH_SCAN);
    execute(FQDB_HASH_SET, "a", std::string(65536, 'y'));
    execute(FQDB_HASH_DELETE, "b");
    core->commit(tx);
    core->compact(p::maintenance_defaults());
    auto decoded = decode(rows);
    check(decode(scalar).value == original && decode(empty).value == "" &&
              decoded.rows.size() == 2 && decoded.rows[0].value == original &&
              decoded.rows[1].value.empty(),
          "retained result changed after private writes, commit or compaction");
}
static fengqudb::StoreCommandResult run(Store &store, uint32_t op, std::string_view scope,
                                        std::string_view key = {}, std::string_view value = {},
                                        int64_t index = 0, double score = 0) {
    return store.execute({op, scope, key, value, index, score});
}
static void binary_cursor_boundaries() {
    Fixture fixture;
    auto path = fixture.name("binary-cursors");
    std::string binary(40000, '\0');
    {
        Store store(path);
        struct Case {
            uint32_t write, scan;
            const char *scope;
        };
        for (auto entry :
             {Case{FQDB_HASH_SET, FQDB_HASH_SCAN, "hash"}, Case{FQDB_SET_ADD, FQDB_SET_SCAN, "set"},
              Case{FQDB_SORTED_ADD, FQDB_SORTED_RANGE, "sorted"}}) {
            run(store, entry.write, entry.scope, binary);
            run(store, entry.write, entry.scope, "z");
            StoreCommand command{entry.scan, entry.scope};
            command.limit = 1;
            auto first = store.execute(command);
            check(first.rows.size() == 1 && first.rows[0].key == binary &&
                      first.cursor.size() > FQDB_MAX_KEY_BYTES,
                  "binary member did not produce long cursor");
            command.key = first.cursor;
            auto next = store.execute(command);
            check(next.rows.size() == 1 && next.rows[0].key == "z",
                  "long collection cursor rejected");
        }
        fengqudb::wire::Writer value;
        value.bytes_field(1, "shared");
        auto key = "doc/" + binary;
        put(store, key, value.data());
        put(store, "doc/z", value.data());
        store.define_index("by-value", "doc/", 1, 4, 0);
        StoreCommand command{FQDB_INDEX_LOOKUP, "by-value", {}, value.data()};
        command.limit = 1;
        auto first = store.execute(command);
        check(first.rows.size() == 1 && first.rows[0].key == key &&
                  first.cursor.size() > FQDB_MAX_KEY_BYTES,
              "index did not produce long cursor");
        command.key = first.cursor;
        auto next = store.execute(command);
        check(next.rows.size() == 1 && next.rows[0].key == "doc/z", "long index cursor rejected");
        store.checkpoint();
    }
    Store reopened(path);
    check(reopened.hash_get("hash", binary)->view().empty(), "binary member restart");
    reopened.verify();
}
static void request_ownership_and_misses() {
    Fixture fixture;
    auto path = fixture.name("request-ownership");
    {
        fqdb_store *raw = nullptr;
        check(test_support::open_strict(path.c_str(), nullptr, &raw) == FQDB_OK,
              "request fixture open");
        std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> store(raw, fqdb_store_close);
        fqdb_store_txn *raw_tx = nullptr;
        check(fqdb_store_txn_begin(raw, 0, &raw_tx) == FQDB_OK, "request transaction begin");
        std::unique_ptr<fqdb_store_txn, decltype(&fqdb_store_txn_close)> tx(raw_tx,
                                                                            fqdb_store_txn_close);
        std::string payload(1024 * 1024, 'v');
        auto request = StoreCommand{FQDB_HASH_SET, "owned", "field", payload}.encode();
        fqdb_buffer result{};
        check(fqdb_store_txn_execute(raw_tx, {request.data(), request.size()}, &result) == FQDB_OK,
              "borrowed request staging");
        fqdb_buffer_free(&result);
        // The API call has ended: the caller can immediately reuse its input.
        std::fill(request.begin(), request.end(), '!');
        uint64_t version = 0;
        check(fqdb_store_txn_commit(raw_tx, &version) == FQDB_OK, "borrowed request commit");
    }
    Store store(path);
    check(store.hash_get("owned", "field")->view() == std::string(1024 * 1024, 'v'),
          "staged command retained caller buffer");
    auto missing = store.transaction();
    check(!missing.get("absent"), "missing key appeared");
    put(store, "absent", "concurrent");
    missing.put("dependent", "must conflict");
    check(missing.commit().status == fengqudb::Status::conflict,
          "missing read lost conflict dependency");
    auto staged = store.transaction();
    staged.put("private", "value");
    check(staged.get("private") == "value", "private read");
    auto save = staged.savepoint();
    staged.erase("private");
    check(!staged.get("private"), "private erase miss");
    staged.rollback_to(save);
    check(staged.get("private") == "value", "private read after rollback");
    commit(staged);
}
static void collections_and_indexes() {
    Fixture fixture;
    auto path = fixture.name("source"), copy = fixture.name("copy");
    {
        Store store(path);
        check(run(store, FQDB_HASH_SET, "hash", "a", "one").changed, "hash insert");
        check(run(store, FQDB_HASH_GET, "hash", "a").value == "one", "hash get");
        auto shared = store.hash_get("hash", "a"), again = store.hash_get("hash", "a");
        check(shared && again && shared->view().data() == again->view().data() &&
                  !store.hash_get("hash", "missing"),
              "Hash shared cache read");
        auto tx = store.transaction();
        tx.execute({FQDB_HASH_SET, "hash", "b", "two"});
        tx.put("business", "raw");
        commit(tx);
        check(run(store, FQDB_COLLECTION_SIZE, "hash").count == 2, "hash count");
        check(run(store, FQDB_SET_ADD, "set", "x").changed &&
                  !run(store, FQDB_SET_ADD, "set", "x").changed,
              "set idempotence");
        run(store, FQDB_LIST_PUSH_RIGHT, "list", {}, "right");
        run(store, FQDB_LIST_PUSH_LEFT, "list", {}, "left");
        check(run(store, FQDB_LIST_GET, "list", {}, {}, -1).value == "right",
              "list negative index");
        check(run(store, FQDB_LIST_POP_LEFT, "list").value == "left", "list pop");
        run(store, FQDB_SORTED_ADD, "sorted", "b", {}, 0, 2);
        run(store, FQDB_SORTED_ADD, "sorted", "a", {}, 0, -3);
        auto rows = run(store, FQDB_SORTED_RANGE, "sorted").rows;
        check(rows.size() == 2 && rows[0].key == "a" && rows[0].score == -3, "sorted order");
        run(store, FQDB_SORTED_ADD, "sorted", "a", {}, 0, 4);
        check(run(store, FQDB_SORTED_RANGE, "sorted").rows[0].key == "b", "sorted update");
        check(status([&] { run(store, FQDB_SET_ADD, "hash", "wrong"); }) == FQDB_CONSTRAINT,
              "collection type safety");
        auto snapshot = store.snapshot();
        auto cursor = snapshot.scan();
        size_t raw = 0;
        while (auto row = cursor.next()) {
            check(row->key == "business", "system row leaked");
            ++raw;
        }
        check(raw == 1, "business scan count");
        auto independent = store.transaction();
        auto business = independent.scan();
        while (business.next()) {
        }
        run(store, FQDB_HASH_SET, "hash", "unrelated", "system-only");
        independent.put("second", "accepted");
        commit(independent);
        fengqudb::wire::Writer a, b;
        a.bytes_field(1, "alice");
        b.bytes_field(1, "bob");
        auto records = store.transaction();
        records.put("user/1", a.data());
        records.put("user/2", b.data());
        commit(records);
        store.define_index("email", "user/", 1, 4, 3);
        check(run(store, FQDB_INDEX_LOOKUP, "email", {}, a.data()).rows[0].key == "user/1",
              "index build");
        auto duplicate = store.transaction();
        duplicate.put("user/3", a.data());
        check(duplicate.commit().status == fengqudb::Status::constraint, "unique constraint");
        auto missing = store.transaction();
        missing.put("user/3", "");
        check(missing.commit().status == fengqudb::Status::constraint, "required constraint");
        auto swap = store.transaction();
        swap.put("user/1", b.data());
        swap.put("user/2", a.data());
        commit(swap);
        check(run(store, FQDB_INDEX_LOOKUP, "email", {}, a.data()).rows[0].key == "user/2",
              "atomic unique swap");
        store.backup(copy);
        store.checkpoint();
    }
    {
        Store store(path);
        check(run(store, FQDB_HASH_GET, "hash", "b").value == "two", "collection restart");
        store.verify();
    }
    {
        Store store(copy);
        check(run(store, FQDB_COLLECTION_SIZE, "sorted").count == 2, "feature backup");
        store.drop_index("email");
    }
}
static void archives_roundtrip() {
    // PITR boundaries must not depend on wall-clock adjustments or scheduler
    // delays in instrumented builds. Advance the test clock between commits.
    struct ControlledClock {
        ControlledClock() {
            p::test_unix_ms.store(100000);
        }
        ~ControlledClock() {
            p::test_unix_ms.store(0);
        }
    } clock;
    Fixture fixture;
    auto source = fixture.name("source"), base = fixture.name("base"),
         archive = fixture.name("archive");
    auto latest = fixture.name("latest"), point = fixture.name("point"),
         timed = fixture.name("timed");
    uint64_t version, time;
    {
        Store store(source);
        put(store, "key", "A");
        auto from = store.backup(base).snapshot_version;
        p::test_unix_ms.store(100010);
        put(store, "key", "B");
        version = store.info().version;
        time = p::unix_ms();
        p::test_unix_ms.store(100020);
        run(store, FQDB_HASH_SET, "hash", "field", "value");
        auto exported = store.backup_incremental(archive, from);
        check(exported.final_version == store.info().version, "incremental capture version");
        check(status([&] { Store wrong(archive); }) == FQDB_UNSUPPORTED,
              "archive opened as live Store");
    }
    Store::recover_chain(base, {archive}, point, version);
    Store::recover_chain(base, {archive}, latest);
    Store::recover_chain(base, {archive}, timed, 0, time);
    {
        Store restored(timed);
        check(restored.get("key")->view() == "B" &&
                  run(restored, FQDB_COLLECTION_SIZE, "hash").count == 0,
              "PITR timestamp");
    }
    {
        Store store(point);
        check(store.get("key")->view() == "B" &&
                  run(store, FQDB_COLLECTION_SIZE, "hash").count == 0,
              "PITR version");
    }
    {
        Store store(latest);
        check(run(store, FQDB_HASH_GET, "hash", "field").value == "value",
              "incremental feature replay");
    }
}
static void grouping_and_background() {
    Fixture fixture;
    Store store(fixture.name("source"));
    fengqudb::StoreRuntimeOptions options;
    options.group_delay_us = 2000;
    store.configure(options);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::future<void>> writers;
    for (unsigned i = 0; i < 8; ++i)
        writers.push_back(std::async(std::launch::async, [&, i] {
            auto tx = store.transaction();
            tx.put("thread/" + std::to_string(i), "value");
            ++ready;
            while (!start.load())
                std::this_thread::yield();
            commit(tx);
        }));
    while (ready.load() != 8)
        std::this_thread::yield();
    start.store(true);
    for (auto &writer : writers)
        writer.get();
    auto grouped = store.runtime_info();
    check(grouped.group_transactions == 8 && grouped.group_flushes < 8,
          "concurrent syncs were not grouped");
    auto expiry = store.transaction();
    expiry.put("expired", "x", 1);
    commit(expiry);
    options.background = true;
    options.interval_ms = 10;
    options.expiry_keys = 1;
    options.compact_min_bytes = 1;
    options.compact_growth_bytes = 1;
    options.garbage_percent = 1;
    store.configure(options);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!store.runtime_info().compactions) {
        check(std::chrono::steady_clock::now() < deadline, "background maintenance did not run");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    store.configure();
    check(!store.runtime_info().running, "background worker did not stop");
    store.verify();
}
static void fault_boundaries() {
    Fixture fixture;
    auto path = fixture.name("group-fault");
    {
        Store store(path);
        put(store, "old", "published");
        std::future<fqdb_status> first;
        Held hold(40);
        first = std::async(std::launch::async, [&] {
            auto tx = store.transaction();
            tx.put("new", "pending");
            return static_cast<fqdb_status>(tx.commit().status);
        });
        reached();
        check(!store.get("new") && store.get("old")->view() == "published",
              "pending group leaked before flush");
        fqdb_store_test_io_fault(0, 1);
        hold.release();
        check(first.get() == FQDB_COMMIT_UNKNOWN, "group sync failure status");
        fqdb_store_test_io_fault(0, 0);
        auto later = store.transaction();
        later.put("later", "no");
        check(later.commit().status == fengqudb::Status::recovery_required,
              "group failure allowed later write");
    }
    {
        Store store(path);
        check(store.get("new")->view() == "pending" && !store.get("later"), "group fault recovery");
        store.verify();
    }
    auto collision = fixture.name("collision");
    {
        auto core = std::make_shared<p::Core>(collision, p::defaults());
        core->open();
        core->write_policy.mode = false; // Strict-path fixture.
        p::Transaction tx(core, 0);
        tx.put(p::system_prefix() + "legacy", "retained", 0);
        core->commit(tx);
    }
    {
        Store store(collision);
        auto old = store.snapshot();
        check(status([&] { run(store, FQDB_HASH_SET, "hash", "field", "value"); }) ==
                  FQDB_CONSTRAINT,
              "legacy namespace collision hidden");
        auto tx = store.transaction();
        tx.erase(p::system_prefix() + "legacy");
        commit(tx);
        run(store, FQDB_HASH_SET, "hash", "field", "value");
        check(old.get(p::system_prefix() + "legacy")->view() == "retained",
              "activation changed old business snapshot");
    }
    auto source = fixture.name("index-resume");
    {
        Store store(source);
        put(store, "doc/1", "");
        check(status([&] { store.define_index("required", "doc/", 1, 4, 2); }) == FQDB_CONSTRAINT,
              "invalid existing source accepted");
        check(status([&] { run(store, FQDB_INDEX_LOOKUP, "required", {}, "\x0a\x01x"); }) ==
                  FQDB_LOCKED,
              "building index queried as complete");
        put(store, "doc/1", "\x0a\x01x");
        store.define_index("required", "doc/", 1, 4, 2);
        check(run(store, FQDB_INDEX_LOOKUP, "required", {}, "\x0a\x01x").rows.size() == 1,
              "index build did not resume");
        store.verify();
    }
}
static void automatic_index_resume() {
    Fixture fixture;
    Store store(fixture.name("automatic-index"));
    auto records = store.transaction();
    records.put("bad/1", "");
    records.put("good/1", "\x0a\x01x");
    commit(records);
    check(status([&] { store.define_index("bad", "bad/", 1, 4, 2); }) == FQDB_CONSTRAINT,
          "bad index setup");
    std::future<fqdb_status> builder;
    Held hold(45);
    builder = std::async(std::launch::async, [&] {
        return status([&] { store.define_index("good", "good/", 1, 4, 2); });
    });
    reached();
    store.cancel_maintenance();
    hold.release();
    check(builder.get() == FQDB_CANCELLED, "index cancel did not leave resumable definition");
    fengqudb::StoreRuntimeOptions runtime;
    runtime.background = true;
    runtime.interval_ms = 10;
    store.configure(runtime);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto code = status([&] {
            check(run(store, FQDB_INDEX_LOOKUP, "good", {}, "\x0a\x01x").rows.size() == 1,
                  "resumed index rows");
        });
        if (code == FQDB_OK)
            break;
        check(code == FQDB_LOCKED && std::chrono::steady_clock::now() < deadline,
              "bad rule starved healthy background index");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    store.configure();
    store.drop_index("bad");
    store.verify();
}
static void collection_model() {
    Fixture fixture;
    fengqudb::StoreOptions options;
    options.durability = fengqudb::Durability::buffered;
    options.cache_bytes = 65536;
    options.delta_bytes = 65536;
    Store store(fixture.name("model"), options);
    std::map<std::string, std::string> hash;
    std::set<std::string> set;
    std::deque<std::string> list;
    std::map<std::string, double> sorted;
    std::mt19937 random(7834);
    for (unsigned i = 0; i < 600; ++i) {
        auto key = "key/" + std::to_string(random() % 32);
        auto value = std::string(3, static_cast<char>(random() % 256));
        switch (random() % 8) {
        case 0:
            run(store, FQDB_HASH_SET, "hash", key, value);
            hash[key] = value;
            break;
        case 1:
            run(store, FQDB_HASH_DELETE, "hash", key);
            hash.erase(key);
            break;
        case 2:
            run(store, FQDB_SET_ADD, "set", key);
            set.insert(key);
            break;
        case 3:
            run(store, FQDB_SET_REMOVE, "set", key);
            set.erase(key);
            break;
        case 4:
            run(store, FQDB_LIST_PUSH_LEFT, "list", {}, value);
            list.push_front(value);
            break;
        case 5:
            if (!list.empty()) {
                check(run(store, FQDB_LIST_POP_RIGHT, "list").value == list.back(),
                      "list model pop");
                list.pop_back();
            }
            break;
        case 6: {
            double score = static_cast<int>(random() % 23) - 11;
            run(store, FQDB_SORTED_ADD, "sorted", key, {}, 0, score);
            sorted[key] = score;
            break;
        }
        case 7:
            run(store, FQDB_SORTED_REMOVE, "sorted", key);
            sorted.erase(key);
            break;
        }
        if (i % 100 == 0) {
            store.checkpoint();
            store.verify();
        }
    }
    auto hashes = run(store, FQDB_HASH_SCAN, "hash");
    check(hashes.rows.size() == hash.size(), "hash model size");
    for (const auto &row : hashes.rows)
        check(hash.at(row.key) == row.value, "hash model bytes");
    auto sets = run(store, FQDB_SET_SCAN, "set");
    check(sets.rows.size() == set.size(), "set model size");
    for (const auto &row : sets.rows)
        check(set.count(row.key) == 1, "set model member");
    auto lists = run(store, FQDB_LIST_RANGE, "list");
    check(lists.rows.size() == list.size(), "list model count");
    for (size_t i = 0; i < list.size(); ++i)
        check(lists.rows[i].value == list[i], "list model order");
    std::set<std::pair<double, std::string>> order;
    for (const auto &item : sorted)
        order.emplace(item.second, item.first);
    auto scores = run(store, FQDB_SORTED_RANGE, "sorted");
    check(scores.rows.size() == order.size(), "sorted model count");
    size_t n = 0;
    for (const auto &item : order) {
        check(scores.rows[n].score == item.first && scores.rows[n].key == item.second,
              "sorted model order");
        ++n;
    }
    std::set<std::string> scanned;
    std::string token;
    do {
        fengqudb::StoreCommand command{FQDB_HASH_SCAN, "hash", token};
        command.limit = 3;
        auto page = store.execute(command);
        for (const auto &row : page.rows)
            check(scanned.insert(row.key).second, "pagination duplicate");
        token = page.cursor;
    } while (!token.empty());
    check(scanned.size() == hash.size(), "pagination lost members");
    auto tx = store.transaction();
    auto mark = tx.savepoint();
    tx.execute({FQDB_HASH_SET, "hash", "rollback", "discard"});
    tx.rollback_to(mark);
    tx.put("business", "accepted");
    commit(tx);
    check(!store.hash_get("hash", "rollback"), "collection savepoint isolation");
    run(store, FQDB_COLLECTION_CLEAR, "hash");
    check(run(store, FQDB_COLLECTION_SIZE, "hash").count == 0, "collection clear");
    store.verify();
}
static void queued_group_handoff() {
    for (unsigned maximum : {64u, 128u, 256u}) {
        Fixture fixture;
        const auto path = fixture.name("handoff");
        auto core = std::make_shared<p::Core>(path, p::defaults());
        core->open();
        core->write_policy.mode = false; // Strict-path fixture.
        core->group_max.store(maximum);
        std::unique_lock<p::WriterGate> writer(core->writer);
        constexpr unsigned count = 192;
        std::vector<std::thread> jobs;
        std::array<std::exception_ptr, count> errors{};
        struct Join {
            std::unique_lock<p::WriterGate> &owner;
            std::vector<std::thread> &jobs;
            ~Join() {
                if (owner.owns_lock())
                    owner.unlock();
                for (auto &job : jobs)
                    if (job.joinable())
                        job.join();
            }
        } join{writer, jobs};
        for (unsigned i = 0; i < count; ++i)
            jobs.emplace_back([&, i] {
                try {
                    p::Transaction tx(core, 0);
                    tx.put("key/" + std::to_string(i), "durable", 0);
                    core->commit(tx);
                } catch (...) {
                    errors[i] = std::current_exception();
                }
            });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            {
                std::lock_guard<std::mutex> queue(core->group_mutex);
                if (core->group_count == count)
                    break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                writer.unlock();
                throw std::runtime_error("writers did not reach group handoff barrier");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        writer.unlock();
        for (auto &job : jobs)
            job.join();
        for (const auto &error : errors)
            if (error)
                std::rethrow_exception(error);
        check(core->current()->version == count && core->current()->durable == count &&
                  core->group_flushes.load() == (count + maximum - 1) / maximum,
              "queued group boundaries or durable acknowledgements changed");
        core.reset();
        Store reopened(path);
        for (unsigned i = 0; i < count; ++i)
            check(reopened.get("key/" + std::to_string(i))->view() == "durable",
                  "group handoff lost an acknowledged transaction");
    }
}
static void concurrent_stress() {
    Fixture fixture;
    Store store(fixture.name("concurrent"));
    fengqudb::StoreRuntimeOptions policy;
    policy.group_delay_us = 500;
    store.configure(policy);
    std::vector<std::future<void>> jobs;
    for (unsigned worker = 0; worker < 8; ++worker)
        jobs.push_back(std::async(std::launch::async, [&, worker] {
            for (unsigned i = 0; i < 64; ++i)
                put(store, "worker/" + std::to_string(worker) + "/" + std::to_string(i),
                    std::to_string(i));
        }));
    for (auto &job : jobs)
        job.get();
    check(store.info().stored_keys == 512 && store.info().durable_version == 512,
          "group stress count/durability");
    for (unsigned worker = 0; worker < 8; ++worker)
        for (unsigned i = 0; i < 64; ++i)
            check(store.get("worker/" + std::to_string(worker) + "/" + std::to_string(i))->view() ==
                      std::to_string(i),
                  "group stress bytes");
    auto stats = store.runtime_info();
    check(stats.group_flushes < stats.group_transactions, "group stress did not share sync");
    store.verify();
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--list-query") {
            contiguous_list_ranges();
            cursor_pin_lifecycle();
            std::cout << "PASS list range conflicts, ownership, fallback and cursor pins\n";
            return 0;
        }
        if (argc == 5) {
            fqdb_store_test_crash(std::stoi(argv[4]));
            std::string_view mode(argv[1]);
            {
                Store store(argv[2]);
                if (mode == "--activation")
                    run(store, FQDB_HASH_SET, "crash", "field", "value");
                else if (mode == "--index")
                    store.define_index("by-id", "doc/", 1, 1, 3);
                else if (mode == "--archive")
                    store.backup_incremental(argv[3], 1);
            }
            return 88;
        }
        contiguous_list_ranges();
        cursor_pin_lifecycle();
        store_test_trace("retained_result_ownership");
        retained_result_ownership();
        store_test_trace("collections_and_indexes");
        collections_and_indexes();
        store_test_trace("binary_cursor_boundaries");
        binary_cursor_boundaries();
        store_test_trace("request_ownership_and_misses");
        request_ownership_and_misses();
        store_test_trace("archives_roundtrip");
        archives_roundtrip();
        store_test_trace("grouping_and_background");
        grouping_and_background();
        store_test_trace("fault_boundaries");
        fault_boundaries();
        store_test_trace("automatic_index_resume");
        automatic_index_resume();
        store_test_trace("collection_model");
        collection_model();
        store_test_trace("queued_group_handoff");
        queued_group_handoff();
        store_test_trace("concurrent_stress");
        concurrent_stress();
#ifndef __SANITIZE_THREAD__
        auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
        for (int point : {1, 2, 3, 4}) {
            Fixture fixture;
            auto path = fixture.name("activation");
            {
                Store store(path);
                put(store, "seed", "stable");
            }
            check(child(executable, "--activation", path, path, point) == 86,
                  "feature activation crash point");
            Store recovered(path);
            check(recovered.get("seed")->view() == "stable", "activation lost old data");
            if (point == 1)
                check(!recovered.hash_get("crash", "field"), "pre-WAL activation committed");
            else if (point == 2) {
                auto value = recovered.hash_get("crash", "field");
                check(!value || value->view() == "value",
                      "unflushed activation recovered partial data");
            } else
                check(recovered.hash_get("crash", "field")->view() == "value",
                      "activation recovery partial");
            auto snapshot = recovered.snapshot();
            auto cursor = snapshot.scan();
            size_t count = 0;
            while (auto row = cursor.next()) {
                check(row->key == "seed", "activation leaked system row");
                ++count;
            }
            check(count == 1, "activation business count");
            recovered.verify();
        }
        for (int point : {45, 46}) {
            Fixture fixture;
            auto path = fixture.name("index");
            {
                Store store(path);
                auto tx = store.transaction();
                for (unsigned i = 0; i < 96; ++i) {
                    fengqudb::wire::Writer value;
                    value.uint_field(1, i);
                    tx.put("doc/" + std::to_string(10000 + i), value.data());
                }
                commit(tx);
            }
            check(child(executable, "--index", path, path, point) == 86, "index crash point");
            Store recovered(path);
            auto info = recovered.define_index("by-id", "doc/", 1, 1, 3);
            check(info.copied_keys == static_cast<uint64_t>(point == 46 ? 64 : 96),
                  "index recovery restarted completed prefix");
            recovered.verify();
        }
        for (int point : {60, 61}) {
            Fixture fixture;
            auto source = fixture.name("source"), archive = fixture.name("archive"),
                 base = fixture.name("base"), restored = fixture.name("restored");
            {
                Store store(source);
                put(store, "key", "base");
                store.backup(base);
                put(store, "key", "new");
            }
            check(child(executable, "--archive", source, archive, point) == 86,
                  "archive publication crash point");
            {
                Store store(source);
                check(store.get("key")->view() == "new", "archive crash changed source");
            }
            if (point == 60)
                check(!std::filesystem::exists(std::filesystem::u8path(archive)),
                      "unpublished archive visible");
            else {
                Store::recover_chain(base, {archive}, restored);
                Store result(restored);
                check(result.get("key")->view() == "new", "published crash archive invalid");
            }
        }

#else
        std::cout << "SKIP feature crash subprocesses under TSan; covered by Windows/ASan\n";
#endif
        std::cout << "PASS native collections, automatic indexes/constraints, backup chains/PITR, "
                     "group commit and background lifecycle\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
