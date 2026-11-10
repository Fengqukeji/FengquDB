#include "strict_store.hpp"
#include "store_test_support.hpp"
#include <atomic>
#include <limits>

static void atomic_operations() {
    Fixture fixture;
    auto path = fixture.name("atomic");
    Store store(path);
    auto inserted = store.put_if_absent("key", "first");
    check(inserted.matched && inserted.changed, "conditional insert");
    auto old = store.get("key");
    check(old && old->version() == inserted.version, "read metadata matches value");
    check(!store.put_if_absent("key", "wrong").matched, "absent condition ignored");
    check(store.info().version == inserted.version, "failed condition generated WAL");
    auto changed = store.compare_exchange("key", old->version(), "second");
    check(changed.matched && changed.changed, "CAS update");
    check(!store.compare_exchange("key", old->version(), "stale").matched,
          "stale CAS overwrote value");
    check(old->view() == "first" && old->version() == inserted.version, "old metadata changed");
    check(!store.put_if_changed("key", "second").changed, "unchanged put wrote WAL");
    check(!store.delete_if_version("key", inserted.version).matched, "stale delete");
    check(store.delete_if_version("key", changed.version).changed && !store.get("key"),
          "version delete");
    check(store.add_i64("counter", 5, 0, 10).value == 5, "bounded increment");
    auto version = store.info().version;
    check(status([&] { store.add_i64("counter", -6, 0, 10); }) == FQDB_CONSTRAINT,
          "counter underflow bound");
    check(store.info().version == version && !store.add_i64("counter", 0).changed,
          "failed/no-op counter commit");
    check(store.add_i64("max", INT64_MAX).value == INT64_MAX, "max counter");
    check(status([&] { store.add_i64("max", 1); }) == FQDB_CONSTRAINT, "signed overflow");
    check(store.add_i64("min", INT64_MIN).value == INT64_MIN, "min counter");
    check(status([&] { store.add_i64("min", -1); }) == FQDB_CONSTRAINT, "signed underflow");
    put(store, "wrong-type", "short");
    check(status([&] { store.add_i64("wrong-type", 1); }) == FQDB_INVALID_ARGUMENT, "counter type");
    auto tx = store.transaction();
    tx.put("expired", "old", 1);
    commit(tx);
    check(store.put_if_absent("expired", "new").changed, "expired key not treated as absent");
    auto expiry = store.transaction();
    expiry.put("ttl", std::string(8, '\0'), UINT64_MAX);
    commit(expiry);
    check(store.add_i64("ttl", 1).value == 1 && store.get("ttl")->expires_at_ms() == UINT64_MAX,
          "counter lost TTL");
    store.checkpoint();
    store.verify();
}
static void batches_and_prepared() {
    Fixture fixture;
    auto path = fixture.name("batch"), other_path = fixture.name("other");
    Store store(path), other(other_path);
    store.put_many({{"a", "old"}, {"empty", ""}, {"a", "last"}});
    auto values = store.get_many({"a", "missing", "empty", "a"});
    check(values.size() == 4 && values[0].value == "last" && !values[1].value &&
              values[2].value == "" && values[3].value == "last",
          "multi-get order/misses/duplicates");
    auto prepared = store.prepare_key("a");
    auto snapshot = store.snapshot();
    check(prepared.get()->view() == "last", "prepared initial");
    store.put_if_changed("a", "new");
    check(prepared.get()->view() == "new" && snapshot.get(prepared)->view() == "last",
          "prepared view identity cache");
    store.checkpoint();
    check(prepared.get()->view() == "new", "prepared after checkpoint");
    store.compact();
    check(prepared.get()->view() == "new" && snapshot.get(prepared)->view() == "last",
          "prepared after generation change");
    char buffer[16]{};
    uint64_t required = 0;
    check(prepared.get_into(buffer, sizeof(buffer), required) && required == 3 &&
              std::string_view(buffer, 3) == "new",
          "prepared caller buffer");
    check(status([&] { prepared.get_into(buffer, 1, required); }) == FQDB_BUFFER_TOO_SMALL &&
              required == 3,
          "buffer required size");
    auto tx = store.transaction();
    tx.put("private", "one");
    auto private_values = tx.get_many({"private", "absent"});
    check(private_values[0].value == "one" && private_values[0].version == 0 &&
              !private_values[1].value,
          "private multi-get");
    tx.patch("private", 0, 3, "two");
    check(private_values[0].value == "one", "batch result changed after patch");
    put(store, "absent", "concurrent");
    check(tx.commit().status == fengqudb::Status::conflict, "batch missing read dependency");
    std::string payload(128 * 1024, 'p');
    auto batch = store.prepare_batch({{"prepared", payload}, {"z", "first"}, {"z", "last"}});
    auto edit = store.transaction();
    auto save = edit.savepoint();
    auto before = store.info().transaction_bytes;
    edit.put_batch(batch);
    auto peak = store.info().transaction_bytes;
    check(peak > before + payload.size(), "prepared retention not charged");
    edit.rollback_to(save);
    check(store.info().transaction_bytes + payload.size() < peak,
          "prepared rollback retained payload charge");
    edit.put_batch(batch);
    edit.patch("prepared", 0, 1, "q");
    commit(edit);
    check(store.get("prepared")->view()[0] == 'q' && store.get("z")->view() == "last",
          "prepared private patch");
    store.put_batch(batch);
    check(store.get("prepared")->view() == payload,
          "prepared batch mutated by previous transaction");
    auto invalid = other.transaction();
    check(status([&] { invalid.put_batch(batch); }) == FQDB_INVALID_ARGUMENT,
          "foreign prepared batch accepted");
    check(status([&] { other.snapshot().get(prepared); }) == FQDB_INVALID_ARGUMENT,
          "foreign prepared key accepted");
    store.delete_many({"a", "z"});
    check(!prepared.get() && values[0].value == "last", "prepared miss/result lifetime");
    store.verify();
}
static void concurrent_atomic() {
    Fixture fixture;
    Store store(fixture.name("concurrent"));
    fengqudb::StoreRuntimeOptions options;
    options.group_delay_us = 1000;
    store.configure(options);
    std::vector<std::future<void>> writers;
    std::atomic<unsigned> starts{0};
    for (unsigned i = 0; i < 8; ++i)
        writers.push_back(std::async(std::launch::async, [&] {
            ++starts;
            while (starts.load() != 8)
                std::this_thread::yield();
            for (unsigned j = 0; j < 32; ++j)
                check(store.add_i64("counter", 1).changed, "atomic writer failed");
        }));
    for (auto &writer : writers)
        writer.get();
    check(store.add_i64("counter", 0).value == 256, "concurrent increment lost update");
    check(store.runtime_info().group_flushes < 256, "atomic updates bypassed shared flush");
    store.verify();
}
static void atomic_faults() {
    Fixture fixture;
    auto path = fixture.name("fault");
    {
        Store store(path);
        store.add_i64("counter", 1);
        Held hold(40);
        auto pending = std::async(std::launch::async,
                                  [&] { return status([&] { store.add_i64("counter", 1); }); });
        reached();
        check(store.get("counter")->view()[0] == 1, "unflushed atomic view leaked");
        fqdb_store_test_io_fault(0, 1);
        hold.release();
        check(pending.get() == FQDB_COMMIT_UNKNOWN, "atomic flush fault status");
        fqdb_store_test_io_fault(0, 0);
        check(status([&] { store.add_i64("counter", 1); }) == FQDB_RECOVERY_REQUIRED,
              "poisoned atomic writer continued");
    }
    Store reopened(path);
    check(reopened.add_i64("counter", 0).value == 2, "atomic fault recovery");
}
static void paths_and_legacy_names() {
    Fixture fixture;
    auto container = p::join(fixture.root, u8"saves-\u5b58\u6863");
    auto path = Store::path_at(container, "slot_1");
    fixture.directories.push_back(path);
    fixture.directories.push_back(container);
    check(std::filesystem::u8path(path).is_absolute() &&
              !std::filesystem::exists(std::filesystem::u8path(container)),
          "path resolution wrote to disk");
    for (const char *bad : {"..", "a/b", "a\\b", "CON", "Lpt1", "a:b", "", "a b",
                            "abcdefghijklmnopqrstuvwxyz1234567"})
        check(status([&] { Store::path_at(container, bad); }) == FQDB_INVALID_ARGUMENT,
              "invalid portable database name");
    check(status([&] { Store::path_at(std::string(1000, 'x'), "slot"); }) == FQDB_LIMIT,
          "long root accepted");
    auto absent_parent = p::join(fixture.root, "absent");
    auto long_component =
        (std::filesystem::u8path(absent_parent) / std::string(256, 'x')).u8string();
    check(status([&] { Store::open_at(long_component, "slot"); }) == FQDB_LIMIT &&
              !std::filesystem::exists(std::filesystem::u8path(absent_parent)),
          "long root component created partial directories");
#ifndef _WIN32
    // POSIX limits bytes, not Unicode code points: 86 CJK characters use 258 bytes.
    std::string unicode_component;
    for (unsigned i = 0; i < 86; ++i)
        unicode_component += u8"\u5b58";
    auto long_unicode =
        (std::filesystem::u8path(absent_parent) / std::filesystem::u8path(unicode_component))
            .u8string();
    check(status([&] { Store::open_at(long_unicode, "slot"); }) == FQDB_LIMIT &&
              !std::filesystem::exists(std::filesystem::u8path(absent_parent)),
          "long UTF-8 root component accepted");
#endif
    fengqudb::StoreOptions existing;
    existing.create = false;
    check(status([&] { Store::open_at(container, "missing", existing); }) == FQDB_NOT_FOUND &&
              !std::filesystem::exists(std::filesystem::u8path(container)),
          "read-only open created container");
    auto invalid_options = existing;
    invalid_options.create = true;
    invalid_options.delta_bytes = 1;
    check(status([&] { Store::open_at(container, "missing", invalid_options); }) ==
                  FQDB_INVALID_ARGUMENT &&
              !std::filesystem::exists(std::filesystem::u8path(container)),
          "invalid options created container");
    {
        auto store = Store::open_at(container, "slot_1");
        auto other_root = p::join(fixture.root, "other");
        auto other_path = Store::path_at(other_root, "slot_1");
        fixture.directories.insert(fixture.directories.begin(), {other_path, other_root});
        auto other = Store::open_at(other_root, "slot_1");
        put(other, "key", "other");
        put(store, "key", "data");
        store.checkpoint();
        other.compact();
        check(other.get("key")->view() == "other" && store.get("key")->view() == "data",
              "per-instance root changed another database");
        auto selected = p::read_current(path);
        check(selected.short_name && selected.filename().size() == 21, "new file name not short");
        for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(path)))
            check(entry.is_regular_file() && entry.path().filename().u8string().size() <= 21,
                  "database layout deep/long");
    }
    auto selected = p::read_current(path);
    auto short_name = selected.filename();
    selected.short_name = false;
    std::filesystem::rename(std::filesystem::u8path(p::join(path, short_name)),
                            std::filesystem::u8path(p::join(path, selected.filename())));
    auto short_companion = std::filesystem::u8path(p::join(path, short_name));
    auto legacy_companion = std::filesystem::u8path(p::join(path, selected.filename()));
    short_companion.replace_extension(".blk");
    legacy_companion.replace_extension(".blk");
    if (std::filesystem::exists(short_companion))
        std::filesystem::rename(short_companion, legacy_companion);
    check(p::write_current(path, selected) == FQDB_OK, "legacy manifest fixture");
    {
        Store store(path);
        check(store.get("key")->view() == "data", "legacy long-name open");
        {
            auto snapshot = store.snapshot();
            store.compact();
            check(p::read_current(path).short_name && snapshot.get("key")->view() == "data",
                  "legacy compaction conversion");
            check(std::filesystem::exists(
                      std::filesystem::u8path(p::join(path, selected.filename()))),
                  "legacy generation deleted while pinned");
        }
        check(!std::filesystem::exists(std::filesystem::u8path(p::join(path, selected.filename()))),
              "legacy generation not retired");
    }
    {
        struct RestoreDirectory {
            std::filesystem::path path = std::filesystem::current_path();
            ~RestoreDirectory() {
                std::error_code ignored;
                std::filesystem::current_path(path, ignored);
            }
        } restore;
        std::filesystem::current_path(std::filesystem::u8path(container));
        Store store("slot_1");
        std::filesystem::current_path(std::filesystem::u8path(fixture.root));
        put(store, "cwd", "anchored");
        store.compact();
        check(
            store.get("cwd")->view() == "anchored" &&
                !std::filesystem::exists(std::filesystem::u8path(p::join(fixture.root, "CURRENT"))),
            "CWD change redirected files");
    }
}
static void input_and_budget_boundaries() {
    Fixture fixture;
    auto path = fixture.name("raw");
    fqdb_store *raw = nullptr;
    check(test_support::open_strict(path.c_str(), nullptr, &raw) == FQDB_OK, "raw open");
    std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> store(raw, fqdb_store_close);
    fqdb_store_txn *tx = nullptr;
    check(fqdb_store_txn_begin(raw, 0, &tx) == FQDB_OK, "raw begin");
    std::unique_ptr<fqdb_store_txn, decltype(&fqdb_store_txn_close)> transaction(
        tx, fqdb_store_txn_close);
    fqdb_store_put_item invalid[] = {{{"good", 4}, {"value", 5}, 0}, {{"bad", 3}, {nullptr, 1}, 0}};
    check(fqdb_store_txn_put_many(tx, invalid, 2) == FQDB_INVALID_ARGUMENT,
          "bulk did not validate tail");
    fqdb_buffer bytes{};
    check(fqdb_store_txn_get(tx, {"good", 4}, &bytes) == FQDB_NOT_FOUND,
          "invalid bulk staged prefix");
    fqdb_store_batch *batch = nullptr;
    check(fqdb_store_batch_create(raw, invalid, 1, &batch) == FQDB_OK, "raw batch");
    std::unique_ptr<fqdb_store_batch, decltype(&fqdb_store_batch_close)> prepared(
        batch, fqdb_store_batch_close);
    uint64_t version = 0;
    check(fqdb_store_txn_commit(tx, &version) == FQDB_OK, "empty commit");
    check(fqdb_store_txn_put_batch(tx, batch) == FQDB_INVALID_ARGUMENT,
          "finished transaction accepted batch");
    check(fqdb_store_txn_get(tx, {"good", 4}, &bytes) == FQDB_INVALID_ARGUMENT,
          "finished transaction read");
    const auto &system = p::system_prefix();
    check(fqdb_store_txn_delete(tx, {system.data(), system.size()}) == FQDB_INVALID_ARGUMENT,
          "finished transaction system delete");
    fqdb_store_atomic_info result{1, 2, 3, 4, 5};
    check(fqdb_store_add_i64(raw, {"c", 1}, 1, 5, 0, 0, &result) == FQDB_INVALID_ARGUMENT &&
              !result.version && !result.observed_version && !result.value && !result.matched &&
              !result.changed,
          "atomic failure output");
    {
        Held hold(40);
        auto pending = std::async(std::launch::async, [&] {
            fqdb_store_atomic_info out{};
            return fqdb_store_add_i64(raw, {"c", 1}, 1, 0, 10, 0, &out);
        });
        reached();
        auto release = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hold.release();
        });
        result = {1, 2, 3, 4, 5};
        auto timed = fqdb_store_add_i64(raw, {"c", 1}, 1, 0, 10, 1, &result);
        release.get();
        check(pending.get() == FQDB_OK && timed == FQDB_TIMEOUT && !result.version &&
                  !result.changed,
              "queued atomic deadline");
    }
    fengqudb::StoreOptions limited;
    limited.pinned_bytes = 65536;
    Store small(fixture.name("pins"), limited);
    std::string value(128 * 1024, 'v');
    put(small, "large", value);
    auto pins = small.info().pinned_bytes;
    check(status([&] { small.get_many({"large"}); }) == FQDB_LIMIT &&
              small.info().pinned_bytes == pins,
          "failed results leaked budget");
    check(status([&] { small.prepare_batch({{"large", value}}); }) == FQDB_LIMIT &&
              small.info().pinned_bytes == pins,
          "failed batch leaked budget");
    check(status([&] { small.prepare_key("large").get_into(nullptr, 0, version); }) ==
                  FQDB_BUFFER_TOO_SMALL &&
              version == value.size(),
          "caller-buffer size probe charged value pins");
    limited.pinned_bytes = 4 * 1024 * 1024;
    limited.transaction_bytes = 1024 * 1024;
    Store bounded(fixture.name("txn-budget"), limited);
    auto large = bounded.prepare_batch({{"large", std::string(1024 * 1024, 'v')}});
    {
        auto edit = bounded.transaction();
        check(status([&] { edit.put_batch(large); }) == FQDB_LIMIT,
              "prepared payload bypassed transaction budget");
    }
    check(bounded.info().transaction_bytes == 0, "failed prepared apply leaked transaction budget");
}
int main() {
#ifdef __SANITIZE_THREAD__
    std::cout << "FQDB_TSAN_STARTED\n";
#endif
    try {
        atomic_operations();
        batches_and_prepared();
        concurrent_atomic();
        atomic_faults();
        paths_and_legacy_names();
        input_and_budget_boundaries();
        std::cout << "PASS Store atomic operations, prepared/bulk lifetimes, budgets, concurrent "
                     "grouped updates and shallow portable layout\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
