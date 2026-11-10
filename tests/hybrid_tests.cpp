#include "store_test_support.hpp"
#include "paged/reclaim.hpp"

static void reader_during_flush() {
    Fixture fixture;
    auto options = p::defaults();
    options.cache_bytes = 0;
    auto core = std::make_shared<p::Core>(fixture.name("readers"), options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    {
        p::Transaction tx(core, 0);
        for (unsigned i = 0; i < 256; ++i)
            tx.put("key/" + std::to_string(i), std::string(256, 'v'), 0);
        core->commit(tx);
    }
    auto old = core->current();
    p::paged_hold_data_sync.store(true);
    auto writer = std::async(std::launch::async, [&] {
        p::Transaction tx(core, 0);
        tx.put("new", std::string(256, 'n'), 0);
        core->commit(tx);
    });
    struct Release {
        ~Release() {
            p::paged_hold_data_sync.store(false);
        }
    } release;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!p::paged_data_sync_entered.load()) {
        if (std::chrono::steady_clock::now() > deadline) {
            p::paged_hold_data_sync.store(false);
            writer.get();
            check(false, "flush barrier not entered");
        }
        std::this_thread::yield();
    }
    auto reader = std::async(std::launch::async, [&] {
        const auto ref = old->find("key/100");
        return ref &&
               *old->generation->value(*ref, old->limit, false, false) == std::string(256, 'v');
    });
    const bool ready = reader.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
    p::paged_hold_data_sync.store(false);
    writer.get();
    check(ready && reader.get(), "cold reader waited for the persistent barrier");
    check(!old->find("new") && core->current()->find("new"), "flush exposed a partial view");
}
static void growth_and_reopen() {
    Fixture fixture;
    const auto path = fixture.name("growth");
    fengqudb::StoreOptions options;
    options.cache_bytes = 65536;
    const std::string big(12u << 20, 'b');
    {
        Store db(path, options);
        put(db, "a", big);
        auto snapshot = db.snapshot();
        put(db, "b", big);
        put(db, "c", big);
        put(db, "small", "retained");
        check(db.info().file_bytes > (32u << 20) && snapshot.get("a")->view() == big &&
                  !snapshot.get("b"),
              "growth boundary or snapshot");
        db.checkpoint();
        db.verify();
    }
    Store db(path, options);
    check(db.get("a")->view() == big && db.get("b")->view() == big && db.get("c")->view() == big &&
              db.get("small")->view() == "retained",
          "large hybrid reopen");
    check(db.info().cache_bytes <= options.cache_bytes, "business cache budget");
}
static void bounded_cells() {
    Fixture fixture;
    const auto directory = fixture.name("cells");
    std::filesystem::create_directory(std::filesystem::u8path(directory));
    const auto path = p::join(directory, "g0000000000000001.fdb");
    auto file = std::make_shared<p::NativeFile>();
    file->open(path, true);
    file->lock();
    p::ArenaFile cells(file);
    cells.create(p::new_uuid(), 1);
    std::array<char, 4032> bytes;
    bytes.fill('x');
    for (unsigned i = 0; i < 80; ++i) {
        cells.write(i * bytes.size(), bytes.data(), bytes.size());
        cells.sync();
    }
    check(cells.cached_bodies() <= 16, "small log retained every historical payload");
    std::array<char, 4032> read{};
    cells.read(0, read.data(), read.size());
    check(read == bytes, "evicted cell reload");
}
static void metered_conversion() {
    Fixture fixture;
    const auto path = fixture.name("conversion");
    auto options = p::defaults();
    options.cache_bytes = 0;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    {
        p::Transaction tx(core, 0);
        for (unsigned i = 0; i < 64; ++i)
            tx.put("key/" + std::to_string(i), std::string(2048, 'a'), 0);
        core->commit(tx);
    }
    auto old = core->current();
    auto file = old->generation->file;
    bool stopped = false;
    for (unsigned i = 0; file->accelerated() && i < 100; ++i) {
        auto policy = p::maintenance_defaults();
        policy.max_read_bytes = 65536;
        policy.max_write_bytes = 8192;
        fqdb_store_maintenance_io io{};
        policy.io_stats = &io;
        try {
            p::MaintenanceIO meter(policy);
            file->prepare_sparse();
        } catch (const p::Error &e) {
            check(e.status == FQDB_LIMIT, "conversion error");
            stopped = true;
        }
        check(io.write_bytes <= 8192 && io.read_bytes <= 65536, "conversion exceeded slice budget");
        if (i == 0) {
            p::Transaction tx(core, 0);
            tx.put("during", "alive", 0);
            core->commit(tx);
        }
    }
    check(stopped && !file->accelerated(), "conversion did not resume across small budgets");
    const auto ref = old->find("key/3");
    check(ref && *old->generation->value(*ref, old->limit, false, false) == std::string(2048, 'a'),
          "conversion changed snapshot offsets");
    check(!old->find("during") && core->current()->find("during"),
          "conversion lost intervening append");
    file.reset();
    old.reset();
    core.reset();
    Store reopened(path);
    check(reopened.get("during")->view() == "alive", "converted image reopen");
    reopened.verify();
}
template <class F> static fqdb_status result_of(F &&fn) {
    try {
        fn();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    }
}
static void active_tail_cache() {
    Fixture fixture;
    const auto directory = fixture.name("tail-cache");
    std::filesystem::create_directory(std::filesystem::u8path(directory));
    auto file = std::make_shared<p::NativeFile>();
    file->open(p::join(directory, "g0000000000000001.fdb"), true);
    file->lock();
    p::ArenaFile cells(file);
    cells.create(p::new_uuid(), 1);
    std::string payload(300000, 'a');
    cells.prepare(0, payload.size());
    cells.write(0, payload.data(), payload.size());
    cells.sync();
    cells.prepare(payload.size(), 32768); // Grow spare capacity without extending the live tail.
    const auto reads = file->reads.load();
    for (unsigned n = 0; n < 8; ++n) {
        const char next = static_cast<char>('b' + n);
        cells.write(payload.size(), &next, 1);
        payload.push_back(next);
        cells.sync();
    }
    check(file->reads.load() == reads, "reserved spare pages evicted the active append page");
    check(cells.cached_bodies() <= 16, "active-tail cache grew past its body budget");
    std::string actual(payload.size(), '\0');
    cells.read(0, actual.data(), actual.size());
    check(actual == payload, "tail cache changed historical bytes");
}
static void publication_failure_fences() {
    for (int point : {1, 2})
        for (unsigned operation = 0; operation < 3; ++operation) {
            Fixture fixture;
            const auto path = fixture.name("publication");
            auto core = std::make_shared<p::Core>(path, p::defaults());
            core->open();
            core->write_policy.mode = false; // Strict-path fixture.
            {
                p::Transaction tx(core, 0);
                tx.put("confirmed", "keep", 0);
                core->commit(tx);
            }
            const auto version = core->current()->version;
            auto file = core->current()->generation->file;
            p::hybrid_publish_fault.store(point);
            struct Reset {
                ~Reset() {
                    p::hybrid_publish_fault.store(0);
                }
            } reset;
            fqdb_status code = FQDB_OK;
            if (operation == 0) {
                auto policy = p::maintenance_defaults();
                fqdb_store_maintenance_io io{};
                policy.io_stats = &io;
                p::MaintenanceIO meter(policy);
                p::Transaction tx(core, 0);
                tx.put("unstarted", "discard", 0);
                code = result_of([&] { core->commit(tx); });
            } else if (operation == 1) {
                auto policy = p::maintenance_defaults();
                fqdb_store_maintenance_io io{};
                policy.io_stats = &io;
                p::MaintenanceIO meter(policy);
                code = result_of([&] { core->checkpoint(); });
            } else {
                p::ReclaimSession session(*core);
                code = result_of([&] { session.step(p::maintenance_defaults()); });
            }
            check(code == FQDB_COMMIT_UNKNOWN && core->poisoned,
                  "uncertain image publication left the Store writer usable");
            check(core->current()->version == version,
                  "failed conversion published an unstarted commit");
            const auto writes = file->writes.load();
            const char byte = 'x';
            check(result_of([&] { file->write(8192, &byte, 1); }) == FQDB_COMMIT_UNKNOWN &&
                      result_of([&] {
                          file->write_known(8192, &byte, 1, fengqudb::storage::crc32(&byte, 1));
                      }) == FQDB_COMMIT_UNKNOWN &&
                      result_of([&] { file->reservation_bytes(0, 1); }) == FQDB_COMMIT_UNKNOWN &&
                      result_of([&] { file->sync(); }) == FQDB_COMMIT_UNKNOWN &&
                      file->writes.load() == writes,
                  "failed direct backend accepted another write");
            {
                p::Transaction refused(core, 0);
                refused.put("forbidden", "no", 0);
                check(result_of([&] { core->commit(refused); }) == FQDB_RECOVERY_REQUIRED &&
                          result_of([&] { core->checkpoint(); }) == FQDB_RECOVERY_REQUIRED &&
                          result_of([&] { core->sync(); }) == FQDB_RECOVERY_REQUIRED,
                      "uncertain Store did not require reopen");
            }
            file.reset();
            core.reset();
            Store reopened(path);
            check(reopened.get("confirmed")->view() == "keep" && !reopened.get("unstarted") &&
                      !reopened.get("forbidden"),
                  "image publication failure changed confirmed data");
            put(reopened, "after", "healthy");
            reopened.verify();
        }
}
static void readonly_mutation_rejected() {
    Fixture fixture;
    const auto path = fixture.name("readonly");
    {
        Store db(path);
        put(db, "key", "value");
    }
    const auto current = p::read_current(path);
    p::SharedFile file;
    file.open(p::join(path, current.filename()), false, true, true);
    file.lock();
    const auto length = file.size(), writes = file.writes.load();
    const char byte = 'x';
    check(result_of([&] { file.reservation_bytes(length, 1); }) == FQDB_UNSUPPORTED &&
              result_of([&] { file.write(8192, &byte, 1); }) == FQDB_UNSUPPORTED &&
              result_of([&] {
                  file.write_known(8192, &byte, 1, fengqudb::storage::crc32(&byte, 1));
              }) == FQDB_UNSUPPORTED &&
              result_of([&] { file.truncate(length + 1); }) == FQDB_UNSUPPORTED &&
              result_of([&] { file.sync(); }) == FQDB_UNSUPPORTED && file.size() == length &&
              file.writes.load() == writes,
          "read-only hybrid accepted a staged mutation");
}
static void conversion_reused_tail() {
    Fixture fixture;
    const auto directory = fixture.name("reused-tail");
    std::filesystem::create_directory(std::filesystem::u8path(directory));
    const auto path = p::join(directory, "g0000000000000001.fdb");
    auto root = std::make_shared<p::NativeFile>();
    root->open(path, true);
    root->lock();
    p::HybridFile file(root, path);
    file.create(p::new_uuid(), 1);
    file.truncate(12288);
    std::string payload(4u << 20, 'a');
    file.write(12288, payload.data(), payload.size());
    file.sync();
    check(!file.materialize(2u << 20, 1u << 20), "conversion unexpectedly finished");
    file.sync_materialization();
    file.truncate(12288);
    payload.assign(3u << 20, 'b');
    file.write(12288, payload.data(), payload.size());
    file.sync();
    while (!file.prepare_sparse_slice())
        file.sync_materialization();
    std::string actual(payload.size(), '\0');
    file.read(12288, actual.data(), actual.size());
    check(actual == payload && file.size() == 12288 + payload.size(),
          "conversion published reused tail bytes");
}
int main() try {
    reader_during_flush();
    growth_and_reopen();
    bounded_cells();
    metered_conversion();
    active_tail_cache();
    publication_failure_fences();
    readonly_mutation_rejected();
    conversion_reused_tail();
    std::cout << "PASS flush/read overlap, growth beyond 32MiB, bounded cell bodies and resumable "
                 "metered image\n";
} catch (const std::exception &e) {
    p::paged_hold_data_sync.store(false);
    std::cerr << e.what() << '\n';
    return 1;
}
