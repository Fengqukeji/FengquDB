#include "strict_store.hpp"
#include "store_test_support.hpp"
#include <array>
#include <map>
#include "store_clock.hpp"

static std::string key(unsigned i) {
    return "key/" + std::to_string(100000 + i);
}
static bool same(const std::optional<p::Ref> &a, const std::optional<p::Ref> &b) {
    if (bool(a) != bool(b))
        return false;
    return !a || (a->version == b->version && a->offset == b->offset && a->size == b->size &&
                  a->expires == b->expires && a->crc == b->crc && a->depth == b->depth &&
                  a->recipe_size == b->recipe_size);
}
static void internal(bool segmented) {
    Fixture fixture;
    auto options = p::defaults();
    options.flags |= segmented ? FQDB_STORE_SEGMENTED : 0;
    options.cache_bytes = 0;
    options.delta_bytes = 65536;
    options.writer_bytes = 1048576;
    options.durability = FQDB_DURABILITY_BUFFERED;
    auto core = std::make_shared<p::Core>(fixture.name("cold"), options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    for (unsigned first = 0; first < 4096; first += 64) {
        p::Transaction tx(core, 0);
        for (unsigned i = first; i < first + 64; ++i)
            tx.put(key(i), std::string(96, char(i % 127)), i % 97 == 0 ? 1 : 0);
        core->commit(tx);
    }
    core->checkpoint();
    auto old = core->current();
    check(!old->delta && old->checkpoint.root != 0, "cold lookup fixture not on disk");
    std::vector<std::optional<p::Ref>> expected;
    const auto before = old->generation->file->reads.load();
    for (unsigned i = 0; i < 32; ++i)
        expected.push_back(old->find(key(1024 + i)));
    const auto baseline = old->generation->file->reads.load() - before;
    p::Table::ReadPath path(262144);
    const auto begin = old->generation->file->reads.load();
    for (unsigned i = 0; i < 32; ++i)
        check(same(expected[i], old->find(key(1024 + i), path)), "adjacent metadata differs");
    const auto reused = old->generation->file->reads.load() - begin;
    check(baseline && reused * 2 < baseline && path.retained_bytes() <= 262144,
          "batch path did not reduce real cold page I/O within its bound");
    std::cout << "lookup segmented=" << segmented << " baseline_reads=" << baseline
              << " reused_reads=" << reused << " retained=" << path.retained_bytes() << '\n';
    for (uint64_t budget : {UINT64_C(0), UINT64_C(1024), UINT64_C(262144)}) {
        p::Table::ReadPath bounded(budget);
        for (unsigned step = 0; step < 4096; ++step) {
            const auto name = step % 17 == 0 ? "missing" : key((step * 211) % 4096);
            check(same(old->find(name), old->find(name, bounded)),
                  "random/missing metadata differs");
            check(bounded.retained_bytes() <= budget, "lookup exceeded its retained-page bound");
        }
    }
    {
        p::Transaction tx(core, 0);
        tx.put(key(1024), "changed", 0);
        tx.erase(key(1025));
        core->commit(tx);
    }
    auto changed = core->current();
    check(same(changed->find(key(1024)), changed->find(key(1024), path)), "delta override ignored");
    core->checkpoint();
    changed = core->current();
    for (unsigned i = 1023; i < 1027; ++i) {
        check(same(changed->find(key(i)), changed->find(key(i), path)),
              "new root reused old metadata");
        check(same(old->find(key(i)), old->find(key(i), path)), "old snapshot metadata changed");
    }
    const auto previous = changed->generation;
    {
        // Exercise hint hits, misses between neighbours, backward movement,
        // duplicates and the end-of-page lower_bound sentinel.
        std::vector<std::string> names{"", "zzzz", key(0), key(0), key(4095), key(4095) + "!"};
        for (unsigned i = 0; i < 256; ++i) {
            names.push_back(key(i));
            names.push_back(key(i) + "!");
        }
        for (unsigned i = 4096; i-- > 3840;)
            names.push_back(key(i));
        p::Table::ReadPath hinted(262144);
        for (const auto &name : names)
            check(same(changed->find(name), changed->find(name, hinted)),
                  "position hint changed lower_bound semantics");
    }
    core->compact(p::maintenance_defaults());
    auto next = core->current();
    check(next->generation != previous, "generation handoff not exercised");
    for (unsigned i = 1023; i < 1027; ++i) {
        check(same(next->find(key(i)), next->find(key(i), path)),
              "generation switch reused old offsets");
        check(same(old->find(key(i)), old->find(key(i), path)), "old generation lookup changed");
    }
    core->verify(p::maintenance_defaults());
}

static void public_api(bool segmented, bool cached) {
    Fixture fixture;
    const auto path = fixture.name("public");
    std::optional<fengqudb::StoreValues> retained;
    fengqudb::StoreOptions options;
    options.segmented = segmented;
    options.cache_bytes = cached ? 8 * 1048576 : 0;
    options.durability = fengqudb::Durability::buffered;
    {
        Store db(path, options);
        for (unsigned first = 0; first < 512; first += 32) {
            auto tx = db.transaction();
            for (unsigned i = first; i < first + 32; ++i)
                tx.put(key(i), i % 19 == 0 ? "" : std::string(96, char(i % 127)),
                       i % 23 == 0 ? 1 : 0);
            commit(tx);
        }
        db.checkpoint();
        auto old = db.snapshot();
        put(db, key(1), "changed");
        {
            auto tx = db.transaction();
            tx.erase(key(2));
            commit(tx);
        }
        for (size_t count :
             {size_t(0), size_t(1), size_t(7), size_t(8), size_t(32), size_t(4096)}) {
            std::vector<std::string> names;
            std::vector<std::string_view> input;
            for (size_t i = 0; i < count; ++i)
                names.push_back(i % 11 == 0 ? "absent" : key(unsigned((i * 137) % 512)));
            for (const auto &name : names)
                input.push_back(name);
            const auto before = db.info().pinned_bytes;
            {
                auto current = db.get_many(input), snapshot = old.get_many(input);
                check(current.size() == count && snapshot.size() == count, "batch count changed");
                for (size_t i = 0; i < count; ++i) {
                    const auto want = db.get(names[i]), previous = old.get(names[i]);
                    check(bool(current[i].value) == bool(want) &&
                              (!want || *current[i].value == want->view()),
                          "current batch value differs");
                    check(bool(snapshot[i].value) == bool(previous) &&
                              (!previous || *snapshot[i].value == previous->view()),
                          "snapshot batch value differs");
                }
            }
            check(db.info().pinned_bytes == before, "request-local path retained a result charge");
        }
        const auto repeated = key(1);
        std::vector<std::string_view> duplicate(32, repeated);
        retained.emplace(db.get_many(duplicate));
        db.compact();
        db.verify();
    }
    check(retained && retained->size() == 32 && (*retained)[0].value == "changed" &&
              (*retained)[31].value == "changed",
          "result lifetime borrowed lookup scratch");
    retained.reset();
    options.create = false;
    Store reopened(path, options);
    check(reopened.get(key(1))->view() == "changed" && !reopened.get(key(2)),
          "batch path changed persistence");
    reopened.verify();
}
static void shared_reader(bool segmented, bool cache) {
    Fixture fixture;
    fengqudb::StoreOptions options;
    options.segmented = segmented;
    options.durability = fengqudb::Durability::buffered;
    options.cache_bytes = cache ? 65536 : 0;
    StoreTestClock clock(1000000);
    std::optional<fengqudb::StoreReader> reader;
    std::optional<fengqudb::StoreValues> held;
    {
        Store db(fixture.name("shared"), options);
        auto tx = db.transaction();
        tx.put("a", "original");
        tx.put("empty", "");
        tx.put("ttl", "alive", 1000010);
        commit(tx);
        auto baseline = db.info().pinned_bytes;
        reader.emplace(
            db.prepare_reader(std::vector<std::string_view>{"a", "missing", "empty", "ttl", "a"}));
        auto rows = reader->read();
        check(rows.size() == 5 && rows[0].value == "original" && !rows[1].value && rows[2].value &&
                  rows[2].value->empty() && rows[3].value == "alive" &&
                  rows[4].value == rows[0].value,
              "shared miss/empty/duplicates");
        auto pointer = rows[0].value->data();
        check(reader->read()[0].value->data() == pointer, "shared hot path copied payload");
        held.emplace(reader->acquire());
        clock.set(1000010);
        check(!reader->read()[3].value && (*held)[3].value == "alive", "reader TTL boundary");
        clock.set(1000001);
        check(reader->read()[3].value == "alive", "reader backwards clock reused expired miss");
        auto edit = db.transaction();
        edit.patch("a", 0, 8, "new");
        edit.erase("empty");
        commit(edit);
        check(reader->read()[0].value == "new" && !reader->read()[2].value &&
                  (*held)[0].value == "original" && (*held)[2].value->empty(),
              "shared COW mutation");
        db.checkpoint();
        db.compact();
        check(reader->read()[0].value == "new" && (*held)[0].value == "original",
              "reader compaction");
        held.reset();
        auto retained = db.info().pinned_bytes;
        reader->release();
        check(db.info().pinned_bytes < retained && db.info().pinned_bytes >= baseline,
              "reader release did not drop payload charge");
        check(reader->read()[0].value == "new", "reader release lost prepared keys");
        auto empty = db.prepare_reader(std::vector<std::string_view>{});
        check(empty.read().size() == 0 && empty.acquire().size() == 0, "zero-row reader");
        // Readers must observe one atomic publication, never half of a write.
        put(db, "x", "0");
        put(db, "y", "0");
        auto pair = db.prepare_reader(std::vector<std::string_view>{"x", "y"});
        auto writer = std::async(std::launch::async, [&] {
            for (unsigned i = 1; i < 100; ++i) {
                auto write = db.transaction();
                auto value = std::to_string(i);
                write.put("x", value);
                write.put("y", value);
                commit(write);
            }
        });
        for (unsigned i = 0; i < 500; ++i) {
            auto values = pair.read();
            check(values[0].value == values[1].value, "shared batch saw torn transaction");
        }
        writer.get();
        db.verify();
    }
    check(reader->read()[0].value == "new", "reader invalid after Store close");
    reader.reset();
    // Resource failure leaves every C output empty; ordinary copied reads still work.
    auto native = options.native();
    native.pinned_bytes = 65536;
    fqdb_store *db = nullptr;
    auto path = fixture.name("limited");
    check(test_support::open_strict(path.c_str(), &native, &db) == FQDB_OK, "limited open");
    std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> db_owner(db, fqdb_store_close);
    fqdb_store_txn *tx = nullptr;
    check(fqdb_store_txn_begin(db, 0, &tx) == FQDB_OK, "limited begin");
    std::unique_ptr<fqdb_store_txn, decltype(&fqdb_store_txn_close)> tx_owner(tx,
                                                                              fqdb_store_txn_close);
    std::string large(131072, 'z');
    check(fqdb_store_txn_put(tx, {"k", 1}, {large.data(), large.size()}, 0) == FQDB_OK,
          "limited put");
    uint64_t version = 0;
    check(fqdb_store_txn_commit(tx, &version) == FQDB_OK, "limited commit");
    tx_owner.reset();
    fqdb_store_shared_value value{};
    check(fqdb_store_get_shared(db, {"k", 1}, &value) == FQDB_LIMIT && !value.owner &&
              !value.row.found,
          "shared pin failure leaked output");
    fqdb_slice input{"k", 1};
    fqdb_store_reader *raw = nullptr;
    check(fqdb_store_reader_create(db, &input, 1, &raw) == FQDB_OK, "limited reader create");
    std::unique_ptr<fqdb_store_reader, decltype(&fqdb_store_reader_close)> owner(
        raw, fqdb_store_reader_close);
    const fqdb_store_result_row *data = reinterpret_cast<const fqdb_store_result_row *>(1);
    uint32_t count = 99;
    check(fqdb_store_reader_read(raw, &data, &count) == FQDB_LIMIT && !data && !count,
          "reader pin failure leaked partial output");
    fqdb_buffer copy{};
    check(fqdb_store_get(db, input, &copy) == FQDB_OK && copy.size == large.size(),
          "copy depends on pins");
    fqdb_buffer_free(&copy);
}

static void tight_batch_budget(bool cached) {
    Fixture fixture;
    fengqudb::StoreOptions options;
    options.pinned_bytes = 65536;
    options.cache_bytes = cached ? 1048576 : 0;
    options.durability = fengqudb::Durability::buffered;
    Store db(fixture.name("batch-credit"), options);
    auto tx = db.transaction();
    std::vector<std::string> names;
    for (unsigned i = 0; i < 32; ++i) {
        names.push_back(key(i));
        tx.put(names.back(), std::string(i ? 1 : 40000, char('a' + i % 26)));
    }
    commit(tx);
    tx = db.transaction();
    std::vector<std::string_view> input;
    for (const auto &name : names)
        input.push_back(name);
    const auto baseline = db.info().pinned_bytes;
    {
        // A preferred eight-row allowance cannot fit, but the exact result can.
        auto rows = db.get_many(input);
        check(rows.size() == 32 && rows[0].value->size() == 40000 && rows[31].value->size() == 1,
              "speculative budget allowance rejected a valid batch");
        auto release = std::async(std::launch::async, [owned = std::move(rows)]() mutable {
            auto local = std::move(owned); // Destroy the payload on this worker.
        });
        release.get();
    }
    check(db.info().pinned_bytes == baseline, "batch allowance leaked after cross-thread close");
    {
        auto snapshot = db.snapshot();
        auto rows = snapshot.get_many(input);
        check(rows[0].value->size() == 40000 && rows[1].value->size() == 1,
              "snapshot exact-budget batch");
    }
    {
        auto rows = tx.get_many(input);
        check(rows[0].value->size() == 40000, "transaction exact-budget batch");
    }
    {
        auto reader = db.prepare_reader(input);
        auto rows = reader.read();
        check(rows[0].value->size() == 40000, "prepared reader exact-budget batch");
    }
    auto failing = input;
    failing.back() = names.front(); // Fail after retaining a valid prefix.
    check(status([&] { (void)db.get_many(failing); }) == FQDB_LIMIT,
          "oversized pinned batch did not fail");
    check(db.info().pinned_bytes == baseline, "failed batch leaked payload or unused allowance");
}
int main() {
    try {
        p::test_segment_geometry.store(65536);
        tight_batch_budget(false);
        tight_batch_budget(true);
        for (bool segmented : {false, true}) {
            for (bool cache : {false, true})
                shared_reader(segmented, cache);
            internal(segmented);
            public_api(segmented, false);
            public_api(segmented, true);
        }
        std::cout << "PASS bounded request-local paths, actual I/O reduction, arbitrary keys, "
                     "roots, generations and result lifetimes\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
