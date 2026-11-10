#include "strict_store.hpp"
#include "store_test_support.hpp"
#include "paged/watch.hpp"

static std::string bytes(fqdb_slice value) {
    return value.size ? std::string(static_cast<const char *>(value.data), value.size) : "";
}
static void transactions_and_ack() {
    Fixture fixture;
    Store db(fixture.name("commits"));
    auto watch = db.watch();
    check(!watch.next(), "fresh stream is not empty");
    auto tx = db.transaction();
    tx.put("", "");
    tx.put("a", "before");
    tx.put("z", "last");
    auto version = tx.commit().version;
    auto first = watch.next();
    check(first && first->info().version == version && first->info().count == 3 &&
              first->info().unix_ms && first->info().durable_version >= version,
          "whole transaction metadata");
    check(bytes(first->info().rows[0].key).empty() && first->info().rows[0].kind == FQDB_PUT &&
              bytes(first->info().rows[0].value).empty() &&
              bytes(first->info().rows[1].value) == "before",
          "empty key/value or sorted rows");
    auto retry = watch.next();
    check(retry->info().version == version &&
              retry->info().rows[1].value.data == first->info().rows[1].value.data,
          "unacked batch was copied or advanced");
    check(status([&] { watch.ack(version + 1); }) == FQDB_INVALID_ARGUMENT, "future ack accepted");
    watch.ack(version);
    watch.ack(version);
    auto edit = db.transaction();
    edit.patch("a", 0, 6, "after");
    edit.erase("z");
    edit.expire("a", UINT64_C(4102444800000));
    commit(edit);
    auto second = watch.next();
    check(second->info().count == 2 && bytes(second->info().rows[0].value) == "after" &&
              second->info().rows[0].expires_at_ms == UINT64_C(4102444800000) &&
              second->info().rows[1].kind == FQDB_DELETE,
          "patch/expiry/delete change semantics");
    watch.ack(second->info().version);
    watch.cancel();
    check(bytes(first->info().rows[1].value) == "before" &&
              status([&] { watch.next(); }) == FQDB_CANCELLED,
          "batch lifetime or terminal cancellation");
}
static void filtering_and_metadata() {
    Fixture fixture;
    Store db(fixture.name("filtered"));
    fengqudb::StoreWatchOptions options;
    options.max_scan_commits = 2;
    options.flags = FQDB_WATCH_METADATA_ONLY;
    auto watch = db.watch(FQDB_HISTORY_CURRENT, "user/", options);
    put(db, "other/1", "x");
    put(db, "other/2", "x");
    auto progress = watch.next();
    check(progress && progress->info().count == 0 && progress->info().version == 2,
          "bounded filtering did not publish progress");
    watch.ack(progress->info().version);
    put(db, "user/1", std::string(1024 * 1024, 'x'));
    auto selected = watch.next();
    check(selected->info().count == 1 && selected->info().rows[0].kind == FQDB_PUT &&
              selected->info().rows[0].value.data == nullptr && selected->info().flags == 1,
          "metadata-only materialized a value");
    watch.ack(selected->info().version);
    auto raw = db.watch();
    options.flags = FQDB_WATCH_INCLUDE_SYSTEM;
    auto internal = db.watch(FQDB_HISTORY_CURRENT, {}, options);
    db.execute({FQDB_HASH_SET, "h", "f", "v"});
    auto hidden = raw.next();
    auto all = internal.next();
    check(hidden->info().count == 0 && all->info().count >= 2, "system filtering");
    db.verify();
}
static void retention_and_raw_namespace() {
    Fixture fixture;
    auto path = fixture.name("raw"), base = fixture.name("base"), inc = fixture.name("inc"),
         copy = fixture.name("copy");
    auto reserved = p::system_prefix() + "H" + p::component("business");
    {
        // Older raw files can contain a key now reserved by public write APIs.
        // History-only retention must preserve its existing read visibility.
        auto core = std::make_shared<p::Core>(path, p::defaults());
        core->open();
        core->write_policy.mode = false; // Strict-path fixture.
        p::Transaction tx(core, 0);
        tx.put(reserved, "legacy", 0);
        core->commit(tx);
    }
    {
        Store db(path);
        auto from = db.backup(base).snapshot_version;
        auto slow = db.watch(from), fast = db.watch(from);
        for (unsigned i = 0; i < 4; ++i) {
            put(db, "ordinary", std::to_string(i));
            db.compact();
        }
        auto current = db.info().version;
        check(db.get(reserved)->view() == "legacy" && db.history_info().floor == from &&
                  db.history_info().active_pins == 0 && db.history_info().retained_generations == 4,
              "process retention hid raw system-prefix key");
        auto snapshot = db.snapshot();
        auto key = db.prepare_key(reserved);
        check(snapshot.get(reserved)->view() == "legacy" && key.get()->view() == "legacy" &&
                  db.get_many({reserved})[0].value == "legacy",
              "namespace fast reads changed");
        check(status([&] { db.execute({FQDB_HASH_SET, "h", "f", "v"}); }) == FQDB_CONSTRAINT,
              "collection activation skipped raw namespace validation");
        uint64_t last = from;
        while (auto batch = fast.next()) {
            check(batch->info().version == last + 1 && batch->info().count == 1,
                  "retained stream order");
            last = batch->info().version;
            fast.ack(last);
        }
        check(last == current, "stream did not reach head");
        db.compact();
        check(db.history_info().floor == from, "fast reader dropped slow reader history");
        db.backup_incremental(inc, from);
        slow.cancel();
        db.compact();
        check(db.history_info().retained_generations == 0, "cancel did not release retention");
        db.verify();
    }
    Store::recover_chain(base, {inc}, copy);
    {
        Store db(path), recovered(copy);
        check(db.get(reserved)->view() == "legacy" && recovered.get(reserved)->view() == "legacy",
              "history-only flag failed restart/restore");
        db.verify();
        recovered.verify();
        check(status([&] { db.watch(0); }) == FQDB_HISTORY_LOST, "lost history accepted");
    }
}
static void waits_and_concurrency() {
    Fixture fixture;
    Store db(fixture.name("wait"));
    auto watch = db.watch();
    check(!watch.next(5), "empty timed wait produced data");
    std::promise<void> entered;
    auto blocked = std::async(std::launch::async, [&] {
        entered.set_value();
        return status([&] { watch.next(5000); });
    });
    entered.get_future().wait();
    check(blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
          "watch did not wait");
    check(status([&] { watch.next(); }) == FQDB_LOCKED, "concurrent next was not rejected");
    watch.cancel();
    check(blocked.get() == FQDB_CANCELLED, "cancel did not interrupt wait");
    auto stream = db.watch();
    auto producer = std::async(std::launch::async, [&] {
        for (unsigned i = 1; i <= 200; ++i) {
            put(db, "counter", std::to_string(i));
            if (i % 17 == 0)
                db.compact();
        }
    });
    for (uint64_t version = 1; version <= 200; ++version) {
        auto batch = stream.next(5000);
        check(batch && batch->info().version == version && batch->info().count == 1 &&
                  bytes(batch->info().rows[0].value) == std::to_string(version),
              "concurrent wait missed or reordered commit");
        stream.ack(version);
    }
    producer.get();
    check(!stream.next(), "stream duplicated acknowledged data");
    Held hold(60);
    put(db, "cancel", "before parse");
    auto parsing = std::async(std::launch::async, [&] { return status([&] { stream.next(); }); });
    reached();
    stream.cancel();
    hold.release();
    check(parsing.get() == FQDB_CANCELLED, "cancel during parse returned data");
}
static void ceilings_and_lifetime() {
    Fixture fixture;
    Store db(fixture.name("limits"));
    std::vector<fengqudb::StoreWatch> watches;
    for (unsigned i = 0; i < 64; ++i)
        watches.push_back(db.watch());
    check(status([&] { db.watch(); }) == FQDB_LIMIT, "watch ceiling");
    watches[0].cancel();
    auto replacement = db.watch();
    watches.clear();
    replacement.cancel();
    fengqudb::StoreWatchOptions small;
    small.max_batch_bytes = 4096;
    auto constrained = db.watch(FQDB_HISTORY_CURRENT, {}, small);
    put(db, "big", std::string(64000, 'a'));
    for (unsigned i = 0; i < 2; ++i)
        check(status([&] { constrained.next(); }) == FQDB_LIMIT, "budget failure advanced stream");
    constrained.cancel();
    small.flags = FQDB_WATCH_METADATA_ONLY;
    auto meta = db.watch(0, {}, small);
    auto batch = meta.next();
    check(batch->info().version == 1 && batch->info().count == 1, "metadata budget fallback");
    auto raw_path = fixture.name("child");
    fqdb_store *parent = nullptr;
    fqdb_store_watch *child = nullptr;
    check(test_support::open_strict(raw_path.c_str(), nullptr, &parent) == FQDB_OK &&
              fqdb_store_watch_open(parent, 0, {}, nullptr, &child) == FQDB_OK,
          "raw watch open");
    fqdb_store_close(parent);
    fqdb_store_change_batch *out = reinterpret_cast<fqdb_store_change_batch *>(1);
    check(fqdb_store_watch_next(child, 0, &out) == FQDB_NOT_MODIFIED && !out,
          "child did not retain parent");
    fqdb_store_watch_close(child);
    fengqudb::StoreOptions options;
    options.max_snapshots = 1;
    Store limited(fixture.name("snapshot-limit"), options);
    auto only = limited.watch();
    check(status([&] { limited.snapshot(); }) == FQDB_LIMIT, "watch bypassed handle budget");
}
static void unpublished_commits() {
    Fixture fixture;
    auto path = fixture.name("publication");
    {
        Store db(path);
        auto watch = db.watch();
        Held hold(40);
        auto writing = std::async(std::launch::async, [&] {
            auto tx = db.transaction();
            tx.put("pending", "value");
            return static_cast<fqdb_status>(tx.commit().status);
        });
        reached();
        check(!watch.next(), "unflushed candidate was streamed");
        fqdb_store_test_io_fault(0, 1);
        hold.release();
        auto result = writing.get();
        fqdb_store_test_io_fault(0, 0);
        check(result == FQDB_COMMIT_UNKNOWN && !watch.next(),
              "uncertain failed publication was streamed");
    }
    Store reopened(path);
    auto watch = reopened.watch(0);
    auto recovered = watch.next();
    check(recovered && recovered->info().version == 1 &&
              bytes(recovered->info().rows[0].value) == "value",
          "uncertain valid WAL was not recovered");
}
template <class F> static fqdb_status internal_status(F function) {
    try {
        function();
        return FQDB_OK;
    } catch (const p::Error &e) {
        return e.status;
    }
}
static void bookmarks_and_corruption() {
    Fixture fixture;
    auto options = p::defaults();
    options.durability = FQDB_DURABILITY_BUFFERED;
    auto core = std::make_shared<p::Core>(fixture.name("io"), options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    auto write = [&](const std::string &value) {
        p::Transaction tx(core, 0);
        tx.put("k", value, 0);
        core->commit(tx);
    };
    for (unsigned i = 0; i < 512; ++i)
        write(std::string(4096, 'x'));
    p::Watch watch(core, FQDB_HISTORY_CURRENT, {}, p::watch_defaults());
    auto file = core->current()->generation->file;
    for (unsigned i = 0; i < 5; ++i) {
        write("incremental");
        auto before = file->read_bytes.load();
        auto result = watch.next(0);
        check(result->info.count == 1 && file->read_bytes.load() - before < 8192 &&
                  result->info.durable_version < result->info.version,
              "poll rescanned old WAL or lied about buffered durability");
        watch.ack(result->info.version);
    }
    write("crc-data");
    auto ref = *core->current()->find("k");
    char original = 0;
    file->read(ref.offset, &original, 1);
    const char damaged = original ^ 1;
    file->write(ref.offset, &damaged, 1);
    check(internal_status([&] { watch.next(0); }) == FQDB_CORRUPT, "WAL CRC was not checked");
    file->write(ref.offset, &original, 1);
    auto repaired = watch.next(0);
    check(bytes(repaired->info.rows[0].value) == "crc-data",
          "corruption failure advanced bookmark");
    watch.ack(repaired->info.version);
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 5) {
            Store db(argv[2]);
            auto watch = db.watch(0);
            fqdb_store_test_crash(std::stoi(argv[4]));
            db.compact();
            return 88;
        }
        store_test_trace("transactions_and_ack");
        transactions_and_ack();
        store_test_trace("filtering_and_metadata");
        filtering_and_metadata();
        store_test_trace("retention_and_raw_namespace");
        retention_and_raw_namespace();
        store_test_trace("waits_and_concurrency");
        waits_and_concurrency();
        store_test_trace("ceilings_and_lifetime");
        ceilings_and_lifetime();
        store_test_trace("unpublished_commits");
        unpublished_commits();
        store_test_trace("bookmarks_and_corruption");
        bookmarks_and_corruption();
#ifndef __SANITIZE_THREAD__
        auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
        for (int point : {10, 11, 12, 13}) {
            Fixture fixture;
            auto path = fixture.name("crash");
            {
                Store db(path);
                put(db, "key", "durable");
            }
            check(child(executable, "--compact", path, path, point) == 86, "watch crash child");
            Store db(path);
            check(db.get("key")->view() == "durable", "watch compaction crash lost data");
            {
                auto watch = db.watch(0);
                check(watch.next()->info().count == 1, "crash lost retained WAL");
            }
            db.compact();
            db.verify();
        }
#endif
        std::cout << "PASS watch commits, explicit ack, filters, live retention, waits/cancel, "
                     "budgets, bookmarks, CRC and restart boundaries\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
