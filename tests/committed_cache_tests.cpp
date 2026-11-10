#include "store_test_support.hpp"
#include "paged/operations.hpp"
using namespace std::chrono_literals;
static auto open_core(Fixture &f, bool segmented, uint64_t cache = 1048576) {
    auto o = p::defaults();
    o.cache_bytes = cache;
    o.transaction_bytes = 1048576;
    o.all_transaction_bytes = 2097152;
    o.max_snapshots = 2;
    if (segmented)
        o.flags |= FQDB_STORE_SEGMENTED;
    auto c = std::make_shared<p::Core>(f.name("hot"), o);
    c->open();
    fqdb_store_write_options mode{sizeof(mode), 1, 1, 5000, 1048576};
    c->configure_write(mode);
    return c;
}
static std::string read(const std::shared_ptr<p::Core> &c, const char *key) {
    auto v = c->current();
    auto ref = v->find(key);
    if (!ref || !ref->visible(p::unix_ms()))
        return {};
    return *v->generation->value(*ref, v->limit);
}
static void mutate(const std::shared_ptr<p::Core> &c, p::AtomicKind kind, const char *key,
                   std::string_view bytes = {}, uint64_t offset = 0, uint64_t remove = 0,
                   bool force = false) {
    p::AtomicUpdate u(c, key, kind, 0);
    u.value = bytes;
    u.offset = offset;
    u.remove = remove;
    u.commit(nullptr, force);
    check(!u.single && !u.transaction, "private operation retained prepared state");
}
static void cached(const std::shared_ptr<p::Core> &c, const char *key, std::string_view expected) {
    auto view = c->current();
    auto ref = view->find(key);
    check(bool(ref), "missing ref");
    uint64_t tag = 0;
    auto value = view->generation->values.get(ref->offset, &tag);
    check(value && *value == expected && p::value_crc(value) == ref->crc &&
              tag == ((uint64_t(ref->depth) << 32) | ref->recipe_size),
          "committed full value not cached");
}
static void aliases_and_budgets(bool segmented) {
    Fixture f;
    auto c = open_core(f, segmented);
    mutate(c, p::AtomicKind::put, "seed", "old");
    check(read(c, "seed") == "old", "seed read");
    cached(c, "seed", "old");
    auto snapshot = std::make_shared<p::Capture>(c);
    p::Value alias;
    p::WriteRoot private_cursor;
    {
        p::Transaction tx(c, 0);
        tx.put("k", "ABCDE", 0);
        alias = tx.get("k");
        private_cursor = tx.writes;
        tx.savepoint();
        tx.patch("k", 1, 3, "Q");
        tx.put("other", "value", 0);
        tx.expire("seed", p::unix_ms() + 60000);
        tx.patch("seed", 0, 1, "O"); // An expiry-only private node has no materialized buffer.
        c->commit(tx);
        cached(c, "seed", "Old");
        cached(c, "k", "AQE");
        check(read(c, "other") == "value", "put read");
        cached(c, "other", "value");
        check(*alias == "ABCDE" && *private_cursor->value.value == "ABCDE",
              "saved private bytes mutated");
    }
    check(c->transaction_budget->used() > 0, "private aliases lost budget charge");
    alias.reset();
    private_cursor.reset();
    check(c->transaction_budget->used() == 0, "cache retained transaction budget");
    // Saturate the public snapshot slots. Direct single-key mutations need no public capture.
    auto second = std::make_shared<p::Capture>(c);
    mutate(c, p::AtomicKind::patch, "seed", "new", 0, 3);
    cached(c, "seed", "new");
    check(*snapshot->view->generation->value(*snapshot->view->find("seed"),
                                             snapshot->view->limit) == "old",
          "single patch changed old snapshot");
    check(c->snapshot_budget->used() == 2 && c->transaction_budget->used() == 0,
          "single mutation charged a snapshot");
    for (int n = 0; n < 12; ++n) {
        mutate(c, p::AtomicKind::patch, "seed", n % 2 ? "XYZ" : "new", 0, 3);
        cached(c, "seed", n % 2 ? "XYZ" : "new");
    }
    mutate(c, p::AtomicKind::put, "empty", {});
    check(read(c, "empty").empty(), "empty read");
    cached(c, "empty", {});
    mutate(c, p::AtomicKind::erase_any, "seed");
    check(read(c, "seed").empty(), "single delete");
    mutate(c, p::AtomicKind::put, "save", "first");
    read(c, "save");
    mutate(c, p::AtomicKind::put, "save", "second");
    const auto unread = c->current()->find("save");
    check(!c->current()->generation->values.recently_read(unread->offset),
          "promotion marked as read");
    auto entries = c->current()->generation->values.info().entries;
    mutate(c, p::AtomicKind::put, "save", "third");
    check(c->current()->generation->values.info().entries == entries,
          "write-only save filled cache");
    check(read(c, "save") == "third", "lazy cache fallback");
    c->sync();
}
static void no_cache(bool segmented, uint64_t cache) {
    Fixture f;
    auto c = open_core(f, segmented, cache);
    std::string value(4096, 'x');
    mutate(c, p::AtomicKind::put, "k", value);
    check(c->current()->generation->values.info().bytes == 0, "oversized value admitted");
    for (int n = 0; n < 12; ++n) {
        mutate(c, p::AtomicKind::patch, "k", n % 2 ? "q" : "r", 17, 1);
        value[17] = n % 2 ? 'q' : 'r';
        check(read(c, "k") == value, "uncached recipe fallback");
    }
    check(c->transaction_budget->used() == 0, "uncached transaction retained");
    c->sync();
}
static void retired_versions(bool segmented) {
    Fixture f;
    auto c = open_core(f, segmented);
    std::string first(4096, 'a');
    for (int i = 0; i < 8; ++i) {
        mutate(c, p::AtomicKind::put, std::to_string(i).c_str(), first);
        read(c, std::to_string(i).c_str());
    }
    auto frozen = c->current();
    auto old = frozen->find("0").value();
    auto borrowed = frozen->generation->value(old, frozen->limit);
    for (unsigned n = 0; n < 160; ++n) {
        auto name = std::to_string(n % 8);
        mutate(c, p::AtomicKind::patch, name.c_str(), "b", n % 4096, 1);
    }
    check(*borrowed == first, "retirement changed a borrowed value");
    check(bool(frozen->generation->values.get(old.offset)), "borrowed cache version retired");
    check(c->current()->generation->values.info().entries <= 16, "obsolete versions filled cache");
    // An unread old snapshot can reconstruct an evicted version before flush.
    auto second = frozen->find("1").value();
    check(*frozen->generation->value(second, frozen->limit) == first,
          "old snapshot after retirement");
    c->sync();
    check(*frozen->generation->value(second, frozen->limit) == first, "old snapshot after flush");
    borrowed.reset();
    frozen->generation->values.retire(old.offset);
    check(!frozen->generation->values.get(old.offset), "unborrowed version retained");
    frozen.reset();
    check(c->transaction_budget->used() == 0, "retirement retained transaction budget");
}
template <class F> static fqdb_status internal_status(F &&call) {
    try {
        call();
        return FQDB_OK;
    } catch (const p::Error &error) {
        return error.status;
    }
}
static void dependency_metadata(bool segmented) {
    Fixture f;
    auto c = open_core(f, segmented);
    mutate(c, p::AtomicKind::put, "k", "old");
    {
        p::Transaction tx(c, 0);
        check(*tx.get("k") == "old", "initial snapshot read");
        mutate(c, p::AtomicKind::put, "k", "new");
        check(*tx.get("k") == "old", "metadata cache crossed snapshots");
        auto save = tx.savepoint();
        tx.patch("k", 0, 1, "O");
        tx.rollback_to(save);
        check(*tx.get("k") == "old", "metadata cache after savepoint");
        tx.patch("k", 0, 1, "O");
        check(internal_status([&] { c->commit(tx); }) == FQDB_CONFLICT,
              "cached read skipped conflict");
    }
    {
        p::Transaction tx(c, 0);
        check(!tx.try_get("missing"), "initial missing");
        mutate(c, p::AtomicKind::put, "missing", "other");
        check(!tx.try_get("missing"), "negative snapshot read changed");
        tx.put("missing", "mine", 0);
        check(internal_status([&] { c->commit(tx); }) == FQDB_CONFLICT, "negative dependency lost");
    }
    mutate(c, p::AtomicKind::erase_any, "k");
    {
        p::Transaction tx(c, 0);
        check(!tx.try_get("k") && tx.depend("k").version, "tombstone metadata lost");
        tx.put("k", "reborn", 0);
        c->commit(tx);
    }
    check(read(c, "k") == "reborn" && c->transaction_budget->used() == 0,
          "cached tombstone recreation");
    c->sync();
}
static void conflict_is_not_failure(bool segmented) {
    Fixture f;
    auto o = p::defaults();
    if (segmented)
        o.flags |= FQDB_STORE_SEGMENTED;
    auto c = std::make_shared<p::Core>(f.name("conflicts"), o);
    c->open();
    mutate(c, p::AtomicKind::put, "a", "100");
    mutate(c, p::AtomicKind::put, "b", "100");
    {
        p::Transaction first(c, 0), second(c, 0);
        check(*first.get("a") == "100" && *second.get("a") == "100" && *first.get("b") == "100" &&
                  *second.get("b") == "100",
              "common transfer snapshot");
        first.put("a", "90", 0);
        first.put("b", "110", 0);
        second.put("a", "80", 0);
        second.put("b", "120", 0);
        c->commit(first);
        check(internal_status([&] { c->commit(second); }) == FQDB_CONFLICT,
              "stale transfer overwrote the first transfer");
        check(read(c, "a") == "90" && read(c, "b") == "110", "conflict changed committed balances");
    }
    {
        p::Transaction retried(c, 0);
        retried.put("a", std::to_string(std::stoi(*retried.get("a")) - 20), 0);
        retried.put("b", std::to_string(std::stoi(*retried.get("b")) + 20), 0);
        c->commit(retried);
        check(read(c, "a") == "70" && read(c, "b") == "130", "retry lost a transfer");
    }
    {
        p::Transaction first(c, 0), second(c, 0);
        first.get("a");
        second.get("b");
        first.put("a", "71", 0);
        second.put("b", "131", 0);
        c->commit(first);
        c->commit(second); // A global version change alone is not a conflict.
    }
    for (unsigned n = 0; n < 8; ++n)
        mutate(c, p::AtomicKind::put, std::to_string(n).c_str(), "0");
    std::atomic<unsigned> conflicts{0};
    std::vector<std::future<void>> workers;
    for (unsigned n = 0; n < 8; ++n)
        workers.push_back(std::async(std::launch::async, [&, n] {
            auto key = std::to_string(n);
            for (unsigned i = 0; i < 100; ++i) {
                p::Transaction tx(c, 0);
                tx.put(key, std::to_string(std::stoi(*tx.get(key)) + 1), 0);
                auto code = internal_status([&] { c->commit(tx); });
                if (code == FQDB_CONFLICT)
                    ++conflicts;
                else
                    check(code == FQDB_OK, "independent transaction failed");
            }
        }));
    for (auto &worker : workers)
        worker.get();
    check(conflicts.load() == 0, "independent keys had false conflicts");
    for (unsigned n = 0; n < 8; ++n)
        check(read(c, std::to_string(n).c_str()) == "100", "independent result");
    c->sync();
}
static void cache_admission() {
    p::Cache<p::Data> cache(65536);
    auto bytes = std::make_shared<const p::Data>("old");
    check(cache.put(1, bytes, 256), "cache admission");
    cache.retire(1);
    check(bool(cache.get(1)), "borrowed entry retired");
    check(!cache.put(2, bytes, 100000), "oversized cache admission");
    bytes.reset();
    cache.retire(1);
    check(cache.info().bytes == 0 && cache.info().entries == 0, "retired cache accounting");
}
static void signals_and_threads() {
    Fixture f;
    auto path = f.name("signals");
    fqdb_store *db = nullptr;
    check(fqdb_store_open(path.c_str(), nullptr, &db) == 0, "open");
    std::vector<std::future<void>> calls;
    for (int t = 0; t < 8; ++t)
        calls.push_back(std::async(std::launch::async, [&, t] {
            auto name = std::to_string(t);
            fqdb_slice key{name.data(), name.size()};
            for (int i = 0; i < 80; ++i) {
                std::string value = std::to_string(i);
                fqdb_store_atomic_info out{};
                check(fqdb_store_put(db, key, {value.data(), value.size()}, 0, &out, i == 79) == 0,
                      "sync signal reuse");
            }
        }));
    for (auto &call : calls)
        call.get();
    // Completion may race the producer's TLS teardown. Repeated short-lived
    // callers exercise signal ownership after publishing the result.
    for (unsigned n = 0; n < 256; ++n) {
        std::thread caller([&] {
            fqdb_store_atomic_info out{};
            check(fqdb_store_put(db, {"churn", 5}, {"value", 5}, 0, &out) == 0,
                  "short-lived signal owner");
        });
        caller.join();
    }
    fqdb_store_commit *receipt = nullptr;
    {
        Held hold(43);
        check(fqdb_store_put_submit(db, {"multi", 5}, {"value", 5}, 0, &receipt) == 0, "submit");
        reached();
        auto wait = [&] {
            fqdb_status result;
            uint64_t version;
            check(fqdb_store_commit_wait(receipt, 5000, &result, &version) == 0 && result == 0 &&
                      version,
                  "multiple receipt waiters");
        };
        auto a = std::async(std::launch::async, wait), b = std::async(std::launch::async, wait);
        hold.release();
        a.get();
        b.get();
    }
    fqdb_store_commit_close(receipt);
    check(fqdb_store_sync(db) == 0, "sync");
    fqdb_store_close(db);
}
int main() {
    try {
#ifdef __SANITIZE_THREAD__
        std::cerr << "FQDB_TSAN_STARTED\n";
#endif
        for (bool segmented : {false, true}) {
            retired_versions(segmented);
            dependency_metadata(segmented);
            aliases_and_budgets(segmented);
            no_cache(segmented, 0);
            no_cache(segmented, 512);
        }
        conflict_is_not_failure(false);
        conflict_is_not_failure(true);
        cache_admission();
        for (int fault : {0, 1, 2}) {
            p::commit_signal_fault.store(fault);
            signals_and_threads();
        }
        p::commit_signal_fault.store(0);
        std::cout << "PASS committed cache, private aliases/budgets, single mutations and reusable "
                     "completion\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
