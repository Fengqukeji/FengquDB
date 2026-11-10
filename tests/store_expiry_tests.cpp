#include "store_test_support.hpp"
#include "paged/expiry.hpp"
#include "store_clock.hpp"
#include "paged/index_build.hpp"

static void indexed_batches() {
    Fixture fixture;
    auto path = fixture.name("source");
    auto options = p::defaults();
    options.cache_bytes = 0;
    options.durability = FQDB_DURABILITY_BUFFERED;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    {
        p::Transaction tx(core, 0);
        for (unsigned i = 0; i < 3000; ++i)
            tx.put("key/" + std::to_string(10000 + i), "x", i % 2 ? 0 : UINT64_MAX);
        for (unsigned i = 0; i < 10; ++i)
            tx.put("key/" + std::to_string(10000 + i * 300), "expired", 1);
        tx.put("big", std::string(8 * 1024 * 1024, 'x'), 1);
        core->commit(tx);
    }
    core->checkpoint();
    {
        p::Transaction tx(core, 0);
        tx.put("key/10000", "renewed", 0);
        tx.put("key/10300", "extended", UINT64_MAX);
        tx.erase("key/10600");
        tx.put("delta", "expired", 1);
        core->commit(tx);
    }
    auto view = core->current();
    auto before = view->generation->file->reads.load();
    p::ViewCursor all(*view);
    while (all.next()) {
    }
    auto all_reads = view->generation->file->reads.load() - before;
    before = view->generation->file->reads.load();
    p::ExpiryCursor due(*view, p::unix_ms(), [] {});
    uint64_t count = 0;
    while (auto row = due.next()) {
        check(row->key != "key/10000" && row->key != "key/10300" && row->key != "key/10600",
              "delta override did not hide expired disk value");
        ++count;
    }
    check(count == 9, "indexed expiry candidates");
    check(view->generation->file->reads.load() - before < all_reads / 2,
          "expiry traversal did not prune future/non-TTL subtrees");
    std::cout << "TTL traversal: " << due.visited_pages() << " candidate pages, " << all_reads
              << " full traversal I/O calls\n";
    auto bytes_before = view->generation->file->read_bytes.load();
    auto settings = p::expiry_defaults();
    settings.max_keys = 2;
    uint64_t removed = 0, calls = 0;
    fqdb_store_expiry_info info{};
    do {
        info = core->purge_expired(settings);
        check(info.removed_keys <= 2, "expiry batch key limit");
        removed += info.removed_keys;
        if (++calls == 1) {
            // Force a new expired key behind the continuation into the disk tree.
            p::Transaction tx(core, 0);
            tx.put("", "late", 1);
            core->commit(tx);
            core->checkpoint();
        }
        check(calls < 20, "expiry continuation did not converge");
    } while (info.more);
    check(removed == 10, "expiry continuation lost/duplicated a key");
    check(view->generation->file->read_bytes.load() - bytes_before < 2 * 1024 * 1024,
          "expiry purge read large value payload");
    check(core->current()->keys == 2992, "expiry stored counter mismatch");
    core->checkpoint();
    auto version = core->current()->version;
    info = core->purge_expired(settings);
    check(info.removed_keys == 0 && !info.more && info.version == version &&
              info.visited_pages == 1,
          "empty expiry pass did not prune root or changed version");
    core->verify(p::maintenance_defaults());

    // Old internal entries with no field 11 remain conservative and readable.
    auto current = core->current();
    auto page = current->generation->page(current->checkpoint.root, current->limit);
    p::Slot slot;
    slot.key = page->slots.front().key;
    slot.child = current->checkpoint.root;
    slot.maximum = page->maximum;
    p::Page legacy(false);
    legacy.set(0, slot, true);
    check(p::Page::decode(legacy.binary).earliest == 0, "legacy expiry summary not unknown");
    slot.earliest = page->earliest;
    p::Page indexed(false);
    indexed.set(0, slot, true);
    check(p::Page::decode(indexed.binary).earliest == page->earliest,
          "new expiry summary encoding");
    // A self-consistent page CRC cannot conceal a lying parent summary from verify.
    slot.earliest = 1;
    indexed.set(0, slot, false);
    auto bad = *current;
    bad.checkpoint.root = current->generation->write_page(indexed, current->version);
    bad.checkpoint.end = current->generation->tail;
    bool corrupt = false;
    try {
        p::verify_index(bad, [] {});
    } catch (const p::Error &error) {
        corrupt = error.status == FQDB_CORRUPT;
    }
    check(corrupt, "verify missed lying expiry summary");
}

static void indexed_large_expiry() {
    Fixture fixture;
    auto opts = p::defaults();
    opts.cache_bytes = 0;
    opts.durability = FQDB_DURABILITY_BUFFERED;
    auto core = std::make_shared<p::Core>(fixture.name("indexed-large"), opts);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    p::IndexRule rule;
    rule.name = "field";
    rule.prefix = "doc/";
    rule.field = 1;
    rule.type = 4;
    rule.flags = 3;
    p::build_index(*core, rule, false, p::maintenance_defaults());
    {
        fengqudb::wire::Writer value;
        value.bytes_field(1, "x");
        value.bytes_field(2, std::string(8 * 1024 * 1024, 'x'));
        p::Transaction tx(core, 0);
        tx.put("doc/large", value.data(), 1);
        core->commit(tx);
    }
    auto generation = core->current()->generation;
    auto before = generation->file->read_bytes.load();
    auto info = core->purge_expired(p::expiry_defaults());
    check(info.removed_value_bytes >= 8 * 1024 * 1024 &&
              generation->file->read_bytes.load() - before < 1048576,
          "indexed TTL cleanup read expired business payload");
    core->verify(p::maintenance_defaults());
}
static void isolation_and_failures() {
    Fixture fixture;
    auto path = fixture.name("source");
    {
        Store store(path);
        auto now = p::unix_ms();
        StoreTestClock clock(now);
        {
            auto tx = store.transaction();
            tx.put("old", "retained", now + 1);
            commit(tx);
        }
        auto snapshot = store.snapshot();
        clock.set(now + 2);
        auto result = store.purge_expired();
        auto retained = snapshot.get("old");
        check(result.removed_keys == 1 && result.removed_value_bytes == 8 && !store.get("old") &&
                  retained && retained->view() == "retained",
              "purge broke frozen-time snapshot");
        clock.reset();
        {
            auto tx = store.transaction();
            tx.put("race", "expired", 1);
            commit(tx);
        }
        Held hold(30);
        auto work =
            std::async(std::launch::async, [&] { return status([&] { store.purge_expired(); }); });
        reached();
        check(status([&] { store.verify(); }) == FQDB_LOCKED, "expiry maintenance lease");
        put(store, "race", "new value");
        hold.release();
        check(work.get() == FQDB_CONFLICT && store.get("race")->view() == "new value",
              "purge removed concurrent renewal");
        {
            auto tx = store.transaction();
            tx.put("cancel", "expired", 1);
            commit(tx);
        }
        fqdb_store_test_maintenance_hold(30);
        work =
            std::async(std::launch::async, [&] { return status([&] { store.purge_expired(); }); });
        reached();
        auto version = store.info().version;
        store.cancel_maintenance();
        hold.release();
        check(work.get() == FQDB_CANCELLED && store.info().version == version,
              "cancelled expiry batch committed");
        fengqudb::StoreExpiryOptions timeout;
        timeout.timeout_ms = 100;
        fqdb_store_test_maintenance_hold(30);
        work = std::async(std::launch::async,
                          [&] { return status([&] { store.purge_expired(timeout); }); });
        reached();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        hold.release();
        check(work.get() == FQDB_TIMEOUT && store.info().version == version,
              "expired maintenance deadline committed");
        timeout.timeout_ms = 0;
        timeout.max_batch_bytes = 1;
        check(status([&] { store.purge_expired(timeout); }) == FQDB_LIMIT &&
                  store.info().version == version,
              "undersized expiry metadata budget mutated state");
        check(store.purge_expired().removed_keys == 1, "expiry after cancellation");
        store.checkpoint();
    }
    {
        Store reopened(path);
        check(reopened.info().stored_keys == 1 && reopened.get("race")->view() == "new value",
              "expiry restart state");
    }
    for (int sync_failure : {0, 1}) {
        auto failed = fixture.name("failure-" + std::to_string(sync_failure));
        {
            Store store(failed);
            auto tx = store.transaction();
            tx.put("expires", "x", 1);
            commit(tx);
            fqdb_store_test_io_fault(sync_failure ? 0 : 1, sync_failure ? 1 : 0);
            check(status([&] { store.purge_expired(); }) == FQDB_COMMIT_UNKNOWN,
                  "expiry I/O failure status");
            fqdb_store_test_io_fault(0, 0);
            check(status([&] { store.purge_expired(); }) == FQDB_RECOVERY_REQUIRED,
                  "expiry failure did not block writes");
        }
        Store reopened(failed);
        reopened.verify();
        check(reopened.info().stored_keys == (sync_failure ? 0u : 1u), "expiry failure recovery");
    }
}

int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 5) {
            Store store(argv[2]);
            fqdb_store_test_crash(std::stoi(argv[4]));
            store.purge_expired();
            return 88;
        }
        indexed_batches();
        indexed_large_expiry();
        isolation_and_failures();
#ifndef __SANITIZE_THREAD__
        auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
        for (int point : {1, 2, 3, 4}) {
            Fixture fixture;
            auto path = fixture.name("source");
            uint64_t version;
            {
                Store store(path);
                auto tx = store.transaction();
                for (auto key : {"a", "b", "c"})
                    tx.put(key, "expired", 1);
                commit(tx);
                version = store.info().version;
            }
            check(child(executable, "--purge", path, path, point) == 86,
                  "expiry crash not reached");
            Store reopened(path);
            auto info = reopened.info();
            check((info.version == version && info.stored_keys == 3) ||
                      (info.version == version + 1 && info.stored_keys == 0),
                  "expiry crash produced partial batch");
            reopened.verify();
        }
#endif
        std::cout << "PASS indexed TTL pruning, bounded/resumable purge, no payload reads, OCC, "
                     "snapshots, cancellation, faults and crash recovery\n";
    } catch (const std::exception &error) {
        fqdb_store_test_maintenance_hold(0);
        fqdb_store_test_io_fault(0, 0);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
