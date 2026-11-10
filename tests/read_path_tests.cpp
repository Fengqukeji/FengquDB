#include "store_test_support.hpp"
#include "store_clock.hpp"
#include <atomic>
using namespace std::chrono_literals;
extern "C" void fqdb_store_test_read_stamp(fqdb_store *, uint64_t);
static std::string message(uint64_t value, size_t size = 4096) {
    fengqudb::wire::Writer w;
    w.fixed64_field(1, value);
    w.bytes_field(2, std::string(size, 'p'));
    return w.take();
}
static std::string selected(uint64_t value) {
    fengqudb::wire::Writer w;
    w.fixed64_field(1, value);
    return w.take();
}
static uint64_t number(std::string_view value) {
    check(value.size() >= 9 && value[0] == 9, "read invalid projection");
    return fengqudb::storage::get64(reinterpret_cast<const unsigned char *>(value.data() + 1));
}
static fqdb_slice b(std::string_view value) {
    return {value.data(), value.size()};
}
static void ok(fqdb_status value) {
    if (value)
        throw fengqudb::Error(value);
}
static std::string raw_get(fqdb_store *db) {
    fqdb_buffer bytes{};
    ok(fqdb_store_get(db, b("key"), &bytes));
    std::string value(static_cast<const char *>(bytes.data), size_t(bytes.size));
    fqdb_buffer_free(&bytes);
    return value;
}
static void raw_put(fqdb_store *db, std::string_view value) {
    fqdb_store_atomic_info out{};
    ok(fqdb_store_put(db, b("key"), b(value), 0, &out));
}
static std::string prepared_get(fqdb_store_key *key) {
    fqdb_store_value *raw = nullptr;
    ok(fqdb_store_key_get_view(key, &raw));
    std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> value(
        raw, fqdb_store_value_close);
    auto bytes = fqdb_store_value_data(raw);
    return std::string(static_cast<const char *>(bytes.data), size_t(bytes.size));
}
static void publications() {
    Fixture f;
    fqdb_store *raw = nullptr;
    ok(fqdb_store_open(f.name("stamp").c_str(), nullptr, &raw));
    std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> db(raw, fqdb_store_close);
    fqdb_store_write_options write{sizeof(write), 1, FQDB_WRITE_PERFORMANCE, 60000, 64ull << 20};
    ok(fqdb_store_write_configure(raw, &write));
    raw_put(raw, message(1));
    check(number(raw_get(raw)) == 1, "memo warmup");
    fqdb_store_key *prepared_raw = nullptr;
    ok(fqdb_store_key_prepare(raw, b("key"), &prepared_raw));
    std::unique_ptr<fqdb_store_key, decltype(&fqdb_store_key_close)> prepared(prepared_raw,
                                                                              fqdb_store_key_close);
    check(number(prepared_get(prepared_raw)) == 1, "prepared memo warmup");
    for (int point : {87, 88}) {
        Held hold(point);
        auto future =
            std::async(std::launch::async, [&] { raw_put(raw, message(unsigned(point))); });
        reached();
        // A second reader may see the new view before the publishing call returns.
        check(number(raw_get(raw)) == unsigned(point == 87 ? 1 : point),
              "odd stamp reused stale memo");
        check(number(prepared_get(prepared_raw)) == unsigned(point == 87 ? 1 : point),
              "odd stamp reused stale prepared value");
        hold.release();
        future.get();
        check(number(raw_get(raw)) == unsigned(point), "published memo stale");
        raw_put(raw, message(1));
    }
    fqdb_store_test_read_stamp(raw, UINT64_MAX - 3);
    raw_put(raw, message(2));
    check(number(raw_get(raw)) == 2, "near-saturated epoch");
    raw_put(raw, message(3));
    check(number(raw_get(raw)) == 3, "saturated epoch");
    raw_put(raw, message(4));
    check(number(raw_get(raw)) == 4, "saturated epoch wrapped");
    check(number(prepared_get(prepared_raw)) == 4, "prepared saturated epoch reused stale value");
}
static void prepared_missing_output() {
    Fixture f;
    auto options = p::defaults();
    options.cache_bytes = 1; // Metadata may be warm, but no full value is retained.
    fqdb_store *raw = nullptr;
    ok(fqdb_store_open(f.name("prepared-missing").c_str(), &options, &raw));
    std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> db(raw, fqdb_store_close);
    fqdb_store_write_options write{sizeof(write), 1, FQDB_WRITE_PERFORMANCE, 60000, 64ull << 20};
    ok(fqdb_store_write_configure(raw, &write));
    raw_put(raw, message(1));
    fqdb_store_key *key_raw = nullptr;
    ok(fqdb_store_key_prepare(raw, b("key"), &key_raw));
    std::unique_ptr<fqdb_store_key, decltype(&fqdb_store_key_close)> key(key_raw,
                                                                         fqdb_store_key_close);
    check(prepared_get(key_raw) == message(1), "expired prepared weak-value fixture");
    Held hold(89);
    auto read = std::async(std::launch::async, [&] {
        std::string target(8192, '\0');
        uint64_t required = UINT64_MAX;
        auto code = fqdb_store_key_get_into(key_raw, target.data(), target.size(), &required);
        return std::make_pair(code, required);
    });
    reached();
    fqdb_store_atomic_info info{};
    ok(fqdb_store_delete(raw, b("key"), &info, 1));
    hold.release();
    auto result = read.get();
    check(result.first == FQDB_NOT_FOUND && result.second == 0,
          "prepared weak-value fallback leaked a stale required length");
}
static void prepared_lifecycle(bool segmented, bool cache) {
    Fixture f;
    auto path = f.name("prepared");
    StoreTestClock clock(1000000);
    fengqudb::StoreOptions options;
    options.segmented = segmented;
    options.cache_bytes = cache ? 64ull << 20 : 0;
    auto native = options.native();
    fqdb_store *raw = nullptr;
    ok(fqdb_store_open(path.c_str(), &native, &raw));
    std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> db(raw, fqdb_store_close);
    const std::string name(300, 'k');
    fqdb_store_atomic_info info{};
    ok(fqdb_store_put(raw, b(name), b(message(1)), 1000010, &info));
    fqdb_store_key *key_raw = nullptr;
    ok(fqdb_store_key_prepare(raw, b(name), &key_raw));
    std::unique_ptr<fqdb_store_key, decltype(&fqdb_store_key_close)> key(key_raw,
                                                                         fqdb_store_key_close);
    check(prepared_get(key_raw) == message(1) && prepared_get(key_raw) == message(1),
          "long prepared key/value reuse");
    uint64_t required = 0;
    check(fqdb_store_key_get_into(key_raw, nullptr, 0, &required) == FQDB_BUFFER_TOO_SMALL &&
              required == message(1).size(),
          "prepared size probe lost required length");
    std::string target(size_t(required), '\0');
    ok(fqdb_store_key_get_into(key_raw, target.data(), target.size(), &required));
    check(target == message(1), "prepared reusable destination");
    fqdb_store_snapshot *snapshot = nullptr;
    ok(fqdb_store_snapshot_create(raw, &snapshot));
    ok(fqdb_store_put(raw, b(name), b(message(2)), 1000010, &info));
    check(prepared_get(key_raw) == message(2), "prepared put invalidation");
    fqdb_store_value *old = nullptr;
    ok(fqdb_store_snapshot_get_prepared_view(snapshot, key_raw, &old));
    auto bytes = fqdb_store_value_data(old);
    check(number({static_cast<const char *>(bytes.data), size_t(bytes.size)}) == 1 &&
              prepared_get(key_raw) == message(2),
          "snapshot resolve polluted latest prepared memo");
    fqdb_store_value_close(old);
    fqdb_store_snapshot_close(snapshot);
    clock.set(1000010);
    check(fqdb_store_key_get_into(key_raw, nullptr, 0, &required) == FQDB_NOT_FOUND && !required,
          "prepared memo hid TTL");
    clock.set(1000001);
    check(prepared_get(key_raw) == message(2), "prepared clock rollback");
    ok(fqdb_store_delete(raw, b(name), &info));
    check(fqdb_store_key_get_into(key_raw, nullptr, 0, &required) == FQDB_NOT_FOUND,
          "prepared delete invalidation");
    ok(fqdb_store_put(raw, b(name), b(message(3)), 0, &info));
    check(prepared_get(key_raw) == message(3), "prepared reinsertion invalidation");
    ok(fqdb_store_checkpoint(raw));
    fqdb_store_maintenance_info maintenance{};
    ok(fqdb_store_compact(raw, nullptr, &maintenance));
    check(prepared_get(key_raw) == message(3), "prepared generation change");
    std::atomic<bool> stop{false};
    std::vector<std::future<void>> readers;
    struct StopReads {
        std::atomic<bool> &stop;
        ~StopReads() {
            stop.store(true, std::memory_order_release);
        }
    } stop_on_failure{stop};
    for (unsigned t = 0; t < 3; ++t)
        readers.push_back(std::async(std::launch::async, [&] {
            uint64_t previous = 3;
            do {
                auto value = number(prepared_get(key_raw));
                check(value >= previous, "shared prepared handle regressed");
                previous = value;
            } while (!stop.load(std::memory_order_acquire));
        }));
    for (unsigned i = 4; i < 70; ++i)
        ok(fqdb_store_put(raw, b(name), b(message(i)), 0, &info));
    stop.store(true, std::memory_order_release);
    for (auto &reader : readers)
        reader.get();
    ok(fqdb_store_sync(raw));
    db.reset();
    check(prepared_get(key_raw) == message(69), "prepared child outlived parent incorrectly");
    key.reset();
    Store reopened(path, options);
    check(reopened.get(name)->view() == message(69), "prepared fixture restart");
}
static void projection(bool segmented, bool cache) {
    Fixture f;
    fengqudb::StoreOptions options;
    options.segmented = segmented;
    options.cache_bytes = cache ? 64ull << 20 : 0;
    StoreTestClock clock(1000000);
    fengqudb::Store db(f.name("projection"), options);
    db.put("key", message(7), 1000010);
    auto reader = db.prepare_project_reader({"key", "absent"}, {1, 1});
    check(db.project("key", {1, 1}) == selected(7) && db.project("key", {}).empty(),
          "direct projection");
    auto values = reader.read();
    check(values.size() == 2 && values[0].value == selected(7) && !values[1].value,
          "projection reader");
    auto held = reader.acquire();
    db.put("key", message(8), 1000010);
    check(held[0].value == selected(7), "acquired projection changed after write");
    check(reader.read()[0].value == selected(8) && db.project("key", {1}) == selected(8),
          "projected write invalidation");
    db.checkpoint();
    db.compact();
    check(reader.read()[0].value == selected(8), "projected generation invalidation");
    clock.set(1000010);
    check(!reader.read()[0].value, "projected TTL");
    check(status([&] { db.project("key", {1}); }) == FQDB_NOT_FOUND, "expired projection visible");
    clock.set(1000001);
    check(reader.read()[0].value == selected(8), "projection clock rollback");
    check(status([&] { db.project("key", {0}); }) == FQDB_INVALID_ARGUMENT, "invalid field number");
    check(status([&] { db.project("key", std::vector<uint32_t>(257, 1)); }) ==
              FQDB_INVALID_ARGUMENT,
          "field limit");
    // Malformed wire bytes are opaque Store values but invalid projection input.
    db.put("key", std::string(1, char(0x80)));
    check(status([&] { reader.read(); }) == FQDB_CONSTRAINT, "malformed projected cache reused");
    db.put("key", message(9));
    check(reader.read()[0].value == selected(9), "failed projection did not recover");
    // Alternating repeated fields exceed the bounded span list and use the fallback.
    fengqudb::wire::Writer input, expected;
    for (unsigned i = 0; i < 160; ++i) {
        input.uint_field(1, i);
        input.uint_field(2, i);
        expected.uint_field(1, i);
    }
    db.put("fragmented", input.data());
    check(db.project("fragmented", {1}) == expected.data(), "fragmented projection");
    auto tx = db.transaction();
    check(tx.project("fragmented", {1}) == expected.data(), "transaction projection");
    auto snapshot = db.snapshot(); // The direct projection must not consume another snapshot slot.
    check(db.project("fragmented", {2, 1, 2}) == input.data(), "ordered duplicate selection");
    reader.release();
    db.sync();
    db.verify();
}
static void mixed(bool segmented) {
    Fixture f;
    fengqudb::StoreOptions options;
    options.segmented = segmented;
    fengqudb::Store db(f.name("mixed"), options);
    db.put_many({{"a", message(0)}, {"b", message(0)}});
    std::atomic<bool> begin{false}, finished{false};
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> reads;
    for (unsigned i = 0; i < 4; ++i)
        reads.emplace_back([&] {
            try {
                auto reader = db.prepare_project_reader({"a", "b"}, {1});
                while (!begin.load(std::memory_order_acquire))
                    std::this_thread::yield();
                uint64_t previous = 0;
                do {
                    auto rows = reader.read();
                    check(rows[0].value && rows[1].value && rows[0].value == rows[1].value,
                          "torn projected batch");
                    auto value = number(*rows[0].value);
                    check(value >= previous, "projection version regressed");
                    previous = value;
                    auto one = db.get("a");
                    check(one && number(one->view()) >= value,
                          "ordinary memo stale after completed read");
                } while (!finished.load(std::memory_order_acquire));
            } catch (...) {
                ++errors;
            }
        });
    begin.store(true, std::memory_order_release);
    for (unsigned i = 1; i <= 600; ++i)
        db.put_many({{"a", message(i)}, {"b", message(i)}});
    finished.store(true, std::memory_order_release);
    for (auto &thread : reads)
        thread.join();
    check(!errors && db.project("a", {1}) == selected(600), "mixed reader correctness");
    db.sync();
}
static void isolation_and_limits() {
    Fixture f;
    fengqudb::StoreOptions options;
    options.max_snapshots = 1;
    fengqudb::Store db(f.name("isolation"), options);
    const std::string long_key(300, 'k');
    db.put(long_key, message(1));
    db.put("", message(2));
    check(db.project(long_key, {1}) == selected(1) && db.project("", {1}) == selected(2),
          "long/empty keys");
    auto snapshot = db.snapshot();
    check(db.project("", {1}) == selected(2), "read-only projection used snapshot slot");
    db.put("", message(3));
    check(snapshot.get("")->view() == message(2) && db.get("")->view() == message(3),
          "snapshot polluted by latest memo");
    auto constrained = options;
    constrained.pinned_bytes = 65536;
    fengqudb::Store small(f.name("pins"), constrained);
    small.put("large", message(1, 100000));
    auto reader = small.prepare_project_reader({"large"}, {2});
    check(status([&] { reader.read(); }) == FQDB_LIMIT, "projection pins bypassed budget");
    check(small.project("large", {1}) == selected(1), "small projection failed after pin limit");
}
static void selective_refresh(bool segmented, bool cache) {
    Fixture f;
    fengqudb::StoreOptions options;
    options.segmented = segmented;
    options.cache_bytes = cache ? 64ull << 20 : 0;
    fengqudb::Store db(f.name("selective"), options);
    db.put_many({{"a", message(1)}, {"b", message(2)}});
    auto reader = db.prepare_project_reader({"a", "b", "missing"}, {1});
    auto held = reader.acquire();
    const auto a = held[0].value->data(), bptr = held[1].value->data();
    db.put("unrelated", message(3));
    db.sync();
    db.checkpoint();
    auto rows = reader.read();
    check(rows[0].value->data() == a && rows[1].value->data() == bptr && !rows[2].value,
          "unrelated publication rebuilt projected payloads");
    db.put("b", message(4));
    rows = reader.read();
    check(rows[0].value->data() == a && rows[1].value == selected(4) &&
              held[1].value == selected(2),
          "partial refresh changed an unaffected row or acquired batch");
    db.put("a", message(1));
    rows = reader.read();
    check(rows[0].version > held[0].version && rows[0].value->data() != a,
          "equal-byte replacement reused obsolete metadata");
    db.put_many({{"a", message(5)}, {"b", std::string(1, char(0x80))}});
    check(status([&] { reader.read(); }) == FQDB_CONSTRAINT,
          "partly refreshed batch escaped failure");
    db.put("b", message(6));
    rows = reader.read();
    check(rows[0].value == selected(5) && rows[1].value == selected(6),
          "partial failure did not recover");
    db.put("missing", message(7));
    check(reader.read()[2].value == selected(7), "negative cache hid insertion");
    db.erase("missing");
    check(!reader.read()[2].value, "selective refresh hid deletion");
    db.sync();
    db.verify();
}
static void tight_refresh_budget() {
    Fixture f;
    fengqudb::StoreOptions options;
    options.pinned_bytes = 65536;
    fengqudb::Store db(f.name("refresh-budget"), options);
    db.put("a", message(1, 30000));
    auto reader = db.prepare_project_reader({"a"}, {2});
    check(reader.read()[0].value->size() > 30000, "initial budget fixture");
    db.put("a", message(2, 40000));
    check(reader.read()[0].value->size() > 40000,
          "incremental refresh rejected a valid fresh-read budget");
    reader.release();
    auto value = db.get("a");
    auto closed =
        std::async(std::launch::async, [owned = std::move(value)]() mutable { owned.reset(); });
    closed.get();
    check(db.info().pinned_bytes < 4096, "cross-thread value close leaked pins");
}
int main() {
    try {
        publications();
        prepared_missing_output();
        isolation_and_limits();
        for (bool segmented : {false, true}) {
            for (bool cache : {false, true})
                prepared_lifecycle(segmented, cache);
            for (bool cache : {false, true})
                selective_refresh(segmented, cache);
            for (bool cache : {false, true})
                projection(segmented, cache);
            mixed(segmented);
        }
        tight_refresh_budget();
        std::cout << "PASS read epochs, weak memo, projection plans, TTL, budgets and mixed "
                     "publication\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
