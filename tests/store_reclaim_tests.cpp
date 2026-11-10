#include "store_test_support.hpp"
#include "paged/reclaim.hpp"
#include <map>
static void commit_value(const std::shared_ptr<p::Core> &core, const std::string &name,
                         const std::string &value) {
    p::Transaction tx(core, 0);
    tx.put(name, value, 0);
    core->commit(tx);
}
static void verify_model(const std::shared_ptr<p::Core> &core,
                         const std::map<std::string, std::string> &model,
                         bool verify_structure = true) {
    auto view = core->current();
    for (const auto &item : model) {
        auto ref = view->find(item.first);
        check(ref && ref->visible(p::unix_ms()) &&
                  *view->generation->value(*ref, view->limit, false, false) == item.second,
              "reclaim changed a cold value");
    }
    if (verify_structure)
        core->verify(p::maintenance_defaults());
}
template <class F> static fqdb_status raw_status(F &&fn) {
    try {
        fn();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    }
}
static auto open_core(const std::string &path) {
    auto options = p::defaults();
    options.durability = FQDB_DURABILITY_BUFFERED;
    options.cache_bytes = 0;
    options.delta_bytes = 65536;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    return core;
}
static void garbage(const std::shared_ptr<p::Core> &core) {
    for (unsigned i = 0; i < 24; ++i)
        commit_value(core, "value", std::string(262144, static_cast<char>(i)));
    commit_value(core, "value", "latest");
    core->checkpoint();
}
static void cancellation_and_limits() {
    Fixture fixture;
    auto core = open_core(fixture.name("cancel"));
    garbage(core);
    {
        Held hold(73);
        auto task = std::async(std::launch::async, [&] {
            p::ReclaimSession session(*core);
            return raw_status([&] { session.step(p::maintenance_defaults()); });
        });
        reached();
        core->cancel_maintenance();
        const auto overlap = raw_status([&] { p::ReclaimSession session(*core); });
        hold.release();
        check(task.get() == FQDB_CANCELLED && overlap == FQDB_LOCKED &&
                  core->reclaim_state.load() == 0,
              "startup lost reclaim cancellation");
    }
    const auto generation = core->current()->generation;
    const auto tail = generation->tail;
    {
        p::ReclaimSession session(*core);
        auto policy = p::maintenance_defaults();
        policy.max_write_bytes =
            2 * (generation->checkpoint_payload(core->current()->checkpoint).size() + 56 + 64) - 1;
        check(raw_status([&] { session.step(policy); }) == FQDB_LIMIT && !core->poisoned &&
                  generation->tail == tail && generation->file->size() == tail,
              "insufficient double-root budget changed the source tail");
    }
    {
        p::ReclaimSession session(*core);
        std::unique_lock<p::WriterGate> lock(core->writer);
        auto task = std::async(std::launch::async, [&] {
            return raw_status([&] { session.step(p::maintenance_defaults()); });
        });
        core->cancel_maintenance();
        const auto ready = task.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        lock.unlock();
        check(ready, "cancelled reclaim remained blocked behind the writer");
        check(task.get() == FQDB_CANCELLED, "writer wait ignored cancellation");
    }
    {
        p::ReclaimSession session(*core);
        check(!session.step(p::maintenance_defaults()), "reclaim floor start");
        p::paged_fail_punch.store(1);
        check(raw_status([&] { session.step(p::maintenance_defaults()); }) == FQDB_IO_ERROR &&
                  !core->poisoned,
              "punch failure poisoned reachable data");
        p::paged_fail_punch.store(0);
        check(session.step(p::maintenance_defaults()), "punch failure was not retryable");
    }
    verify_model(core, {{"value", "latest"}});
    {
        p::ReclaimSession session(*core);
        check(!session.step(p::maintenance_defaults()), "generation-conflict floor");
        core->compact(p::maintenance_defaults());
        check(raw_status([&] { session.step(p::maintenance_defaults()); }) == FQDB_CONFLICT,
              "reclaim continued after generation replacement");
    }
    verify_model(core, {{"value", "latest"}});
    // A slice that appends unpublished folded pages must roll only those back.
    for (unsigned i = 0; i < 64; ++i)
        commit_value(core, "key/" + std::to_string(i), std::string(256, 'x'));
    const auto before = core->current()->generation->tail;
    {
        p::ReclaimSession session(*core);
        auto policy = p::maintenance_defaults();
        policy.max_write_bytes = 1000;
        check(raw_status([&] { session.step(policy); }) == FQDB_LIMIT && !core->poisoned &&
                  core->current()->generation->tail == before &&
                  core->current()->generation->file->size() == before,
              "failed delta fold left unpublished pages behind");
    }
    core->checkpoint();
    core->verify(p::maintenance_defaults());
}
struct StopWorker {
    std::shared_ptr<p::Core> core;
    ~StopWorker() {
        core->stop_worker();
    }
};
template <class F> static void await(F &&ready, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!ready()) {
        check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
static fqdb_store_runtime_options runtime() {
    return {sizeof(fqdb_store_runtime_options),
            1,
            1,
            10,
            256,
            64,
            0,
            30,
            1048576,
            524288,
            100,
            1048576,
            1048576};
}
static void buffered_allocation_pressure() {
    Fixture fixture;
    const auto path = fixture.name("allocation-pressure");
    {
        auto core = open_core(path);
        StopWorker stop{core};
        garbage(core);
        {
            p::ReclaimSession session(*core);
            while (!session.step(p::maintenance_defaults())) {
            }
        }
        for (unsigned round = 0; round < 40; ++round) {
            p::Transaction tx(core, 0);
            for (unsigned i = 0; i < 128; ++i)
                tx.put("key/" + std::to_string(i), std::string(4096, static_cast<char>(round)), 0);
            core->commit(tx);
        }
        {
            const auto view = core->current();
            check(view->generation->file->size() - view->checkpoint.end > 8 * 1048576,
                  "allocation-pressure fixture did not append an unsynced tail");
            std::cout << "buffered tail=" << view->generation->file->size() - view->checkpoint.end
                      << " reported_allocation=" << view->generation->file->allocated() << '\n';
        }
        // Windows sparse-file allocation metadata may omit this dirty tail
        // until a flush. Background triggering must not depend on that flush.
        struct DelayedAllocation {
            DelayedAllocation() {
                p::paged_allocation_ceiling.store(131072);
            }
            ~DelayedAllocation() {
                p::paged_allocation_ceiling.store(UINT64_MAX);
            }
        } delayed;
        auto policy = runtime();
        policy.compact_min_bytes = 8 * 1048576;
        core->configure_runtime(policy);
        await(
            [&] {
                return core->background_compactions.load() != 0 && core->reclaim_state.load() == 0;
            },
            "buffered sparse tail did not trigger background reclaim");
        core->stop_worker();
        p::paged_allocation_ceiling.store(UINT64_MAX);
        check(core->current()->generation->file->allocated() < 2 * 1048576,
              "buffered sparse tail was not physically reclaimed");
        std::map<std::string, std::string> model{{"value", "latest"}};
        for (unsigned i = 0; i < 128; ++i)
            model["key/" + std::to_string(i)] = std::string(4096, char(39));
        verify_model(core, model);
    }
    auto reopened = open_core(path);
    std::map<std::string, std::string> model{{"value", "latest"}};
    for (unsigned i = 0; i < 128; ++i)
        model["key/" + std::to_string(i)] = std::string(4096, char(39));
    verify_model(reopened, model);
}
static void background_waves() {
    Fixture fixture;
    const auto path = fixture.name("waves");
    std::map<std::string, std::string> model;
    {
        auto core = open_core(path);
        StopWorker stop{core};
        core->configure_runtime(runtime());
        for (unsigned wave = 0; wave < 2; ++wave) {
            const auto completed = core->background_compactions.load();
            std::vector<std::future<void>> writers;
            for (unsigned worker = 0; worker < 4; ++worker)
                writers.push_back(std::async(std::launch::async, [&, worker] {
                    for (unsigned round = 0; round < 40; ++round) {
                        p::Transaction tx(core, 0);
                        for (unsigned i = 0; i < 32; ++i) {
                            const auto key = "key/" + std::to_string(worker * 32 + i);
                            tx.put(key, std::string(4096, static_cast<char>(wave * 40 + round)), 0);
                        }
                        (void)core->commit(tx);
                    }
                }));
            for (auto &writer : writers)
                writer.get();
            // The growth threshold may leave a small final tail for the next cycle.
            try {
                await(
                    [&] {
                        return core->background_compactions.load() > completed &&
                               core->current()->generation->file->allocated() < 8 * 1048576 &&
                               core->reclaim_state.load() == 0;
                    },
                    "background slices did not converge");
            } catch (...) {
                const auto view = core->current();
                std::cerr << "background wave=" << wave << " version=" << view->version
                          << " generation=" << view->generation->number
                          << " logical=" << view->generation->file->size()
                          << " allocated=" << view->generation->file->allocated()
                          << " completed=" << core->background_compactions.load()
                          << " previous=" << completed
                          << " status=" << core->background_status.load()
                          << " reclaim=" << unsigned(core->reclaim_state.load())
                          << " running=" << core->background_running.load() << '\n';
                throw;
            }
            for (unsigned i = 0; i < 128; ++i)
                model["key/" + std::to_string(i)] =
                    std::string(4096, static_cast<char>(wave * 40 + 39));
            // Point reads are concurrent. Structural verify is an exclusive
            // maintenance job and may return LOCKED while this worker runs.
            // Validate the full structure below after stop_worker joins it.
            verify_model(core, model, false);
        }
        core->stop_worker();
        {
            const auto view = core->current();
            const auto allocated = view->generation->file->allocated();
            check(allocated < 8 * 1048576 && core->background_compactions.load() >= 2,
                  "physical allocation did not stabilize after background waves");
            std::cout << "background logical=" << view->generation->file->size()
                      << " allocated=" << allocated
                      << " completed=" << core->background_compactions.load() << '\n';
        }
        // Reconfiguration and cancellation must release the resumable session.
        garbage(core);
        Held hold(71);
        core->configure_runtime(runtime());
        reached();
        core->cancel_maintenance();
        hold.release();
        core->stop_worker();
        check(!core->background_running.load() && !core->reclaim_state.load(),
              "worker shutdown retained reclaim state");
        model["value"] = "latest";
        verify_model(core, model);
    }
    auto reopened = open_core(path);
    verify_model(reopened, model);
}
static void retained_reader_fallback() {
    Fixture fixture;
    auto core = open_core(fixture.name("fallback"));
    StopWorker stop{core};
    commit_value(core, "value", "snapshot");
    auto snapshot = std::make_shared<p::Capture>(core);
    garbage(core);
    auto previous = core->current()->generation->number;
    core->configure_runtime(runtime());
    await([&] { return core->current()->generation->number != previous; },
          "old reader did not trigger dense-generation fallback");
    core->stop_worker();
    auto ref = snapshot->view->find("value");
    check(ref && *snapshot->view->generation->value(*ref, snapshot->view->limit, false, false) ==
                     "snapshot",
          "fallback invalidated the old reader");
    verify_model(core, {{"value", "latest"}});
}
static void maintenance_lease_contract() {
    Fixture fixture;
    auto core = open_core(fixture.name("verify-lease"));
    StopWorker stop{core};
    garbage(core);
    Held hold(71);
    core->configure_runtime(runtime());
    reached();
    const auto occupied = raw_status([&] { core->verify(p::maintenance_defaults()); });
    verify_model(core, {{"value", "latest"}}, false);
    core->cancel_maintenance();
    hold.release();
    core->stop_worker();
    check(occupied == FQDB_LOCKED, "structural verify ignored the exclusive maintenance lease");
    verify_model(core, {{"value", "latest"}});
}
static void abandoned_reader_epochs() {
    Fixture fixture;
    auto core = open_core(fixture.name("abandoned"));
    commit_value(core, "value", std::string(262144, 's'));
    auto snapshot = std::make_shared<p::Capture>(core);
    garbage(core);
    {
        p::ReclaimSession session(*core);
        check(!session.step(p::maintenance_defaults()), "abandoned floor start");
        core->cancel_maintenance();
    }
    {
        p::ReclaimSession session(*core);
        check(!session.step(p::maintenance_defaults()), "replacement floor start");
        check(!session.step(p::maintenance_defaults()) && session.waiting_for_readers() &&
                  !session.punched,
              "replacement reclaim forgot an earlier reader epoch");
    }
    {
        p::ReclaimSession session(*core);
        check(!session.step(p::maintenance_defaults()) &&
                  !session.step(p::maintenance_defaults()) && !session.punched,
              "repeated abandon released an older reader");
        const auto ref = snapshot->view->find("value");
        check(ref && *snapshot->view->generation->value(*ref, snapshot->view->limit, false,
                                                        false) == std::string(262144, 's'),
              "abandoned session changed protected bytes");
        snapshot.reset();
        check(session.step(p::maintenance_defaults()), "expired abandoned epochs blocked reclaim");
    }
    verify_model(core, {{"value", "latest"}});
}
static void root_fallback() {
    for (unsigned damaged = 0; damaged < 2; ++damaged) {
        Fixture fixture;
        auto path = fixture.name("roots");
        {
            auto core = open_core(path);
            garbage(core);
            p::ReclaimSession session(*core);
            while (!session.step(p::maintenance_defaults())) {
            }
        }
        {
            auto selected = p::read_current(path);
            p::SharedFile file;
            file.open(p::join(path, selected.filename()), false, true);
            const char corrupt = 'x';
            file.write(4096 + damaged * 4096, &corrupt, 1);
            file.sync();
        }
        auto core = open_core(path);
        verify_model(core, {{"value", "latest"}});
    }
}
static void root_write_failures() {
    for (unsigned type = 0; type < 2; ++type)
        for (int point = 1; point <= 4; ++point) {
            Fixture fixture;
            const auto path = fixture.name("root-fault");
            uint64_t version;
            {
                auto core = open_core(path);
                garbage(core);
                version = core->current()->version;
                core->current()
                    ->generation->file->prepare_sparse(); // Inject into floor publication.
                p::ReclaimSession session(*core);
                fqdb_store_test_io_fault(type ? 0 : point, type ? point : 0);
                const auto result = raw_status([&] { session.step(p::maintenance_defaults()); });
                fqdb_store_test_io_fault(0, 0);
                check(result == FQDB_IO_ERROR && core->poisoned &&
                          raw_status([&] { commit_value(core, "forbidden", "x"); }) ==
                              FQDB_RECOVERY_REQUIRED,
                      "failed floor publication did not fence the writer");
            }
            auto core = open_core(path);
            check(core->current()->version == version, "floor I/O failure lost an accepted commit");
            verify_model(core, {{"value", "latest"}});
            commit_value(core, "later", "works");
        }
}
static void archives_and_watch() {
    Fixture fixture;
    const auto path = fixture.name("history"), base = fixture.name("base"),
               increment = fixture.name("increment"), restored = fixture.name("restored");
    fengqudb::StoreOptions options;
    options.durability = fengqudb::Durability::buffered;
    options.cache_bytes = 0;
    options.delta_bytes = 65536;
    Store db(path, options);
    for (unsigned i = 0; i < 24; ++i)
        put(db, "value", std::string(262144, static_cast<char>(i)));
    put(db, "value", "latest");
    fengqudb::StoreRuntimeOptions policy;
    policy.background = true;
    policy.interval_ms = 10;
    policy.compact_min_bytes = 1048576;
    policy.compact_growth_bytes = 1;
    policy.job_max_read_bytes = policy.job_max_write_bytes = 1048576;
    db.configure(policy);
    await([&] { return db.runtime_info().compactions != 0; },
          "public background reclaim did not run");
    db.configure();
    auto selected = p::read_current(path);
    {
        p::SharedFile file;
        file.open(p::join(path, selected.filename()), false, true, true);
        char magic[8];
        file.read(4096, magic, sizeof(magic));
        check(!std::memcmp(magic, "FQROOT05", 8), "public worker did not publish sparse roots");
    }
    const auto from = db.backup(base).snapshot_version;
    db.hold_history("backup", from);
    auto watch = db.watch(from);
    put(db, "after", "one");
    for (unsigned i = 0; i < 12; ++i)
        put(db, "value", std::string(262144, static_cast<char>(i)));
    put(db, "value", "retained");
    const auto completed = db.runtime_info().compactions;
    db.configure(policy);
    await([&] { return db.runtime_info().compactions > completed; },
          "retained history did not fall back to dense compaction");
    db.configure();
    check(db.history_info().floor == from && db.history_info().retained_generations != 0,
          "sparse floor discarded a persistent pin or watch history");
    unsigned seen = 0;
    while (auto batch = watch.next()) {
        for (uint32_t i = 0; i < batch->info().count; ++i)
            ++seen;
        watch.ack(batch->info().version);
    }
    check(seen == 14, "watch could not traverse a sparse ancestor after compaction");
    const auto version = db.info().version;
    db.backup_incremental(increment, from);
    Store::recover_chain(base, {increment}, restored);
    Store copy(restored, options);
    check(copy.info().version == version && copy.get("after")->view() == "one" &&
              copy.get("value")->view() == "retained",
          "sparse full/incremental recovery changed values");
    copy.verify();
    db.verify();
}
static void conversion_yields() {
    Fixture f;
    auto core = open_core(f.name("yield"));
    garbage(core);
    Held hold(86);
    auto future = std::async(std::launch::async, [&] {
        p::ReclaimSession session(*core);
        while (!session.step(p::maintenance_defaults())) {
        }
    });
    reached();
    auto writer = std::async(std::launch::async, [&] {
        commit_value(core, "during", "continues");
        core->checkpoint();
    });
    const bool progressed = writer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    hold.release();
    writer.get();
    future.get();
    check(progressed, "conversion held writer between slices");
    verify_model(core, {{"value", "latest"}, {"during", "continues"}});
}
static void crash_recovery(const std::string &executable) {
#ifndef __SANITIZE_THREAD__
    for (int point : {86, 74, 75, 71, 72}) {
        Fixture fixture;
        const auto path = fixture.name("crash");
        uint64_t version;
        {
            auto core = open_core(path);
            garbage(core);
            version = core->current()->version;
        }
        check(child(executable, "--reclaim", path, "unused", point) == 86,
              "reclaim crash point was not reached");
        auto core = open_core(path);
        check(core->current()->version == version, "reclaim crash lost an accepted version");
        verify_model(core, {{"value", "latest"}});
        commit_value(core, "later", "works");
        verify_model(core, {{"value", "latest"}, {"later", "works"}});
    }
#else
    (void)executable;
#endif
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--allocation-pressure") {
            buffered_allocation_pressure();
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--reclaim") {
            auto core = open_core(argv[2]);
            fqdb_store_test_crash(std::stoi(argv[4]));
            p::ReclaimSession session(*core);
            while (!session.step(p::maintenance_defaults())) {
            }
            return 88;
        }
        Fixture fixture;
        const auto path = fixture.name("reclaim");
        auto options = p::defaults();
        options.durability = FQDB_DURABILITY_BUFFERED;
        options.cache_bytes = 0;
        options.delta_bytes = 65536;
        std::map<std::string, std::string> model;
        {
            auto core = std::make_shared<p::Core>(path, options);
            core->open();
            core->write_policy.mode = false; // Strict-path fixture.
            for (unsigned i = 0; i < 48; ++i)
                commit_value(core, "obsolete", std::string(262144, static_cast<char>(i)));
            for (unsigned begin = 0; begin < 1024; begin += 32) {
                p::Transaction tx(core, 0);
                for (unsigned i = begin; i < begin + 32; ++i) {
                    auto key = "key/" + std::to_string(100000 + i);
                    model[key] = std::string(128, static_cast<char>(i % 127));
                    tx.put(key, model[key], 0);
                }
                core->commit(tx);
            }
            auto snapshot = std::make_shared<p::Capture>(core);
            commit_value(core, "obsolete", "new");
            model["obsolete"] = "new";
            model["patch"] = std::string(2097152, 'p');
            commit_value(core, "patch", model["patch"]);
            {
                p::Transaction tx(core, 0);
                tx.patch("patch", 128, 1900000, "cut");
                core->commit(tx);
                model["patch"].replace(128, 1900000, "cut");
            }
            p::ReclaimSession reclaim(*core);
            auto policy = p::maintenance_defaults();
            check(!reclaim.step(policy) && reclaim.started(), "reclaim floor did not start");
            auto generation = core->current()->generation;
            const auto logical = generation->file->size(),
                       allocated = generation->file->allocated();
            check(!reclaim.step(policy) && reclaim.waiting_for_readers() && !reclaim.punched,
                  "reclaim punched bytes while an older snapshot was alive");
            auto old = snapshot->view->find("obsolete");
            check(old && *generation->value(*old, snapshot->view->limit, false, false) ==
                             std::string(262144, 47),
                  "reclaim changed protected snapshot");
            snapshot.reset();
            commit_value(core, "appended", "after-floor");
            model["appended"] = "after-floor";
            {
                p::Transaction tx(core, 0);
                tx.patch("key/100000", 4, 5, "later");
                core->commit(tx);
                model["key/100000"].replace(4, 5, "later");
            }
            auto sliced = p::maintenance_defaults();
            sliced.max_write_bytes = 1048576;
            unsigned slices = 0;
            for (;;) {
                try {
                    if (reclaim.step(sliced))
                        break;
                } catch (const p::IOBudgetStop &) {
                }
                check(++slices < 64, "reclaim did not resume across I/O slices");
            }
            verify_model(core, model);
            const auto after = generation->file->allocated();
            check(generation->file->size() >= logical && after + 8388608 < allocated,
                  "reclaim did not free physical extents without moving live data");
            std::cout << "reclaim logical=" << generation->file->size()
                      << " allocated_before=" << allocated << " allocated_after=" << after
                      << " slices=" << slices << " punched=" << reclaim.punched << '\n';
            p::HistoryReader reader(core->current(), reclaim.version());
            uint64_t commits = 0;
            while (auto record = reader.next([] {})) {
                (void)record;
                ++commits;
            }
            check(commits == 2, "history reader did not start after the reclaimed floor");
            core->checkpoint();
        }
        {
            auto core = std::make_shared<p::Core>(path, options);
            core->open();
            core->write_policy.mode = false; // Strict-path fixture.
            verify_model(core, model);
            check(core->current()->checkpoint.history_start != 0,
                  "reclaim history boundary did not survive reopen");
            commit_value(core, "reopened", "works");
            model["reopened"] = "works";
            verify_model(core, model);
        }
        store_test_trace("reclaim/cancellation_and_limits");
        cancellation_and_limits();
        store_test_trace("reclaim/background_waves");
        buffered_allocation_pressure();
        background_waves();
        store_test_trace("reclaim/maintenance_lease_contract");
        maintenance_lease_contract();
        store_test_trace("reclaim/retained_reader_fallback");
        retained_reader_fallback();
        store_test_trace("reclaim/abandoned_reader_epochs");
        abandoned_reader_epochs();
        store_test_trace("reclaim/root_fallback");
        root_fallback();
        store_test_trace("reclaim/root_write_failures");
        root_write_failures();
        store_test_trace("reclaim/archives_and_watch");
        archives_and_watch();
        store_test_trace("reclaim/crash_recovery");
        conversion_yields();
        crash_recovery(std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string());
        std::cout << "PASS sparse reclaim slices, cancellation, worker lifecycle, reader epochs, "
                     "patch ancestry, double-root crashes, history and reopen\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
