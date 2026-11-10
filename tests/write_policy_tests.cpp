#include "store_test_support.hpp"
using namespace std::chrono_literals;
static void adaptive_wait_policy() {
    p::AdaptiveCommitWait policy;
    using Clock = p::AdaptiveCommitWait::Clock;
    auto now = Clock::time_point{} + 1s;
    check(policy.begin_idle(now) == 0, "cold worker spun without completed work");
    policy.arrived(now + 10ms);
    unsigned expected = p::commit_spins();
    for (unsigned i = 0; i < 6; ++i) {
        now += 20ms;
        policy.completed(true);
        check(policy.begin_idle(now) == expected, "adaptive wait did not learn a short gap");
        check(policy.begin_idle(now + 1us) == 0, "timer wake rearmed idle spinning");
        policy.arrived(now + 10us);
        expected = std::min(4096u, expected * 2);
    }
    now += 20ms;
    policy.completed(true);
    check(policy.begin_idle(now) == 4096, "adaptive wait ceiling");
    policy.arrived(now + 500us);
    now += 20ms;
    policy.completed(true);
    check(policy.begin_idle(now) == 2048, "medium idle gap did not reduce spinning");
    policy.arrived(now + 50ms);
    now += 60ms;
    policy.completed(true);
    check(policy.begin_idle(now) == p::commit_spins(), "long idle did not reset spinning");
    policy.arrived(now + 10us);
    policy.completed(false);
    check(policy.begin_idle(now + 20us) == 0, "reliable completion enabled spinning");
    std::atomic<bool> ready{true};
    check(p::AdaptiveCommitWait::spin(ready, 4096, Clock::now() + 1s) == 0,
          "available work was delayed by spinning");
    ready.store(false);
    check(p::AdaptiveCommitWait::spin(ready, 4096, Clock::now() - 1ns) == 0,
          "adaptive wait crossed an already due flush deadline");
    p::AdaptiveCommitWait single(false);
    for (unsigned i = 0; i < 8; ++i) {
        now += 1ms;
        single.completed(true);
        check(single.begin_idle(now) == p::commit_spins(), "single CPU grew its spin budget");
        single.arrived(now + 10us);
    }
}
static fqdb_slice b(std::string_view v) {
    return {v.data(), v.size()};
}
static void ok(fqdb_status s) {
    if (s)
        throw fengqudb::Error(s);
}
static void mode(fqdb_store *s, uint32_t m, uint32_t period = 5000, uint64_t bytes = 1048576) {
    fqdb_store_write_options p{sizeof(p), 1, m, period, bytes};
    ok(fqdb_store_write_configure(s, &p));
}
static fqdb_store_write_state info(fqdb_store *s) {
    fqdb_store_write_state i{};
    ok(fqdb_store_write_state_get(s, &i));
    return i;
}
static std::string get(fqdb_store *s, std::string_view key) {
    fqdb_buffer o{};
    auto c = fqdb_store_get(s, b(key), &o);
    if (c == FQDB_NOT_FOUND)
        return {};
    ok(c);
    std::string v(static_cast<const char *>(o.data), size_t(o.size));
    fqdb_buffer_free(&o);
    return v;
}
static uint64_t put(fqdb_store *s, std::string_view key, std::string_view value,
                    bool force = false) {
    fqdb_store_atomic_info i{};
    ok(fqdb_store_put(s, b(key), b(value), 0, &i, force));
    return i.version;
}
static fqdb_store_commit *submit(fqdb_store *s, std::string_view key, bool force = false) {
    fqdb_store_commit *r = nullptr;
    ok(fqdb_store_put_submit(s, b(key), b(key), 0, &r, force));
    return r;
}
static fqdb_status done(fqdb_store_commit *r) {
    fqdb_status c;
    uint64_t v;
    ok(fqdb_store_commit_wait(r, 5000, &c, &v));
    fqdb_store_commit_close(r);
    return c;
}
static void visible(bool segmented, bool direct) {
    Fixture f;
    auto path = f.name("visible");
    auto o = p::defaults();
    o.cache_bytes = 0;
    if (direct)
        o.all_transaction_bytes = UINT64_MAX; // The buffer ceiling must not wrap.
    if (segmented)
        o.flags |= FQDB_STORE_SEGMENTED;
    fqdb_store *s = nullptr;
    ok(fqdb_store_open(path.c_str(), &o, &s));
    fqdb_store_write_options c{};
    ok(fqdb_store_write_options_get(s, &c));
    check(c.mode == 1 && c.period_ms == 50 && c.threshold_bytes == 1048576, "defaults");
    mode(s, 1);
    if (direct) {
        fqdb_store_maintenance_info i{};
        ok(fqdb_store_compact(s, nullptr, &i));
    }
    auto v = put(s, "k", "abcde");
    auto i = info(s);
    check(i.version == v && i.durable_version < v && i.pending_bytes,
          "memory commit waited for disk");
    check(get(s, "k") == "abcde", "uncached pending read");
    fqdb_store_snapshot *snapshot = nullptr;
    ok(fqdb_store_snapshot_create(s, &snapshot));
    fqdb_store_atomic_info a{};
    ok(fqdb_store_patch(s, b("k"), 1, 3, b("Q"), &a));
    check(get(s, "k") == "aQe", "subtraction patch");
    fqdb_buffer old{};
    ok(fqdb_store_snapshot_get(snapshot, b("k"), &old));
    check(std::string(static_cast<const char *>(old.data), size_t(old.size)) == "abcde",
          "snapshot changed");
    fqdb_buffer_free(&old);
    fqdb_store_snapshot_close(snapshot);
    ok(fqdb_store_delete(s, b("k"), &a));
    check(get(s, "k").empty(), "delete");
    put(s, "same", "v");
    auto before = info(s);
    ok(fqdb_store_put_if_changed(s, b("same"), b("v"), 0, 0, &a, 1));
    check(!a.changed && info(s).version == before.version &&
              info(s).durable_version == before.version,
          "forced no-op did not persist prefix");
    fqdb_store_txn *tx = nullptr;
    ok(fqdb_store_txn_begin(s, 0, &tx));
    ok(fqdb_store_txn_put(tx, b("a"), b("A"), 0));
    ok(fqdb_store_txn_put(tx, b("b"), b("B"), 0));
    ok(fqdb_store_txn_commit(tx, &v));
    fqdb_store_txn_close(tx);
    check(get(s, "a") == "A" && get(s, "b") == "B" && info(s).durable_version < v,
          "performance synchronous transaction");
    ok(fqdb_store_txn_begin(s, 0, &tx));
    ok(fqdb_store_txn_put(tx, b("forced-tx"), b("yes"), 0));
    ok(fqdb_store_txn_commit(tx, &v, 1));
    fqdb_store_txn_close(tx);
    check(info(s).durable_version >= v, "forced synchronous transaction");
    auto paid = put(s, "paid", "yes", true);
    check(info(s).durable_version >= paid, "forced commit");
    check(done(submit(s, "async")) == FQDB_OK && get(s, "async") == "async",
          "async memory completion");
    put(s, "maintenance", "pending");
    fqdb_store_maintenance_info mi{};
    fqdb_store_maintenance_io io{};
    fqdb_store_maintenance_options mo{sizeof(mo), 1, 8, 0, 0, 1048576, 0, 1048576, &io};
    ok(fqdb_store_verify(s, &mo, &mi));
    ok(fqdb_store_compact(s, nullptr, &mi));
    check(get(s, "maintenance") == "pending", "maintenance lost memory commit");
    mode(s, 0, 37, 32768);
    check(info(s).version == info(s).durable_version, "switch drain");
    v = put(s, "strict", "yes");
    check(info(s).durable_version >= v, "reliable default");
    fqdb_store_close(s);
    ok(fqdb_store_open(path.c_str(), &o, &s));
    ok(fqdb_store_write_options_get(s, &c));
    check(c.mode == 0 && c.period_ms == 37 && get(s, "paid") == "yes" && get(s, "b") == "B",
          "saved mode/reopen");
    ok(fqdb_store_write_configure(s, nullptr));
    ok(fqdb_store_write_options_get(s, &c));
    check(c.mode == 1 && c.period_ms == 50 && c.threshold_bytes == 1048576, "reset");
    fqdb_store_close(s);
    ok(fqdb_store_open(path.c_str(), &o, &s));
    ok(fqdb_store_write_options_get(s, &c));
    check(c.mode == 1 && c.period_ms == 50 && c.threshold_bytes == 1048576, "saved reset/reopen");
    // A saved 100 ms choice must survive the new product default.
    mode(s, 1, 100);
    fqdb_store_close(s);
    ok(fqdb_store_open(path.c_str(), &o, &s));
    ok(fqdb_store_write_options_get(s, &c));
    check(c.mode == 1 && c.period_ms == 100 && c.threshold_bytes == 1048576,
          "saved custom period overwritten");
    fqdb_store_close(s);
}
static void fault_and_controls() {
    Fixture f;
    auto path = f.name("fault");
    fqdb_store *s = nullptr;
    ok(fqdb_store_open(path.c_str(), nullptr, &s));
    mode(s, 1);
    put(s, "prefix", "memory");
    {
        Held hold(40);
        auto call = std::async(std::launch::async, [&] { return put(s, "critical", "yes", true); });
        reached();
        check(call.wait_for(1ms) != std::future_status::ready && get(s, "critical").empty() &&
                  get(s, "prefix") == "memory",
              "forced publication before flush");
        hold.release();
        auto v = call.get();
        check(info(s).durable_version >= v, "forced barrier");
    }
    {
        Held hold(43);
        auto first = submit(s, "held");
        reached();
        auto cancelled = submit(s, "cancelled");
        fqdb_store_commit_cancel(cancelled);
        fqdb_store_txn *tx = nullptr;
        ok(fqdb_store_txn_begin(s, 30, &tx));
        ok(fqdb_store_txn_put(tx, b("timed"), b("no"), 0));
        fqdb_store_commit *timed = nullptr;
        ok(fqdb_store_txn_submit(tx, &timed));
        fqdb_store_txn_close(tx);
        std::this_thread::sleep_for(50ms);
        hold.release();
        check(done(first) == FQDB_OK && done(cancelled) == FQDB_CANCELLED &&
                  done(timed) == FQDB_TIMEOUT,
              "queued controls");
    }
    std::vector<std::future<void>> calls;
    for (int t = 0; t < 4; ++t)
        calls.push_back(std::async(std::launch::async, [&] {
            for (int n = 0; n < 50; ++n) {
                fqdb_store_atomic_info r{};
                ok(fqdb_store_add_i64(s, b("counter"), 1, INT64_MIN, INT64_MAX, 0, &r));
            }
        }));
    for (auto &c : calls)
        c.get();
    auto counter = get(s, "counter");
    check(counter.size() == 8 && fengqudb::storage::get64(reinterpret_cast<const unsigned char *>(
                                     counter.data())) == 200,
          "lost increments");
    fqdb_store_txn *a = nullptr, *bb = nullptr;
    ok(fqdb_store_txn_begin(s, 0, &a));
    ok(fqdb_store_txn_begin(s, 0, &bb));
    ok(fqdb_store_txn_put(a, b("conflict"), b("a"), 0));
    ok(fqdb_store_txn_put(bb, b("conflict"), b("b"), 0));
    uint64_t v;
    ok(fqdb_store_txn_commit(a, &v));
    check(fqdb_store_txn_commit(bb, &v) == FQDB_CONFLICT, "snapshot conflict removed");
    fqdb_store_txn_close(a);
    fqdb_store_txn_close(bb);
    mode(s, 0);
    {
        Held hold(40);
        auto r = submit(s, "reliable");
        reached();
        fqdb_status result;
        check(fqdb_store_commit_poll(r, &result, &v) == FQDB_NOT_MODIFIED,
              "reliable early receipt");
        hold.release();
        check(done(r) == FQDB_OK, "reliable result");
    }
    mode(s, 1, 1, 1);
    {
        Held hold(40);
        put(s, "volatile", "new");
        reached();
        check(get(s, "volatile") == "new", "memory hidden during flush");
        fqdb_store_test_io_fault(0, 1);
        hold.release();
        auto end = std::chrono::steady_clock::now() + 3s;
        while (info(s).last_status == 0) {
            check(std::chrono::steady_clock::now() < end, "fault not reported");
            std::this_thread::yield();
        }
        check(info(s).last_status == FQDB_COMMIT_UNKNOWN &&
                  info(s).durable_version < info(s).version,
              "failed flush advanced durability");
        fqdb_store_test_io_fault(0, 0);
        fqdb_store_atomic_info r{};
        check(fqdb_store_put(s, b("later"), b("no"), 0, &r) == FQDB_RECOVERY_REQUIRED,
              "write after fault");
    }
    fqdb_store_close(s);
}
static void idle_without_flush(bool segmented, bool direct) {
    Fixture f;
    auto o = p::defaults();
    if (segmented)
        o.flags |= FQDB_STORE_SEGMENTED;
    auto core = std::make_shared<p::Core>(f.name("idle"), o);
    core->open();
    if (direct)
        core->compact({sizeof(fqdb_store_maintenance_options), 1, 8, 0, 0, 1048576, 0, 0, nullptr});
    auto file = core->current()->generation->file;
    auto writes = file->writes.load(), syncs = file->syncs.load();
    std::this_thread::sleep_for(200ms);
    check(!core->periodic_thread.joinable() && !core->periodic_iterations.load() &&
              file->writes.load() == writes && file->syncs.load() == syncs,
          "empty database started a timer or disk work");
    for (unsigned round = 0; round < 2; ++round) {
        const auto prior = core->periodic_idle_waits.load();
        p::Transaction tx(core, 0);
        tx.put("k", round ? "again" : "first", 0);
        const auto version = core->commit(tx);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(core->periodic_mutex);
                if (core->periodic_idle_waits.load() > prior &&
                    core->current()->durable >= version && !core->dirty_bytes.load())
                    break;
            }
            check(std::chrono::steady_clock::now() < deadline, "default timer did not become idle");
            std::this_thread::sleep_for(1ms);
        }
        uint64_t iterations, waits;
        {
            std::lock_guard<std::mutex> lock(core->periodic_mutex);
            iterations = core->periodic_iterations.load();
            waits = core->periodic_idle_waits.load();
            writes = file->writes.load();
            syncs = file->syncs.load();
        }
        std::this_thread::sleep_for(250ms); // Five configured periods after the last flush.
        {
            std::lock_guard<std::mutex> lock(core->periodic_mutex);
            check(!core->periodic_head && !core->dirty_bytes.load() &&
                      core->periodic_iterations.load() == iterations &&
                      core->periodic_idle_waits.load() == waits && file->writes.load() == writes &&
                      file->syncs.load() == syncs,
                  "idle writer woke periodically or flushed empty data");
        }
    }
}
static void triggers() {
    for (bool timer : {false, true}) {
        Fixture f;
        auto path = f.name("trigger");
        fqdb_store *s = nullptr;
        ok(fqdb_store_open(path.c_str(), nullptr, &s));
        mode(s, 1, timer ? 20 : 5000, timer ? 1048576 : 128);
        auto v = put(s, "key", std::string(256, 'x'));
        auto end = std::chrono::steady_clock::now() + 2s;
        while (info(s).durable_version < v) {
            check(std::chrono::steady_clock::now() < end,
                  timer ? "timer not flushed" : "byte trigger not flushed");
            std::this_thread::yield();
        }
        fqdb_store_close(s);
    }
}
static void config_fault() {
    Fixture f;
    auto path = f.name("config");
    fqdb_store *s = nullptr;
    ok(fqdb_store_open(path.c_str(), nullptr, &s));
    mode(s, 1);
    fqdb_store_write_options c{sizeof(c), 1, 0, 19, 32768};
    fqdb_store_test_io_fault(1, 0);
    check(fqdb_store_write_configure(s, &c) == FQDB_IO_ERROR, "save failure hidden");
    fqdb_store_test_io_fault(0, 0);
    ok(fqdb_store_write_options_get(s, &c));
    check(c.mode == 1, "save failure changed policy");
    fqdb_store_close(s);
    p::NativeFile file;
    file.open(p::join(path, "WRITE"), false, true);
    unsigned char bad = 7;
    file.write(12, &bad, 1);
    file.sync();
    file.retire();
    s = nullptr;
    check(fqdb_store_open(path.c_str(), nullptr, &s) == FQDB_CORRUPT && !s,
          "corrupt policy accepted");
}
int main(int argc, char **argv) {
    try {
        adaptive_wait_policy();
#ifdef __SANITIZE_THREAD__
        std::cerr << "FQDB_TSAN_STARTED\n";
#endif
        if (argc == 5 && std::string_view(argv[1]) == "--crash") {
            fqdb_store *s = nullptr;
            ok(fqdb_store_open(argv[2], nullptr, &s));
            int point = std::stoi(argv[4]);
            if (point == 9) {
                put(s, "before", "yes");
                put(s, "paid", "yes", true);
                put(s, "after", "no");
                std::_Exit(86);
            }
            fqdb_store_test_crash(point);
            put(s, "crash", "complete", true);
            return 2;
        }
        visible(false, false);
        visible(true, false);
        visible(false, true);
        fault_and_controls();
        config_fault();
        triggers();
        idle_without_flush(false, false);
        idle_without_flush(true, false);
        idle_without_flush(false, true);
#ifndef __SANITIZE_THREAD__
        for (int point : {1, 2, 3, 4, 9})
            for (bool segmented : {false, true}) {
                Fixture f;
                auto path = f.name("crash");
                auto o = p::defaults();
                if (segmented)
                    o.flags |= FQDB_STORE_SEGMENTED;
                fqdb_store *s = nullptr;
                ok(fqdb_store_open(path.c_str(), &o, &s));
                mode(s, 1);
                put(s, "prefix", "keep", true);
                fqdb_store_close(s);
                auto exe = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
                check(child(exe, "--crash", path, "unused", point) == 86, "crash child");
                ok(fqdb_store_open(path.c_str(), &o, &s));
                check(get(s, "prefix") == "keep", "durable prefix lost");
                if (point == 9)
                    check(get(s, "before") == "yes" && get(s, "paid") == "yes" &&
                              get(s, "after").empty(),
                          "forced prefix or RAM suffix");
                else {
                    auto value = get(s, "crash");
                    check(point == 1   ? value.empty()
                          : point == 2 ? value.empty() || value == "complete"
                                       : value == "complete",
                          "recovery");
                }
                fqdb_store_close(s);
            }
#endif
        std::cout << "PASS performance/reliable, memory visibility, forced prefix, controls and "
                     "recovery\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
