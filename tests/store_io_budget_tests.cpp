#include "strict_store.hpp"
#include "store_test_support.hpp"
#include <fstream>

static_assert(offsetof(fqdb_store_maintenance_options, max_read_bytes) == 32);
static_assert(offsetof(fqdb_store_runtime_options, job_max_read_bytes) == 56);
template <class F> static fqdb_status paged_status(F &&function) {
    try {
        function();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    }
}
static std::shared_ptr<p::Core> core(Fixture &fixture, const char *name) {
    auto value = std::make_shared<p::Core>(fixture.name(name), p::defaults());
    value->open();
    value->write_policy.mode = false; // Strict-path fixture.
    return value;
}
static void write(p::Core &core, std::string_view key, std::string_view value) {
    p::Transaction tx(core.shared_from_this(), 0);
    tx.put(p::Bytes(key), value, 0);
    core.commit(tx);
}
static std::string file_bytes(const std::string &path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void old_prefix_compatibility() {
    Fixture fixture;
    auto path = fixture.name("abi");
    fqdb_store *db = nullptr;
    check(test_support::open_strict(path.c_str(), nullptr, &db) == FQDB_OK, "ABI fixture");
    struct OldMaintenance {
        uint32_t size, abi, rounds, reserved;
        uint64_t timeout, barrier;
    };
    static_assert(sizeof(OldMaintenance) == 32);
    OldMaintenance old{32, 1, 8, 0, 0, 1048576};
    fqdb_store_maintenance_info info{};
    check(fqdb_store_compact(db, reinterpret_cast<const fqdb_store_maintenance_options *>(&old),
                             &info) == FQDB_OK,
          "old 32-byte maintenance prefix rejected");
    old.abi = 2;
    info.final_version = 123;
    check(fqdb_store_verify(db, reinterpret_cast<const fqdb_store_maintenance_options *>(&old),
                            &info) == FQDB_INVALID_ARGUMENT &&
              info.final_version == 0,
          "undersized v2 maintenance accepted or output stale");
    struct OldRuntime {
        uint32_t size, abi, background, interval, expiry, group, delay, garbage;
        uint64_t minimum, growth, timeout;
    };
    static_assert(sizeof(OldRuntime) == 56);
    OldRuntime runtime{56, 1, 0, 1000, 256, 64, 0, 30, 67108864, 67108864, 1000};
    check(fqdb_store_configure(
              db, reinterpret_cast<const fqdb_store_runtime_options *>(&runtime)) == FQDB_OK,
          "old 56-byte runtime prefix rejected");
    runtime.abi = 2;
    check(fqdb_store_configure(db, reinterpret_cast<const fqdb_store_runtime_options *>(
                                       &runtime)) == FQDB_INVALID_ARGUMENT,
          "undersized v2 runtime accepted");
    fqdb_store_close(db);
}
static void wal_and_checkpoint_reservations() {
    Fixture fixture;
    auto source = core(fixture, "wal");
    write(*source, "key", "old");
    auto size = source->current()->generation->file->size(), version = source->current()->version;
    fqdb_store_maintenance_io io{999, 999, 999};
    auto policy = p::maintenance_defaults();
    policy.max_write_bytes = 100;
    policy.io_stats = &io;
    {
        p::MaintenanceIO scope(policy);
        check(paged_status([&] { write(*source, "key", std::string(64000, 'x')); }) == FQDB_LIMIT,
              "oversized WAL did not stop before append");
    }
    check(!fengqudb::storage::io_quota && !source->poisoned && io.write_bytes == 0 &&
              io.sync_calls == 0 && source->current()->version == version &&
              source->current()->generation->file->size() == size,
          "WAL quota left partial bytes, poison or leaked thread scope");
    write(*source, "key", "healthy");
    auto measured = core(fixture, "checkpoint-measure");
    write(*measured, "key", "value");
    auto measure = p::maintenance_defaults();
    fqdb_store_maintenance_io expected{};
    measure.io_stats = &expected;
    {
        p::MaintenanceIO scope(measure);
        measured->checkpoint();
    }
    check(expected.write_bytes > 64, "checkpoint I/O measurement");
    auto limited = core(fixture, "checkpoint-limited");
    write(*limited, "key", "value");
    auto sequence = limited->current()->checkpoint.sequence;
    policy.max_write_bytes = expected.write_bytes - 1;
    {
        p::MaintenanceIO scope(policy);
        check(paged_status([&] { limited->checkpoint(); }) == FQDB_LIMIT,
              "checkpoint quota not enforced");
    }
    check(!limited->poisoned && limited->current()->checkpoint.sequence == sequence &&
              io.write_bytes <= policy.max_write_bytes,
          "quota interrupted checkpoint publication");
    write(*limited, "after", "present");
    limited->checkpoint();
    limited->verify(p::maintenance_defaults());
}
static void copy_verify_restore() {
    Fixture fixture;
    auto path = fixture.name("source"), base = fixture.name("base"), again = fixture.name("again"),
         failed = fixture.name("failed"), restored = fixture.name("restored"),
         inc = fixture.name("inc"), chain = fixture.name("chain"),
         migrated = fixture.name("migrated");
    fengqudb::StoreOptions opts;
    opts.cache_bytes = 0;
    Store db(path, opts);
    const std::string value(64000, 'v');
    put(db, "key", value);
    fqdb_store_maintenance_io io{};
    fengqudb::StoreMaintenanceOptions options;
    options.io_stats = &io;
    options.max_read_bytes = 1;
    check(status([&] { db.verify(options); }) == FQDB_LIMIT && io.read_bytes <= 1 &&
              io.write_bytes == 0,
          "verify read quota");
    options.max_read_bytes = 0;
    db.verify(options);
    check(io.read_bytes >= value.size() && io.write_bytes == 0, "verify I/O stats");
    auto first = db.backup(base, options);
    auto baseline = io;
    check(baseline.read_bytes > 0 && baseline.write_bytes > value.size() && baseline.sync_calls > 0,
          "backup I/O stats");
    options.max_read_bytes = baseline.read_bytes;
    options.max_write_bytes = baseline.write_bytes;
    db.backup(again, options);
    check(io.read_bytes <= options.max_read_bytes && io.write_bytes <= options.max_write_bytes,
          "exact budget exceeded");
    options.max_read_bytes = 0;
    options.max_write_bytes = baseline.write_bytes - 1;
    auto before = p::read_current(path);
    check(status([&] { db.backup(failed, options); }) == FQDB_LIMIT &&
              !std::filesystem::exists(std::filesystem::u8path(failed)) &&
              io.write_bytes <= options.max_write_bytes,
          "failed output was published or budget exceeded");
    options.max_write_bytes = 1;
    check(status([&] { db.compact(options); }) == FQDB_LIMIT &&
              p::read_current(path).generation == before.generation &&
              db.get("key")->view() == value,
          "failed compact changed live database");
    options.max_write_bytes = 0;
    options.max_read_bytes = 1;
    check(status([&] { Store::restore(base, restored, options); }) == FQDB_LIMIT &&
              io.read_bytes <= 1 && io.write_bytes == 0,
          "restore source-open I/O escaped budget");
    options.max_read_bytes = 0;
    Store::restore(base, restored, options);
    db.hold_history("backup", first.snapshot_version);
    put(db, "key", "new");
    db.compact();
    options.max_write_bytes = 1;
    check(status([&] { db.backup_incremental(inc, first.snapshot_version, options); }) ==
                  FQDB_LIMIT &&
              io.write_bytes <= 1,
          "incremental output budget");
    options.max_write_bytes = 0;
    db.backup_incremental(inc, first.snapshot_version, options);
    options.max_read_bytes = 1;
    check(status([&] { Store::recover_chain(base, {inc}, chain, 0, 0, options); }) == FQDB_LIMIT &&
              io.read_bytes <= 1,
          "chain recovery source-open escaped budget");
    options.max_read_bytes = 0;
    Store::recover_chain(base, {inc}, chain, 0, 0, options);
}
static std::string document(uint64_t id) {
    fengqudb::wire::Writer out;
    out.uint_field(1, id);
    return out.take();
}
static void late_manifest_budget() {
    Fixture fixture;
    auto path = fixture.name("late");
    Store db(path);
    put(db, "key", std::string(64000, 'z'));
    fengqudb::StoreMaintenanceOptions policy;
    fqdb_store_maintenance_io io{};
    policy.io_stats = &io;
    db.compact(policy);
    auto writes = io.write_bytes;
    auto current = file_bytes(p::join(path, "CURRENT"));
    auto generation = p::read_current(path).generation;
    auto snapshot = db.snapshot();
    policy.max_write_bytes = writes - 1;
    check(status([&] { db.compact(policy); }) == FQDB_LIMIT &&
              file_bytes(p::join(path, "CURRENT")) == current &&
              p::read_current(path).generation == generation &&
              io.write_bytes <= policy.max_write_bytes,
          "late CURRENT quota changed selected generation");
    size_t files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(path))) {
        check(entry.is_regular_file(), "late quota leaked nested output");
        const auto name = entry.path().filename().u8string();
        check(name == "LOCK" || name == "CURRENT" || name == "WRITE" ||
                  name == p::read_current(path).filename(),
              "late quota leaked unknown file");
        ++files;
    }
    check(files == 4, "late quota did not remove rejected generation");
    db.put_if_changed("after", "healthy");
    check(!snapshot.get("after") && snapshot.get("key")->view() == std::string(64000, 'z'),
          "late quota affected snapshot or left poisoned writer");
    policy.max_write_bytes = 0;
    db.compact(policy);
    db.verify();
}
static void resumable_index_and_background() {
    Fixture fixture;
    Store db(fixture.name("index"));
    for (unsigned i = 0; i < 100; ++i)
        put(db, "doc/" + std::to_string(i), document(i));
    fengqudb::StoreMaintenanceOptions options;
    fqdb_store_maintenance_io io{};
    options.io_stats = &io;
    options.max_write_bytes = 1024;
    check(status([&] { db.define_index("by-id", "doc/", 1, 1, 3, options); }) == FQDB_LIMIT &&
              io.write_bytes <= 1024,
          "index budget did not leave bounded progress");
    db.put_if_changed("business", "alive");
    options.max_write_bytes = 8192;
    unsigned stops = 0;
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        auto result = status([&] { db.define_index("by-id", "doc/", 1, 1, 3, options); });
        check(io.write_bytes <= options.max_write_bytes, "resumed index budget exceeded");
        if (result == FQDB_OK)
            break;
        check(result == FQDB_LIMIT && ++stops < 29, "index could not resume after budget stop");
    }
    check(stops > 0 && db.execute({FQDB_INDEX_LOOKUP, "by-id", {}, document(99)}).rows.size() == 1,
          "resumed index not ready");
    db.verify();
    Store background(fixture.name("background"));
    for (unsigned i = 0; i < 4; ++i)
        put(background, "replace", std::string(64000, static_cast<char>(i)));
    fengqudb::StoreRuntimeOptions runtime;
    runtime.background = true;
    runtime.interval_ms = 10;
    runtime.compact_min_bytes = 1;
    runtime.compact_growth_bytes = 1;
    runtime.job_max_write_bytes = 1;
    background.configure(runtime);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (background.runtime_info().last_status != FQDB_LIMIT) {
        check(std::chrono::steady_clock::now() < deadline, "background quota status missing");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(background.runtime_info().compactions == 0, "background published over-budget output");
    for (unsigned i = 0; i < 10; ++i)
        background.put_if_changed("user/" + std::to_string(i), "unlimited");
    runtime.job_max_write_bytes = 0;
    background.configure(runtime);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (background.runtime_info().compactions == 0) {
        check(std::chrono::steady_clock::now() < deadline,
              "background did not recover after quota raised");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    background.configure();
    background.verify();
}
static void sync_failure_and_thread_isolation() {
    Fixture fixture;
    auto path = fixture.name("fault");
    {
        auto source = std::make_shared<p::Core>(path, p::defaults());
        source->open();
        source->write_policy.mode = false;                     // Strict-path fixture.
        source->current()->generation->file->prepare_sparse(); // Isolate the commit's sync fault.
        fqdb_store_maintenance_io io{};
        auto policy = p::maintenance_defaults();
        policy.io_stats = &io;
        std::future<fqdb_status> pending;
        Held hold(40);
        pending = std::async(std::launch::async, [&] {
            p::MaintenanceIO scope(policy);
            return paged_status([&] { write(*source, "pending", "value"); });
        });
        reached();
        check(source->current()->version == 0, "metered SYNC published before flush");
        fqdb_store_test_io_fault(0, 1);
        hold.release();
        auto result = pending.get();
        fqdb_store_test_io_fault(0, 0);
        check(result == FQDB_COMMIT_UNKNOWN && source->poisoned && io.sync_calls == 1 &&
                  io.write_bytes > 0,
              "metered sync uncertainty not preserved");
    }
    {
        Store db(path);
        check(db.get("pending")->view() == "value", "metered uncertainty recovery");
    }
    auto source = core(fixture, "concurrent");
    source->group_delay_us.store(10000);
    auto business = std::async(std::launch::async, [&] {
        return paged_status([&] { write(*source, "business", "ordinary"); });
    });
    auto limited = std::async(std::launch::async, [&] {
        auto policy = p::maintenance_defaults();
        policy.max_write_bytes = 1;
        p::MaintenanceIO scope(policy);
        return paged_status([&] { write(*source, "maintenance", "limited"); });
    });
    check(limited.get() == FQDB_LIMIT && business.get() == FQDB_OK && !source->poisoned,
          "maintenance quota leaked into ordinary grouped writer");
    check(!source->current()->find("maintenance") && source->current()->find("business"),
          "quota isolation state");
}
int main() {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        store_test_trace("old_prefix_compatibility");
        old_prefix_compatibility();
        store_test_trace("wal_and_checkpoint_reservations");
        wal_and_checkpoint_reservations();
        store_test_trace("copy_verify_restore");
        copy_verify_restore();
        store_test_trace("late_manifest_budget");
        late_manifest_budget();
        store_test_trace("resumable_index_and_background");
        resumable_index_and_background();
        store_test_trace("sync_failure_and_thread_isolation");
        sync_failure_and_thread_isolation();
        std::cout << "PASS maintenance I/O byte budgets, atomic preflight, ABI prefixes, stats, "
                     "rollback cleanup, index resume, background and writer isolation\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
