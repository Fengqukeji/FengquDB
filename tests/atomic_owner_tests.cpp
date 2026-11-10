#include "store_test_support.hpp"
#include "paged/operations.hpp"
#include <algorithm>

using namespace std::chrono_literals;
template <class F> static fqdb_status internal_status(F operation) {
    try {
        operation();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    }
}
template <class F> static bool until(F condition) {
    auto end = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
static size_t queued(const std::shared_ptr<p::Core> &core) {
    std::lock_guard<std::mutex> lock(core->group_mutex);
    return core->group_count;
}
static void empty_queue(const std::shared_ptr<p::Core> &core) {
    std::lock_guard<std::mutex> lock(core->group_mutex);
    check(!core->group_head && !core->group_tail && !core->group_count && !core->group_active,
          "private transaction left queued state");
}
static auto open(const std::string &path, bool segmented, bool sync, uint64_t snapshots) {
    auto options = p::defaults();
    options.flags |= segmented ? FQDB_STORE_SEGMENTED : 0;
    options.durability = sync ? FQDB_DURABILITY_SYNC : FQDB_DURABILITY_BUFFERED;
    options.delta_bytes = 65536;
    options.max_snapshots = snapshots;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    return core;
}
static uint64_t counter(const std::shared_ptr<p::Core> &core) {
    auto view = core->current();
    auto ref = view->find("counter");
    check(ref && ref->size == 8, "counter missing or invalid");
    auto bytes = view->generation->value(*ref, view->limit);
    return fengqudb::storage::get64(reinterpret_cast<const unsigned char *>(bytes->data()));
}
static void seed(const std::shared_ptr<p::Core> &core) {
    p::Transaction tx(core, 0);
    tx.put("a", "0", 0);
    tx.put("b", "0", 0);
    core->commit(tx);
    core->sync();
}
struct AtomicOutcome {
    fqdb_status code = FQDB_OK;
    fqdb_store_atomic_info info{};
    bool released = false;
};
static void quota_group(bool segmented, bool reproduce) {
    Fixture fixture;
    auto path = fixture.name("quota");
    auto core = open(path, segmented, true, 8);
    std::vector<std::shared_ptr<p::Capture>> ordinary;
    for (unsigned i = 0; i < 6; ++i)
        ordinary.push_back(std::make_shared<p::Capture>(core));
    std::vector<std::future<AtomicOutcome>> writers;
    Held hold(40);
    std::unique_lock<p::WriterGate> owner(core->writer);
    for (unsigned i = 0; i < 8; ++i) {
        writers.push_back(std::async(std::launch::async, [&] {
            p::AtomicUpdate update(core, "counter", p::AtomicKind::add, 0);
            update.delta = 1;
            AtomicOutcome result;
            result.code = internal_status([&] { result.info = update.commit(nullptr); });
            result.released = !update.transaction;
            return result;
        }));
        check(until([&] { return queued(core) == i + 1; }), "atomic request did not queue");
    }
    owner.unlock();
    reached();
    const auto captures_at_publish = core->snapshot_budget->used();
    const bool hidden = core->current()->version == 0;
    hold.release();
    unsigned successes = 0, limits = 0;
    bool released = true;
    for (auto &writer : writers) {
        auto result = writer.get();
        if (result.code == FQDB_OK) {
            ++successes;
            check(result.info.changed && result.info.value == successes &&
                      result.info.version == successes,
                  "per-operation version/value changed");
        } else {
            check(result.code == FQDB_LIMIT, "unexpected quota failure status");
            ++limits;
        }
        released &= result.released;
    }
    const auto value = counter(core);
    std::cout << "{\"case\":\"quota\",\"segmented\":" << (segmented ? "true" : "false")
              << ",\"successes\":" << successes << ",\"limits\":" << limits
              << ",\"captures_at_publish\":" << captures_at_publish << ",\"final_value\":" << value
              << "}\n"
              << std::flush;
    check(hidden && core->snapshot_budget->used() == 6 && !ordinary.front()->view->find("counter"),
          "ordinary snapshot ownership changed");
    check(core->current()->version == successes && core->current()->durable == successes &&
              value == successes,
          "group committed prefix differs from outcomes");
    empty_queue(core);
    ordinary.clear();
    core.reset();
    core = open(path, false, true, 8);
    check(counter(core) == value && core->current()->version == value,
          "quota group prefix lost after reopen");
    if (reproduce)
        check(successes == 2 && limits == 6 && captures_at_publish == 8,
              "original low-quota defect did not reproduce");
    else
        check(successes == 8 && limits == 0 && captures_at_publish == 6 && released,
              "atomic group exhausted quota or retained private state");
}
static void private_cleanup(bool segmented, bool fail_flush) {
    Fixture fixture;
    auto core = open(fixture.name("private"), segmented, true, 1);
    seed(core);
    std::vector<std::future<AtomicOutcome>> writers;
    Held hold(40);
    std::unique_lock<p::WriterGate> owner(core->writer);
    for (unsigned i = 0; i < 6; ++i) {
        writers.push_back(std::async(std::launch::async, [&, i] {
            p::AtomicUpdate update(core, i == 1 || i == 3 ? "a" : "counter",
                                   i == 2   ? p::AtomicKind::compare_exchange
                                   : i == 3 ? p::AtomicKind::put_changed
                                            : p::AtomicKind::add,
                                   0);
            update.delta = i == 0 || i == 5 ? 1 : 0;
            update.expected = 999;
            update.value = "0";
            AtomicOutcome result;
            result.code = internal_status([&] { result.info = update.commit(nullptr); });
            result.released = !update.transaction;
            return result;
        }));
        check(until([&] { return queued(core) == i + 1; }), "private operation did not queue");
    }
    owner.unlock();
    reached();
    check(core->snapshot_budget->used() == 0 && core->transaction_budget->used() == 0 &&
              core->current()->version == 1,
          "private state retained before publication");
    if (fail_flush)
        fqdb_store_test_io_fault(0, 1);
    hold.release();
    std::vector<AtomicOutcome> results;
    for (auto &writer : writers)
        results.push_back(writer.get());
    fqdb_store_test_io_fault(0, 0);
    for (const auto &result : results)
        check(result.released, "private state survived completion");
    check(results[1].code == FQDB_INVALID_ARGUMENT, "failed prepare lost its status");
    for (unsigned i : {0u, 2u, 3u, 4u, 5u})
        check(results[i].code == (fail_flush ? FQDB_COMMIT_UNKNOWN : FQDB_OK),
              "private completion status changed");
    if (fail_flush) {
        check(core->poisoned, "sync fault did not poison writer");
        auto path = core->directory;
        core.reset();
        core = open(path, false, true, 1);
    }
    p::AtomicUpdate noop(core, "counter", p::AtomicKind::add, 0);
    auto info = noop.commit(nullptr);
    check(!info.changed && info.value == 2 && info.version == 3 && !noop.transaction,
          "private no-op or sync recovery lost bytes");
    {
        auto capture = std::make_shared<p::Capture>(core);
        p::AtomicUpdate limited(core, "counter", p::AtomicKind::add, 0);
        limited.delta = 1;
        check(internal_status([&] { limited.commit(nullptr); }) == FQDB_LIMIT &&
                  !limited.transaction && core->snapshot_budget->used() == 1,
              "failed Capture changed ordinary snapshot ownership");
    }
    empty_queue(core);
}
static void write_failure(bool segmented, bool sync) {
    Fixture fixture;
    auto path = fixture.name("write-failure");
    auto core = open(path, segmented, sync, 1);
    seed(core);
    {
        p::AtomicUpdate invalid(core, "a", p::AtomicKind::add, 0);
        check(internal_status([&] { invalid.commit(nullptr); }) == FQDB_INVALID_ARGUMENT &&
                  !invalid.transaction && core->snapshot_budget->used() == 0,
              "failed prepare retained private transaction");
        p::AtomicUpdate failed(core, "counter", p::AtomicKind::add, 0);
        failed.delta = 1;
        fqdb_store_test_io_fault(1, 0);
        auto code = internal_status([&] { failed.commit(nullptr); });
        fqdb_store_test_io_fault(0, 0);
        check(code == FQDB_COMMIT_UNKNOWN && !failed.transaction && core->poisoned &&
                  core->snapshot_budget->used() == 0 && core->transaction_budget->used() == 0,
              "partial WAL failure retained private state or changed status");
        check(internal_status([&] { failed.commit(nullptr); }) == FQDB_RECOVERY_REQUIRED,
              "poisoned writer continued");
    }
    empty_queue(core);
    core.reset();
    core = open(path, false, sync, 1);
    check(core->current()->version == 1 && !core->current()->find("counter"),
          "partial WAL failure changed confirmed prefix");
    p::AtomicUpdate next(core, "counter", p::AtomicKind::add, 0);
    next.delta = 1;
    check(next.commit(nullptr).version == 2 && !next.transaction, "write after recovery failed");
}
static void ordinary_ownership(bool segmented, bool sync) {
    Fixture fixture;
    auto core = open(fixture.name("ordinary"), segmented, sync, 4);
    seed(core);
    p::Transaction stale(core, 0), winner(core, 0);
    (void)stale.get("a");
    stale.put("b", "pending", 0);
    winner.put("a", "winner", 0);
    core->commit(winner);
    check(internal_status([&] { core->commit(stale); }) == FQDB_CONFLICT && !stale.sealed &&
              stale.writes && core->snapshot_budget->used() == 2,
          "ordinary conflict destroyed caller-owned retry state");
    stale.cancelled.store(true);
    check(internal_status([&] { core->commit(stale); }) == FQDB_CANCELLED && stale.writes,
          "ordinary cancellation lost caller-owned state");
    empty_queue(core);
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        const bool reproduce = argc == 2 && std::string_view(argv[1]) == "--reproduce";
        p::test_segment_geometry.store(65536);
        for (bool segmented : {false, true}) {
            quota_group(segmented, reproduce);
            if (!reproduce) {
                private_cleanup(segmented, false);
                private_cleanup(segmented, true);
                for (bool sync : {false, true}) {
                    write_failure(segmented, sync);
                    ordinary_ownership(segmented, sync);
                }
            }
        }
        std::cout << (reproduce ? "REPRODUCED original private snapshot retention\n"
                                : "PASS private ownership without scheduling changes\n");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
