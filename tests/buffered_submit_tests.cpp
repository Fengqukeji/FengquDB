#include "store_test_support.hpp"
#include <cstdlib>

using namespace std::chrono_literals;
static void need(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << std::endl;
        std::abort(); // Never block unwinding an async test behind its held writer.
    }
}
template <class F> static void await(F condition) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!condition()) {
        need(std::chrono::steady_clock::now() < end, "probe barrier timeout");
        std::this_thread::sleep_for(1ms);
    }
}
static size_t pending(p::Core &core) {
    std::lock_guard<std::mutex> lock(core.group_mutex);
    return core.group_count;
}
static auto open_core(const std::string &path, bool segmented = false) {
    auto options = p::defaults();
    options.durability = FQDB_DURABILITY_BUFFERED;
    options.flags |= segmented ? FQDB_STORE_SEGMENTED : 0;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    return core;
}
static auto launch(const std::shared_ptr<p::Core> &core, p::Transaction &tx) {
    return std::async(std::launch::async, [core, &tx]() -> fqdb_status {
        try {
            core->commit(tx);
            return FQDB_OK;
        } catch (const p::Error &error) {
            return error.status;
        } catch (...) {
            return FQDB_INTERNAL_ERROR;
        }
    });
}
static fqdb_status take(std::future<fqdb_status> &future) {
    need(future.wait_for(3s) == std::future_status::ready, "probe request hung");
    return future.get();
}
static std::string read(const p::View &view, const char *key) {
    auto value = p::try_read(view, p::unix_ms(), key);
    return value ? std::string(value->data(), value->size()) : std::string{};
}
static void conflict(uint32_t maximum, bool segmented) {
    Fixture owned;
    const auto path = owned.name("conflict");
    {
        auto core = open_core(path, segmented);
        core->group_max.store(maximum);
        p::Transaction seed(core, 0);
        seed.put("a", std::string(4096, 'a'), 0);
        core->commit(seed);
        auto old = std::make_shared<p::Capture>(core);
        p::Transaction first(core, 0), second(core, 0);
        first.put("a", std::string(4096, 'b'), 0);
        second.put("a", std::string(4096, 'c'), 0);
        std::unique_lock<p::WriterGate> hold(core->writer);
        auto a = launch(core, first);
        await([&] { return core->buffered_active.load() != 0; });
        auto b = launch(core, second);
        await([&] { return pending(*core) == 1; });
        hold.unlock();
        need(take(a) == FQDB_OK, "first conflicting write failed");
        need(take(b) == FQDB_CONFLICT, "second conflicting write was accepted");
        need(read(*core->current(), "a") == std::string(4096, 'b'), "conflict leaked bytes");
        need(read(*old->view, "a") == std::string(4096, 'a'), "old snapshot changed");
        need(!core->buffered_active.load() && !pending(*core), "writer state leaked");
        core->sync();
    }
    auto core = open_core(path, segmented);
    need(read(*core->current(), "a") == std::string(4096, 'b'), "conflict reopen mismatch");
}
static void withdrawals(bool cancel_leader, bool segmented) {
    Fixture owned;
    const auto path = owned.name("withdraw");
    {
        auto core = open_core(path, segmented);
        p::Transaction first(core, 0), cancelled(core, 0), expired(core, 500), survivor(core, 0);
        first.put("first", std::string(4096, 'a'), 0);
        cancelled.put("cancelled", std::string(4096, 'b'), 0);
        expired.put("expired", std::string(4096, 'c'), 0);
        survivor.put("survivor", std::string(4096, 'd'), 0);
        std::unique_lock<p::WriterGate> hold(core->writer);
        auto a = launch(core, first);
        await([&] { return core->buffered_active.load() != 0; });
        auto b = launch(core, cancelled);
        await([&] { return pending(*core) == 1; });
        auto c = launch(core, expired);
        await([&] { return pending(*core) == 2; });
        auto d = launch(core, survivor);
        await([&] { return pending(*core) == 3; });
        cancelled.cancelled.store(true);
        need(take(b) == FQDB_CANCELLED, "queued cancel failed");
        need(take(c) == FQDB_TIMEOUT, "queued timeout failed");
        if (cancel_leader) {
            first.cancelled.store(true);
            need(take(a) == FQDB_CANCELLED, "leader cancel failed");
            await([&] { return pending(*core) == 0; });
        }
        need(core->current()->version == 0, "write passed held writer");
        hold.unlock();
        if (!cancel_leader)
            need(take(a) == FQDB_OK, "leader failed");
        need(take(d) == FQDB_OK, "survivor failed");
        need(read(*core->current(), "cancelled").empty() &&
                 read(*core->current(), "expired").empty(),
             "withdrawn request wrote bytes");
        need(read(*core->current(), "survivor") == std::string(4096, 'd'), "survivor mismatch");
        need(!core->buffered_active.load() && !pending(*core), "withdraw writer state leaked");
        core->sync();
    }
    auto core = open_core(path, segmented);
    need(read(*core->current(), "survivor") == std::string(4096, 'd'), "withdraw reopen mismatch");
    need(read(*core->current(), "cancelled").empty() && read(*core->current(), "expired").empty(),
         "withdrawn request recovered");
}

static void detached_request(bool segmented, bool fail_write) {
    Fixture fixture;
    const auto path = fixture.name("detached");
    {
        auto core = open_core(path, segmented);
        auto old = std::make_shared<p::Capture>(core);
        p::Transaction first(core, 0), second(core, 0), third(core, 0);
        first.put("first", std::string(4096, 'a'), 0);
        second.put("second", std::string(4096, 'b'), 0);
        third.put("third", std::string(4096, 'c'), 0);
        std::unique_lock<p::WriterGate> hold(core->writer);
        auto a = launch(core, first);
        await([&] { return core->buffered_active.load() != 0; });
        auto b = launch(core, second);
        await([&] { return pending(*core) == 1; });
        auto c = launch(core, third);
        await([&] { return pending(*core) == 2; });
        Held detached(73);
        hold.unlock();
        reached();
        need(read(*core->current(), "first") == std::string(4096, 'a'),
             "first transaction was not independently published");
        need(read(*old->view, "first").empty(), "old snapshot saw pending update");
        if (fail_write) {
            // The leader holds the writer and is paused at the test barrier.
            // Establish a durable prefix before injecting a later WAL failure.
            core->current()->generation->file->sync();
            fqdb_store_test_io_fault(1, 0);
        } else
            second.cancelled.store(true);
        need(b.wait_for(10ms) == std::future_status::timeout,
             "detached caller returned before leader released its request");
        detached.release();
        need(take(a) == FQDB_OK, "later failure changed confirmed first result");
        need(take(b) == (fail_write ? FQDB_COMMIT_UNKNOWN : FQDB_CANCELLED),
             "detached request status mismatch");
        need(take(c) == (fail_write ? FQDB_RECOVERY_REQUIRED : FQDB_OK),
             "successor did not preserve poison or continue normally");
        fqdb_store_test_io_fault(0, 0);
        need(!pending(*core) && !core->buffered_active.load(), "detached state leaked");
        need(read(*core->current(), "second").empty(), "failed detached write was published");
        if (!fail_write)
            core->sync();
    }
    auto reopened = open_core(path, segmented);
    need(read(*reopened->current(), "first") == std::string(4096, 'a'),
         "confirmed first write missing after reopen");
    need(read(*reopened->current(), "second").empty(), "failed tail survived recovery");
    need(read(*reopened->current(), "third") ==
             (fail_write ? std::string{} : std::string(4096, 'c')),
         "successor recovery mismatch");
}

static void metered_caller(bool segmented) {
    Fixture fixture;
    auto core = open_core(fixture.name("metered"), segmented);
    p::Transaction first(core, 0);
    first.put("first", std::string(4096, 'a'), 0);
    std::unique_lock<p::WriterGate> hold(core->writer);
    auto leader = launch(core, first);
    await([&] { return core->buffered_active.load() != 0; });
    std::atomic<unsigned> checks{0};
    fqdb_store_maintenance_io io{};
    auto metered = std::async(std::launch::async, [&]() -> fqdb_status {
        const auto caller = std::this_thread::get_id();
        auto policy = p::maintenance_defaults();
        policy.max_write_bytes = 100;
        policy.io_stats = &io;
        p::MaintenanceIO quota(policy);
        p::Transaction tx(core, 0);
        tx.put("metered", std::string(4096, 'b'), 0);
        try {
            core->commit_checked(tx, nullptr, [&] {
                need(std::this_thread::get_id() == caller, "metered check migrated threads");
                checks.fetch_add(1);
            });
            return FQDB_OK;
        } catch (const p::Error &error) {
            return error.status;
        }
    });
    await([&] { return checks.load() >= 3; });
    need(!pending(*core), "metered request entered the delegation queue");
    hold.unlock();
    need(take(leader) == FQDB_OK, "unmetered leader consumed another caller quota");
    need(take(metered) == FQDB_LIMIT && io.write_bytes == 0 && io.sync_calls == 0,
         "metered caller did not stop before WAL or leaked its quota");
    need(!core->poisoned && !pending(*core) && !core->buffered_active.load(),
         "metered cancellation poisoned or stranded the writer");
    need(read(*core->current(), "metered").empty(), "metered write became visible");
}

static void repeated_handoffs(bool segmented) {
    Fixture fixture;
    const auto path = fixture.name("handoffs");
    {
        auto core = open_core(path, segmented);
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::future<void>> workers;
        for (unsigned thread = 0; thread < 8; ++thread)
            workers.push_back(std::async(std::launch::async, [&, thread] {
                ready.fetch_add(1);
                while (!go.load())
                    std::this_thread::yield();
                for (unsigned iteration = 0; iteration < 24; ++iteration) {
                    p::Transaction tx(core, 0);
                    tx.put("worker" + std::to_string(thread),
                           std::string(4096, static_cast<char>('a' + iteration)), 0);
                    core->commit(tx);
                }
            }));
        await([&] { return ready.load() == 8; });
        go.store(true);
        for (auto &worker : workers) {
            need(worker.wait_for(10s) == std::future_status::ready, "handoff stress hung");
            worker.get();
        }
        need(core->current()->version == 8 * 24 && !pending(*core) && !core->buffered_active.load(),
             "handoff lost a commit or queue state");
        need(core->snapshot_budget->used() == 0 && core->transaction_budget->used() == 0,
             "handoff retained transaction resources");
        core->sync();
    }
    auto core = open_core(path, segmented);
    for (unsigned thread = 0; thread < 8; ++thread)
        need(read(*core->current(), ("worker" + std::to_string(thread)).c_str()) ==
                 std::string(4096, 'a' + 23),
             "handoff reopen lost final model");
}

int main() {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        p::test_segment_geometry.store(65536);
        for (bool segmented : {false, true}) {
            conflict(64, segmented);
            conflict(1, segmented);
            withdrawals(false, segmented);
            withdrawals(true, segmented);
            detached_request(segmented, false);
            detached_request(segmented, true);
            metered_caller(segmented);
            repeated_handoffs(segmented);
        }
        std::cout << "PASS bounded BUFFERED submission, both layouts\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
