#include "store_test_support.hpp"
#include <fstream>

static size_t generations(const std::string &path) {
    size_t count = 0;
    for (auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(path)))
        if (p::generation_filename(entry.path().filename().u8string()))
            ++count;
    return count;
}
static void retained_archives_and_forks() {
    Fixture fixture;
    auto source = fixture.name("source"), base = fixture.name("base"),
         increment = fixture.name("inc"), restored = fixture.name("restored");
    uint64_t from;
    {
        Store db(source);
        put(db, "key", "first");
        from = db.backup(base).snapshot_version;
        check(db.hold_history("backup", from).changed, "hold watermark");
        auto version = db.info().version;
        check(!db.hold_history("backup", from).changed && db.info().version == version,
              "same watermark wrote WAL");
        for (unsigned i = 0; i < 4; ++i) {
            put(db, "key", std::to_string(i));
            db.compact();
        }
        auto info = db.history_info();
        check(info.floor == from && info.active_pins == 1 && info.retained_generations == 4 &&
                  info.retained_file_bytes > 0,
              "retained chain info");
        db.verify();
    }
    {
        Store db(source);
        check(db.history_info().floor == from && db.history_info().retained_generations == 4,
              "restart lost retention");
        check(db.backup_incremental(increment, from).final_version == db.info().version,
              "cross-generation incremental archive");
        Store::recover_chain(base, {increment}, restored);
        {
            Store fork(restored);
            check(fork.get("key")->view() == "3" && fork.history_info().active_pins == 0,
                  "fork inherited active source watermark");
            fork.hold_history("backup", fork.info().version);
            check(fork.history_info().active_pins == 1, "foreign pin could not be rebound");
            fork.release_history("backup");
            fork.compact();
            fork.verify();
        }
        {
            auto snapshot = db.snapshot();
            db.release_history("backup");
            db.compact();
            check(db.history_info().retained_generations == 0 && snapshot.get("key")->view() == "3",
                  "release broke old snapshot");
            check(generations(source) > 1, "pinned snapshot generations were deleted");
        }
        check(generations(source) == 1, "released ancestors did not retire");
        check(!db.release_history("missing").changed, "missing release not idempotent");
        check(status([&] { db.hold_history("too-old", from); }) == FQDB_HISTORY_LOST,
              "unavailable history was accepted");
        db.verify();
    }
}
static void reserve_before_backup() {
    Fixture fixture;
    Store db(fixture.name("source"));
    put(db, "key", "value");
    auto from = db.info().version;
    db.hold_history("new-backup");
    check(db.history_info().oldest_pin == from, "latest reservation was not atomic");
    auto base = db.backup(fixture.name("base"));
    db.hold_history("new-backup", base.snapshot_version);
    put(db, "key", "new");
    db.compact();
    check(db.history_info().floor == base.snapshot_version, "base watermark advancement");
    db.verify();
}
static void multiple_watermarks_and_large_patches() {
    Fixture fixture;
    auto source = fixture.name("source"), base = fixture.name("base"), inc1 = fixture.name("inc1"),
         inc2 = fixture.name("inc2"), copy = fixture.name("copy");
    Store db(source);
    std::string value(1024 * 1024, 'x');
    put(db, "large", value);
    auto from = db.backup(base).snapshot_version;
    db.hold_history("slow", from);
    db.hold_history("fast", from);
    for (unsigned i = 0; i < 4; ++i) {
        auto tx = db.transaction();
        tx.patch("large", i, 1, "z");
        commit(tx);
        value[i] = 'z';
    }
    db.compact();
    auto end = db.backup_incremental(inc1, from).final_version;
    db.hold_history("fast", end);
    check(db.history_info().oldest_pin == from, "fast reader released slow reader history");
    db.release_history("slow");
    put(db, "later", "yes");
    db.compact();
    check(db.history_info().floor == end && db.history_info().retained_generations <= 2,
          "watermark advancement retained unnecessary ancestors");
    db.backup_incremental(inc2, end);
    Store::recover_chain(base, {inc1, inc2}, copy);
    {
        Store recovered(copy);
        check(recovered.get("large")->view() == value && recovered.get("later")->view() == "yes",
              "patch history chain recovery");
        recovered.verify();
    }
    db.release_history("fast");
    db.compact();
    db.verify();
}
static void ceilings_and_unchanged_compactions() {
    Fixture fixture;
    Store db(fixture.name("limits"));
    db.hold_history("keep", 0);
    put(db, "n", "seed");
    db.compact();
    for (unsigned i = 0; i < 5; ++i)
        db.compact();
    check(db.history_info().retained_generations == 1,
          "unchanged compaction extended empty history chain");
    for (unsigned i = 1; i < 128; ++i) {
        put(db, "n", std::to_string(i));
        db.compact();
    }
    check(db.history_info().retained_generations == 128, "retention segment ceiling setup");
    put(db, "n", "overflow");
    auto version = db.info().version;
    check(status([&] { db.compact(); }) == FQDB_LIMIT && db.info().version == version,
          "retention ceiling corrupted current generation");
    db.release_history("keep");
    db.compact();
    check(db.history_info().retained_generations == 0, "release did not unblock compaction");
    auto from = db.info().version;
    for (unsigned i = 0; i < 64; ++i)
        db.hold_history("pin/" + std::to_string(i), from);
    check(status([&] { db.hold_history("overflow", from); }) == FQDB_LIMIT &&
              db.history_info().active_pins == 64,
          "watermark ceiling");
    for (unsigned i = 0; i < 64; ++i)
        db.release_history("pin/" + std::to_string(i));
    db.compact();
    db.verify();
}
static void concurrent_registration_and_activation() {
    Fixture fixture;
    Store db(fixture.name("concurrent"));
    put(db, "seed", "x");
    std::future<fqdb_status> compact;
    Held hold(10);
    compact = std::async(std::launch::async, [&] { return status([&] { db.compact(); }); });
    reached();
    auto from = db.info().version;
    db.hold_history("concurrent", from);
    put(db, "key", "during copy");
    hold.release();
    check(compact.get() == FQDB_OK && db.history_info().floor == from,
          "late watermark not captured by compaction");
    check(db.get("key")->view() == "during copy", "catch-up source data lost");
    db.verify();
    db.release_history("concurrent");
    db.compact();
}
static void missing_parent_preserves_files() {
    Fixture fixture;
    auto path = fixture.name("missing");
    std::filesystem::path parent, missing;
    {
        Store db(path);
        db.hold_history("keep", 0);
        put(db, "n", "v");
        db.compact();
    }
    auto selected = p::read_current(path);
    for (auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(path)))
        if (p::generation_filename(entry.path().filename().u8string()) &&
            entry.path().filename().u8string() != selected.filename())
            parent = entry.path();
    missing = std::filesystem::u8path(fixture.root) / "parent.saved";
    std::filesystem::rename(parent, missing);
    auto count = generations(path);
    check(status([&] { Store broken(path); }) != FQDB_OK && generations(path) == count,
          "missing ancestor was ignored or files reclaimed");
    std::filesystem::rename(missing, parent);
    Store recovered(path);
    check(recovered.get("n")->view() == "v", "missing ancestor repair failed");
    recovered.verify();
}
static void retained_buffered_flush_failure() {
    Fixture fixture;
    auto path = fixture.name("flush-fault");
    {
        fengqudb::StoreOptions options;
        options.durability = fengqudb::Durability::buffered;
        Store db(path, options);
        db.hold_history("keep", 0);
        put(db, "key", "pending");
        // CompactTarget seals with two syncs before persisting the retained
        // source generation. Fail that source sync, before CURRENT publication.
        fqdb_store_test_io_fault(0, 3);
        auto result = status([&] { db.compact(); });
        fqdb_store_test_io_fault(0, 0);
        check(result == FQDB_COMMIT_UNKNOWN, "retained source sync uncertainty");
        check(status([&] { db.put_if_changed("later", "no"); }) == FQDB_RECOVERY_REQUIRED,
              "source sync fault allowed further writes");
    }
    Store recovered(path);
    check(recovered.get("key")->view() == "pending" && recovered.history_info().active_pins == 1,
          "retained buffered recovery");
    recovered.verify();
}
static void late_collection_flags_and_checkpoint_time() {
    Fixture fixture;
    auto path = fixture.name("late-flags");
    {
        Store db(path);
        put(db, "seed", "x");
        Held hold(10);
        auto compact =
            std::async(std::launch::async, [&] { return status([&] { db.compact(); }); });
        reached();
        db.execute({FQDB_HASH_SET, "h", "f", "v"});
        hold.release();
        check(compact.get() == FQDB_OK && db.hash_get("h", "f")->view() == "v",
              "late collection activation lost format flags");
    }
    {
        Store reopened(path);
        check(reopened.hash_get("h", "f")->view() == "v",
              "late collection flags were not persisted");
    }
    auto clock_path = fixture.name("late-time");
    struct Clock {
        ~Clock() {
            p::test_unix_ms.store(0);
        }
    } clock;
    p::test_unix_ms.store(100000);
    {
        Store db(clock_path);
        put(db, "seed", "x");
        p::test_unix_ms.store(100010);
        Held hold(10);
        auto compact =
            std::async(std::launch::async, [&] { return status([&] { db.compact(); }); });
        reached();
        p::test_unix_ms.store(100020);
        db.checkpoint();
        hold.release();
        check(compact.get() == FQDB_OK, "checkpoint clock catch-up compaction");
    }
    auto core = std::make_shared<p::Core>(clock_path, p::defaults());
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    check(core->current()->checkpoint.time == 100020,
          "checkpoint timestamp watermark lost across compaction");
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 5) {
            Store db(argv[2]);
            fqdb_store_test_crash(std::stoi(argv[4]));
            db.compact();
            return 88;
        }
        store_test_trace("retained_archives_and_forks");
        retained_archives_and_forks();
        store_test_trace("reserve_before_backup");
        reserve_before_backup();
        store_test_trace("multiple_watermarks_and_large_patches");
        multiple_watermarks_and_large_patches();
        store_test_trace("ceilings_and_unchanged_compactions");
        ceilings_and_unchanged_compactions();
        store_test_trace("concurrent_registration_and_activation");
        concurrent_registration_and_activation();
        store_test_trace("missing_parent_preserves_files");
        missing_parent_preserves_files();
        store_test_trace("retained_buffered_flush_failure");
        retained_buffered_flush_failure();
        store_test_trace("late_collection_flags_and_checkpoint_time");
        late_collection_flags_and_checkpoint_time();
#ifndef __SANITIZE_THREAD__
        auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
        for (int point : {10, 11, 12, 13}) {
            Fixture fixture;
            auto path = fixture.name("source"), base = fixture.name("base"),
                 inc = fixture.name("inc"), copy = fixture.name("copy");
            uint64_t from;
            {
                Store db(path);
                put(db, "key", "A");
                from = db.backup(base).snapshot_version;
                db.hold_history("keep", from);
                put(db, "key", "B");
                db.compact();
                put(db, "key", "C");
            }
            check(child(executable, "--compact", path, path, point) == 86,
                  "retained compaction crash child");
            {
                Store db(path);
                check(db.get("key")->view() == "C" && db.history_info().floor == from,
                      "retained compaction crash restart");
                db.backup_incremental(inc, from);
                db.verify();
            }
            Store::recover_chain(base, {inc}, copy);
            {
                Store restored(copy);
                check(restored.get("key")->view() == "C", "crash retained incremental chain");
            }
        }
#endif
        std::cout << "PASS durable backup watermarks, retained generation chains, incremental/PITR "
                     "history, ceilings, concurrent activation and crash recovery\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
