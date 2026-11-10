#include "store_test_support.hpp"
#include "paged/operations.hpp"
#include <atomic>

using namespace std::chrono_literals;
static fqdb_slice b(std::string_view value) {
    return {value.data(), value.size()};
}
static void ok(fqdb_status value) {
    if (value)
        throw fengqudb::Error(value);
}
using Db = std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)>;
static Db open(const std::string &path, bool segmented, bool reliable = false,
               uint64_t transaction_bytes = 64ull << 20) {
    auto options = p::defaults();
    options.flags |= segmented ? FQDB_STORE_SEGMENTED : 0;
    options.transaction_bytes = transaction_bytes;
    fqdb_store *raw = nullptr;
    ok(fqdb_store_open(path.c_str(), &options, &raw));
    Db db(raw, fqdb_store_close);
    fqdb_store_write_options write{
        sizeof(write), 1, uint32_t(reliable ? FQDB_WRITE_RELIABLE : FQDB_WRITE_PERFORMANCE), 5000,
        1048576};
    ok(fqdb_store_write_configure(raw, &write));
    return db;
}
static std::string record(int64_t credit, size_t body = 4096) {
    fengqudb::wire::Writer w;
    w.fixed64_field(1, static_cast<uint64_t>(credit));
    w.fixed64_field(2, 7);
    w.bytes_field(3, std::string(body, 'x'));
    return w.take();
}
static void put(fqdb_store *db, std::string_view key, std::string_view value, uint64_t expiry = 0) {
    fqdb_store_atomic_info out{};
    ok(fqdb_store_put(db, b(key), b(value), expiry, &out, 1));
}
static std::string get(fqdb_store *db, std::string_view key) {
    fqdb_store_shared_value out{};
    ok(fqdb_store_get_shared(db, b(key), &out));
    std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owner(
        out.owner, fqdb_store_value_close);
    return {static_cast<const char *>(out.row.value.data), size_t(out.row.value.size)};
}
static int64_t credit(fqdb_store *db, std::string_view key, size_t offset = 1) {
    auto value = get(db, key);
    uint64_t bits =
        fengqudb::storage::get64(reinterpret_cast<const unsigned char *>(value.data() + offset));
    int64_t out;
    std::memcpy(&out, &bits, 8);
    return out;
}
static fqdb_store_info info(fqdb_store *db) {
    fqdb_store_info out{};
    ok(fqdb_store_get_info(db, &out));
    return out;
}
static auto transfer(std::string_view from, std::string_view to, int64_t amount = 1) {
    return std::array<fqdb_store_i64_field_update, 2>{
        {{b(from), 1, -amount, 0, INT64_MAX}, {b(to), 1, amount, 0, INT64_MAX}}};
}
static fqdb_store_atomic_info field_apply(fqdb_store *db,
                                          const std::vector<fqdb_store_i64_field_update> &items,
                                          bool reliable = false) {
    fqdb_store_atomic_info out{};
    ok(fqdb_store_add_i64_fields(db, items.data(), uint32_t(items.size()), 0, &out, reliable));
    return out;
}
static void semantics(bool segmented, bool reliable) {
    Fixture f;
    auto path = f.name("semantics");
    auto db = open(path, segmented, reliable);
    auto expiry = p::unix_ms() + 600000;
    put(db.get(), "a", record(100), expiry);
    put(db.get(), "b", record(200));
    auto version = info(db.get()).version;
    auto out =
        field_apply(db.get(), {{b("a"), 1, -10, 0, INT64_MAX}, {b("b"), 1, 10, 0, INT64_MAX}});
    check(out.version == version + 1 && out.observed_version == version && out.value == 210 &&
              out.changed && out.matched && credit(db.get(), "a") == 90 &&
              credit(db.get(), "b") == 210,
          "multi-key result/visibility");
    fqdb_store_shared_value shared{};
    ok(fqdb_store_get_shared(db.get(), b("a"), &shared));
    check(shared.row.expires_at_ms == expiry, "field command changed TTL");
    fqdb_store_value_close(shared.owner);
    version = out.version;
    auto unchanged = field_apply(db.get(), {{b("a"), 1, 0, 0, 100}}, true);
    check(!unchanged.changed && unchanged.version == version &&
              info(db.get()).durable_version == version,
          "forced no-op did not persist prefix");
    // Input order, duplicate keys and buffer reuse across different fields.
    field_apply(db.get(),
                {{b("a"), 1, 2, 0, 100}, {b("a"), 1, 3, 0, 100}, {b("a"), 10, -4, 0, 100}}, true);
    check(credit(db.get(), "a") == 95 && credit(db.get(), "a", 10) == 3, "ordered fields");
    auto before = get(db.get(), "a");
    version = info(db.get()).version;
    for (auto bad :
         std::vector<fqdb_store_i64_field_update>{{b("missing"), 1, 1, 0, INT64_MAX},
                                                  {b("b"), UINT64_MAX, 1, 0, INT64_MAX},
                                                  {b("b"), 1, INT64_MAX, INT64_MIN, INT64_MAX},
                                                  {b("b"), 1, -211, 0, INT64_MAX}}) {
        fqdb_store_i64_field_update items[]{{b("a"), 1, -1, 0, 100}, bad};
        fqdb_store_atomic_info failed{1, 2, 3, 4, 5};
        auto code = fqdb_store_add_i64_fields(db.get(), items, 2, 0, &failed, 0);
        check(code != FQDB_OK && !failed.version && !failed.observed_version && !failed.value &&
                  !failed.matched && !failed.changed && get(db.get(), "a") == before &&
                  info(db.get()).version == version,
              "failed command partially published");
    }
    put(db.get(), "negative", record(0));
    auto edge = field_apply(db.get(), {{b("negative"), 1, INT64_MIN, INT64_MIN, INT64_MAX}}, true);
    check(edge.value == INT64_MIN, "INT64_MIN delta overflowed");
    check(status([&] { field_apply(db.get(), {{b("negative"), 1, -1, INT64_MIN, INT64_MAX}}); }) ==
              FQDB_CONSTRAINT,
          "negative overflow accepted");
    std::vector<fqdb_store_i64_field_update> maximum(256, {b("a"), 1, 0, 0, 100});
    check(!field_apply(db.get(), maximum).changed, "256-item no-op failed");
    fqdb_store_atomic_info unused{};
    check(fqdb_store_add_i64_fields(db.get(), nullptr, 0, 0, &unused, 0) == FQDB_INVALID_ARGUMENT &&
              fqdb_store_add_i64_fields(db.get(), maximum.data(), 257, 0, &unused, 0) ==
                  FQDB_INVALID_ARGUMENT &&
              fqdb_store_add_i64_fields(db.get(), maximum.data(), 1, 0, &unused, 2) ==
                  FQDB_INVALID_ARGUMENT,
          "invalid native arguments accepted");
    ok(fqdb_store_sync(db.get()));
    check(info(db.get()).transaction_bytes == 0, "private budget retained");
    db.reset();
    db = open(path, segmented, reliable);
    check(get(db.get(), "a") == before && credit(db.get(), "b") == 210 &&
              credit(db.get(), "negative") == INT64_MIN,
          "field reopen changed data");
}
static void parallel_fields(bool segmented) {
    Fixture f;
    auto db = open(f.name("concurrent"), segmented);
    put(db.get(), "a", record(100000));
    put(db.get(), "b", record(100000));
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 8; ++thread)
        workers.emplace_back([&, thread] {
            auto items = thread % 2 ? transfer("b", "a") : transfer("a", "b");
            for (unsigned i = 0; i < 200; ++i) {
                fqdb_store_atomic_info out{};
                if (fqdb_store_add_i64_fields(db.get(), items.data(), 2, 0, &out, 0) != FQDB_OK)
                    ++errors;
            }
        });
    for (auto &thread : workers)
        thread.join();
    ok(fqdb_store_sync(db.get()));
    check(!errors && credit(db.get(), "a") == 100000 && credit(db.get(), "b") == 100000 &&
              !info(db.get()).transaction_bytes,
          "concurrent transfer lost data or leaked budget");
}
static void constraints_and_limits() {
    Fixture f;
    auto db = open(f.name("index"), false);
    put(db.get(), "user/a", record(100));
    put(db.get(), "user/b", record(200));
    fqdb_store_maintenance_info maintenance{};
    ok(fqdb_store_index_define(db.get(), b("credit"), b("user/"), 1, 8, 1, nullptr, &maintenance));
    auto before = info(db.get()).version;
    check(status([&] { field_apply(db.get(), {{b("user/a"), 1, 100, 0, 1000}}); }) ==
                  FQDB_CONSTRAINT &&
              info(db.get()).version == before && credit(db.get(), "user/a") == 100,
          "writer command bypassed unique index");
    field_apply(db.get(), {{b("user/a"), 1, 1, 0, 1000}}, true);
    ok(fqdb_store_verify(db.get(), nullptr, &maintenance));
    ok(fqdb_store_compact(db.get(), nullptr, &maintenance));
    field_apply(db.get(), {{b("user/a"), 1, 1, 0, 1000}}, true);
    ok(fqdb_store_verify(db.get(), nullptr, &maintenance));
    auto limited = open(f.name("limit"), false, false, 1048576);
    put(limited.get(), "a", record(100, 400000));
    put(limited.get(), "b", record(100, 400000));
    check(status([&] {
              field_apply(limited.get(), {{b("a"), 1, -1, 0, 1000}, {b("b"), 1, 1, 0, 1000}});
          }) == FQDB_LIMIT &&
              credit(limited.get(), "a") == 100 && credit(limited.get(), "b") == 100 &&
              !info(limited.get()).transaction_bytes,
          "budget failure was not atomic");
    auto expired = p::unix_ms() - 1;
    put(db.get(), "expired", record(1), expired);
    check(status([&] { field_apply(db.get(), {{b("expired"), 1, 1, 0, 100}}); }) == FQDB_NOT_FOUND,
          "expired record resurrected");
    fengqudb::Store cpp(f.name("cpp"));
    cpp.put("a", record(5));
    cpp.put("b", record(10));
    check(cpp.add_i64_fields(std::vector<fengqudb::StoreI64FieldUpdate>{{"a", 1, -1, 0, 100},
                                                                        {"b", 1, 1, 0, 100}},
                             0, true)
                  .value == 11,
          "C++ field wrapper");
    auto runtime = fengqudb::StoreRuntimeOptions{};
    check(runtime.interval_ms == 200 && runtime.compact_min_bytes == 33554432 &&
              runtime.compact_growth_bytes == 33554432,
          "maintenance defaults");
}
static void queued_timeout() {
    Fixture f;
    auto db = open(f.name("timeout"), false);
    put(db.get(), "a", record(100));
    put(db.get(), "b", record(100));
    Held hold(43);
    auto first = std::async(std::launch::async,
                            [&] { return field_apply(db.get(), {{b("a"), 1, 1, 0, 1000}}); });
    reached();
    auto second = std::async(std::launch::async, [&] {
        auto items = transfer("a", "b");
        fqdb_store_atomic_info out{};
        return fqdb_store_add_i64_fields(db.get(), items.data(), 2, 1, &out, 0);
    });
    std::this_thread::sleep_for(20ms);
    hold.release();
    first.get();
    check(second.get() == FQDB_TIMEOUT && credit(db.get(), "a") == 101 &&
              credit(db.get(), "b") == 100,
          "expired queued command changed state");
}
static void io_failure(bool segmented, bool sync_failure) {
    Fixture f;
    auto path = f.name("failure");
    auto db = open(path, segmented);
    put(db.get(), "a", record(100));
    put(db.get(), "b", record(100));
    auto items = transfer("a", "b");
    fqdb_store_atomic_info out{1, 1, 1, 1, 1};
    fqdb_store_test_io_fault(sync_failure ? 0 : 1, sync_failure ? 1 : 0);
    auto code = fqdb_store_add_i64_fields(db.get(), items.data(), 2, 0, &out, 1);
    fqdb_store_test_io_fault(0, 0);
    check(code == FQDB_COMMIT_UNKNOWN && !out.version && !out.matched && !out.changed,
          "I/O failure falsely acknowledged field command");
    db.reset();
    db = open(path, segmented);
    auto a = credit(db.get(), "a"), bvalue = credit(db.get(), "b");
    check((a == 100 && bvalue == 100) || (a == 99 && bvalue == 101), "I/O recovery split command");
}
int main(int argc, char **argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--crash") {
            fqdb_store *raw = nullptr;
            ok(fqdb_store_open(argv[2], nullptr, &raw));
            auto items = transfer("a", "b");
            fqdb_store_atomic_info out{};
            fqdb_store_test_crash(std::stoi(argv[4]));
            ok(fqdb_store_add_i64_fields(raw, items.data(), 2, 0, &out, 1));
            return 2;
        }
        for (bool segmented : {false, true}) {
            semantics(segmented, false);
            semantics(segmented, true);
            parallel_fields(segmented);
            io_failure(segmented, false);
            io_failure(segmented, true);
        }
        constraints_and_limits();
        queued_timeout();
#ifndef __SANITIZE_THREAD__
        for (bool segmented : {false, true})
            for (int point : {1, 2, 3, 4}) {
                Fixture f;
                auto path = f.name("crash");
                auto db = open(path, segmented);
                put(db.get(), "a", record(100));
                put(db.get(), "b", record(100));
                db.reset();
                auto exe = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
                check(child(exe, "--crash", path, "unused", point) == 86, "field crash child");
                db = open(path, segmented);
                auto a = credit(db.get(), "a"), bvalue = credit(db.get(), "b");
                check((a == 100 && bvalue == 100) || (a == 99 && bvalue == 101),
                      "torn multi-key recovery");
                check(point < 3 || (a == 99 && bvalue == 101), "durable transfer lost");
            }
#endif
        std::cout << "PASS field commands, ordered edits, atomic constraints/budgets, timeout, "
                     "concurrent transfers and recovery\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
