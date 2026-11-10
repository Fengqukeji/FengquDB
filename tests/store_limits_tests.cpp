#include "store_test_support.hpp"
#include "store_clock.hpp"
#include "paged/features.hpp"
#include <atomic>
#include <fstream>
#include <limits>

template <class F> static fqdb_status code(F &&operation) {
    try {
        operation();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    } catch (const fengqudb::Error &error) {
        return error.code();
    }
}
static std::string data_path(const std::string &path) {
    return p::join(path, p::read_current(path).filename());
}
static std::string bytes(const std::string &path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    check(bool(input), "read owned fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
// Valid format fixtures jump to the numeric boundary instead of executing
// billions of operations. This helper is never linked into the runtime library.
static void seed(const std::string &path, uint64_t version, uint64_t sequence = 1,
                 uint64_t number = 1) {
    check(sequence && number, "nonzero fixture sequence/generation");
    check(std::filesystem::create_directory(std::filesystem::u8path(path)), "fresh fixture");
    auto uuid = p::new_uuid();
    auto generation = std::make_shared<p::Generation>(0);
    generation->create(p::join(path, p::generation_name(uuid, number)), uuid, number);
    p::Checkpoint checkpoint;
    checkpoint.version = checkpoint.floor = version;
    checkpoint.sequence = sequence - 1;
    checkpoint.time = p::unix_ms();
    generation->seal(checkpoint);
    check(p::write_current(path, {uuid, number}) == FQDB_OK, "fixture CURRENT");
}
static void sequence_exhaustion() {
    Fixture fixture;
    auto path = fixture.name("sequence");
    seed(path, 0, UINT64_MAX);
    auto original = bytes(data_path(path));
    {
        Store store(path);
        check(code([&] { store.put_if_changed("rejected", "value"); }) == FQDB_LIMIT,
              "exhausted sequence accepted an unrecoverable WAL");
        check(store.info().version == 0 && !store.get("rejected"), "rejected write published");
        store.checkpoint(); // Clean exhausted roots remain usable.
        store.verify();
    }
    check(bytes(data_path(path)) == original, "rejected exhausted write changed bytes");
    {
        Store store(path);
        store.compact(); // New per-file sequence, same logical version.
        check(store.put_if_changed("accepted", "value").version == 1,
              "compaction did not reset exhausted sequence");
    }
    {
        Store reopened(path);
        check(reopened.get("accepted")->view() == "value", "sequence restart");
    }

    auto last = fixture.name("last-sequence");
    seed(last, 0, UINT64_MAX - 1);
    {
        Store store(last);
        store.put_if_changed("last", "recoverable");
    }
    {
        Store store(last); // Recovery consumes the final checkpoint sequence.
        check(store.get("last")->view() == "recoverable", "last sequence failed to recover");
        check(code([&] { store.put_if_changed("extra", "no"); }) == FQDB_LIMIT,
              "recovered exhausted file accepted more WAL");
        store.compact();
        store.put_if_changed("after", "yes");
        store.verify();
    }

    // Simulate a legacy in-memory view at exhaustion with pending metadata.
    auto pending = fixture.name("pending-fold");
    auto core = std::make_shared<p::Core>(pending, p::defaults());
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    p::Transaction tx(core, 0);
    tx.put("key", "value", 0);
    core->commit(tx);
    auto view = std::make_shared<p::View>(*core->current());
    view->checkpoint.sequence = UINT64_MAX;
    core->publish(view);
    auto size = view->generation->file->size();
    check(code([&] { core->checkpoint(); }) == FQDB_LIMIT &&
              view->generation->file->size() == size && view->generation->tail == size,
          "checkpoint appended pages before detecting sequence exhaustion");
    core->compact(p::maintenance_defaults());
}
static void exhausted_recovery_preserves_source() {
    Fixture fixture;
    for (bool complete : {false, true}) {
        auto path = fixture.name(complete ? "complete-tail" : "partial-tail");
        seed(path, 5, UINT64_MAX);
        auto current = p::read_current(path);
        auto file = data_path(path);
        {
            auto generation = std::make_shared<p::Generation>(0);
            generation->open(file, current.uuid, current.generation);
            if (complete) {
                p::WalBatch wal(6, p::unix_ms());
                wal.add(p::OpKind::put, "legacy", std::make_shared<p::Data>("confirmed"));
                wal.append(*generation);
            } else {
                const std::string partial = "FQTXN004";
                generation->file->write(generation->tail, partial.data(), partial.size());
            }
            generation->file->sync();
        }
        auto original = bytes(file);
        check(code([&] { Store store(path); }) == FQDB_LIMIT,
              "exhausted legacy recovery unexpectedly resealed");
        check(bytes(file) == original, "failed exhausted recovery mutated the source");
    }
}
static void last_version_and_generation() {
    Fixture fixture;
    auto path = fixture.name("last-version");
    seed(path, UINT64_MAX - 1);
    std::string winner;
    {
        Store store(path);
        fengqudb::StoreRuntimeOptions runtime;
        runtime.group_delay_us = 1000;
        store.configure(runtime);
        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::future<fqdb_status>> results;
        for (unsigned i = 0; i < 4; ++i)
            results.push_back(std::async(std::launch::async, [&, i] {
                ++ready;
                while (!start.load())
                    std::this_thread::yield();
                return code([&] { store.put_if_changed("key" + std::to_string(i), "last"); });
            }));
        while (ready.load() != 4)
            std::this_thread::yield();
        start = true;
        unsigned accepted = 0;
        for (unsigned i = 0; i < 4; ++i) {
            auto status = results[i].get();
            check(status == FQDB_OK || status == FQDB_LIMIT, "last-version group status");
            if (status == FQDB_OK) {
                ++accepted;
                winner = "key" + std::to_string(i);
            }
        }
        check(accepted == 1 && store.info().version == UINT64_MAX &&
                  store.info().durable_version == UINT64_MAX,
              "version wrapped or published multiple final commits");
        check(!store.put_if_changed(winner, "last").changed, "no-op needs another version");
        auto empty = store.transaction();
        check(empty.commit().version == UINT64_MAX, "empty final commit");
    }
    {
        Store store(path);
        check(store.get(winner)->version() == UINT64_MAX, "final WAL restart");
        store.checkpoint();
        store.compact();
        store.verify();
        check(code([&] { store.put_if_changed("extra", "no"); }) == FQDB_LIMIT,
              "version reset by compact");
    }

    auto last = fixture.name("last-generation"), backup = fixture.name("generation-backup");
    seed(last, 0, 1, UINT64_MAX);
    auto manifest = bytes(p::join(last, "CURRENT"));
    {
        Store store(last);
        check(code([&] { store.compact(); }) == FQDB_LIMIT, "generation wrapped");
        check(bytes(p::join(last, "CURRENT")) == manifest, "failed compaction changed CURRENT");
        store.put_if_changed("still", "writable");
        store.backup(backup);
        store.verify();
    }
    {
        Store store(backup);
        store.compact();
        store.put_if_changed("independent", "yes");
    }
}
static void logical_limits() {
    Fixture fixture;
    auto core = std::make_shared<p::Core>(fixture.name("savepoint"), p::defaults());
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    p::Transaction tx(core, 0);
    tx.next_savepoint = UINT64_MAX - 1;
    auto saved = tx.savepoint();
    check(saved == UINT64_MAX, "last savepoint");
    tx.put("private", "no", 0);
    check(code([&] { tx.savepoint(); }) == FQDB_LIMIT, "savepoint wrapped");
    tx.rollback_to(saved);
    check(!tx.try_get("private"), "last savepoint rollback");

    for (uint64_t kind : {1u, 2u, 3u, 4u}) {
        p::CollectionMeta invalid(kind);
        invalid.count = UINT64_MAX;
        if (kind == 3) {
            invalid.head = 0;
            invalid.tail = UINT64_MAX;
        }
        check(code([&] { p::CollectionMeta decoded(invalid.encode()); }) == FQDB_CORRUPT,
              "malformed count accepted before an increment could wrap");
    }
    Store store(fixture.name("public-limits"));
    check(code([&] { auto tx = store.transaction(UINT64_MAX); }) == FQDB_INVALID_ARGUMENT,
          "timeout duration overflow");
    fengqudb::StoreMaintenanceOptions options;
    options.timeout_ms = UINT64_MAX;
    check(code([&] { store.verify(options); }) == FQDB_INVALID_ARGUMENT,
          "maintenance timeout overflow");
    auto watch = store.watch();
    check(code([&] { watch.next(UINT64_MAX); }) == FQDB_INVALID_ARGUMENT, "watch timeout overflow");
    store.put_if_changed("still", "usable");
    store.execute({FQDB_LIST_PUSH_RIGHT, "list", {}, "item"});
    check(code([&] { store.execute({FQDB_LIST_GET, "list", {}, {}, INT64_MIN}); }) ==
              FQDB_NOT_FOUND,
          "minimum signed list index overflow");
    for (double value :
         {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()})
        check(code([&] { store.execute({FQDB_SORTED_ADD, "scores", "bad", {}, 0, value}); }) ==
                  FQDB_INVALID_ARGUMENT,
              "nonfinite sorted score accepted");
    store.verify();
    {
        StoreTestClock time(UINT64_MAX - 1);
        auto expiry = store.transaction();
        expiry.put("ttl", "last-ms", UINT64_MAX);
        commit(expiry);
        store.checkpoint();
        check(store.get("ttl").has_value(), "last TTL prematurely expired");
        time.set(UINT64_MAX);
        check(!store.get("ttl") && store.purge_expired().removed_keys == 1 &&
                  store.get("still")->view() == "usable",
              "max TTL/no-expiry sentinel collision");
        store.verify();
    }
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cout << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 6 && std::string(argv[1]) == "--seed") {
            seed(argv[2], std::stoull(argv[3]), std::stoull(argv[4]), std::stoull(argv[5]));
            return 0;
        }
        check(argc == 1, "numeric test arguments");
        sequence_exhaustion();
        exhausted_recovery_preserves_source();
        last_version_and_generation();
        logical_limits();
        std::cout
            << "PASS numeric exhaustion, recovery source preservation, grouped final version, "
               "generation rollover guards, savepoints, counts, TTL and durations\n";
    } catch (const p::Error &error) {
        std::cerr << "Native status " << error.status << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
