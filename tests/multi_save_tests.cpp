#include "store_test_support.hpp"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <random>
#ifndef _WIN32
#include <sys/wait.h>
#endif
static void ok(fqdb_status s) {
    if (s)
        throw std::runtime_error(fqdb_status_message(s));
}
static fqdb_slice slice(const std::string &s) {
    return {s.data(), s.size()};
}
static void put(fqdb_store *db, const std::string &value, bool reliable = false) {
    fqdb_store_atomic_info i{};
    ok(fqdb_store_put(db, {"k", 1}, slice(value), 0, &i, reliable));
}
static void get(fqdb_store *db, const std::string &want) {
    fqdb_buffer b{};
    ok(fqdb_store_get(db, {"k", 1}, &b));
    bool same = b.size == want.size() && !std::memcmp(b.data, want.data(), want.size());
    fqdb_buffer_free(&b);
    check(same, "value mismatch");
}
static std::string changed(std::string v, unsigned round) {
    v[12] = char(round);
    v[1234] = char(round + 1);
    v[v.size() - 1] = char(round + 2);
    return v;
}
static fqdb_store *open(const std::string &path, bool segmented = false,
                        uint64_t cache = 64 * 1048576) {
    fqdb_store_options o{sizeof(o),
                         1,
                         FQDB_DURABILITY_SYNC,
                         FQDB_STORE_CREATE,
                         cache,
                         4 * 1048576,
                         32 * 1048576,
                         64 * 1048576,
                         256 * 1048576,
                         128 * 1048576,
                         1024};
    if (segmented)
        o.flags |= FQDB_STORE_SEGMENTED;
    fqdb_store *db = nullptr;
    ok(fqdb_store_open(path.c_str(), &o, &db));
    return db;
}
static unsigned scan(const std::string &path) {
    auto c = p::read_current(path);
    auto g = std::make_shared<p::Generation>(0);
    g->open(p::join(path, c.filename()), c.uuid, c.generation, true);
    uint64_t at = p::data_start;
    unsigned multi = 0;
    while (at < g->tail) {
        auto f = g->frame(at, g->tail);
        if (!f)
            break;
        if (f->kind == p::Frame::Kind::wal)
            p::read_wal(*g, *f, [&](const p::WalOperation &op) {
                if (op.kind == p::OpKind::multi) {
                    ++multi;
                    check(op.ranges.size() <= 8, "range limit");
                }
                if (!op.result.deleted())
                    (void)g->value(op.result, f->end, false, false);
            });
        at = f->end;
    }
    return multi;
}
int main(int argc, char **argv) try {
    const bool crashing = argc == 5 && std::string_view(argv[1]) == "--crash";
    std::unique_ptr<Fixture> fixture;
    if (!crashing)
        fixture = std::make_unique<Fixture>();
    std::string root = crashing ? argv[2] : fixture->root;
    if (crashing) {
        const int point = std::stoi(argv[4]);
        auto db = open(root + "/crash");
        fqdb_store_write_options policy{sizeof(policy), 1, FQDB_WRITE_PERFORMANCE, 5000, 1048576};
        ok(fqdb_store_write_configure(db, &policy));
        std::string v(4173, 'a');
        put(db, v, true);
        if (point == 9) {
            for (unsigned r = 1; r <= 5; ++r) {
                v = changed(v, r);
                put(db, v, true);
            }
            v = changed(v, 6);
            put(db, v); // Accepted RAM suffix must not erase the durable prefix.
        } else {
            get(db, v);
            fqdb_store_test_crash(point);
            v = changed(v, 1);
            put(db, v, true);
        }
        std::_Exit(86);
    }
    for (const char *name : {"direct", "seg", "random", "backup", "crash"})
        fixture->name(name);
    unsigned multi = 0;
    for (bool segmented : {false, true}) {
        auto path = root + (segmented ? "/seg" : "/direct");
        auto db = open(path, segmented);
        std::string v(4173, 'a');
        put(db, v);
        get(db, v);
        for (unsigned r = 1; r <= 25; ++r) {
            v = changed(v, r);
            put(db, v, r % 3 == 0);
            get(db, v);
            if (r == 6) {
                ok(fqdb_store_sync(db));
                fqdb_store_close(db);
                db = open(path, segmented, 0);
                get(db, v);
                fqdb_store_close(db);
                db = open(path, segmented);
                get(db, v);
            }
        }
        // A complete replacement with many changes, changed length and identical put.
        v.assign(4173, 'z');
        put(db, v);
        get(db, v);
        v.push_back('!');
        put(db, v);
        get(db, v);
        put(db, v);
        get(db, v);
        ok(fqdb_store_sync(db));
        fqdb_store_close(db);
        multi += scan(path);
        db = open(path, segmented, 0);
        get(db, v);
        fqdb_store_close(db);
    }
    check(multi > 10, "multi recipes not exercised");
    {
        auto path = root + "/random";
        auto db = open(path);
        std::string v(4173, 'r');
        put(db, v, true);
        fqdb_store_value *held = nullptr;
        ok(fqdb_store_get_view(db, {"k", 1}, &held));
        std::mt19937 random(77);
        for (unsigned r = 0; r < 80; ++r) {
            for (unsigned i = 0; i < 1 + r % 12; ++i)
                v[random() % v.size()] = char(random());
            // Exercise the transaction path, not just the single-write fast path.
            fqdb_store_txn *tx = nullptr;
            ok(fqdb_store_txn_begin(db, 0, &tx));
            ok(fqdb_store_txn_put(tx, {"k", 1}, slice(v), 0));
            ok(fqdb_store_txn_put(tx, {"other", 5}, slice(v), 0));
            uint64_t version = 0;
            ok(fqdb_store_txn_commit(tx, &version, r % 5 == 0));
            fqdb_store_txn_close(tx);
            get(db, v);
        }
        auto old = fqdb_store_value_data(held);
        check(old.size == 4173 && std::string(static_cast<const char *>(old.data), old.size) ==
                                      std::string(4173, 'r'),
              "shared bytes mutated");
        fqdb_store_value_close(held);
        v.assign(4173, 'q');
        put(db, v, true);
        get(db, v); // Force a raw base below the recipe depth cap.
        // A second single-range patch falls back to put while retaining its
        // old patch offset. A new multi recipe must not serialize that offset.
        fqdb_store_txn *tx = nullptr;
        ok(fqdb_store_txn_begin(db, 0, &tx));
        ok(fqdb_store_txn_patch(tx, {"k", 1}, 42, 1, {"x", 1}));
        ok(fqdb_store_txn_patch(tx, {"k", 1}, 4000, 1, {"y", 1}));
        uint64_t version = 0;
        ok(fqdb_store_txn_commit(tx, &version, true));
        fqdb_store_txn_close(tx);
        v[42] = 'x';
        v[4000] = 'y';
        get(db, v);
        fqdb_store_maintenance_info info{};
        ok(fqdb_store_verify(db, nullptr, &info));
        ok(fqdb_store_backup(db, (root + "/backup").c_str(), nullptr, &info));
        ok(fqdb_store_compact(db, nullptr, &info));
        get(db, v);
        fqdb_store_close(db);
        db = open(root + "/backup", false, 0);
        get(db, v);
        fqdb_store_close(db);
        db = open(path, false, 0);
        get(db, v);
        fqdb_store_close(db);
    }
    {
        auto path = fixture->name("concurrent");
        auto db = open(path);
        std::vector<std::future<void>> writers;
        for (unsigned thread = 0; thread < 4; ++thread)
            writers.push_back(std::async(std::launch::async, [&, thread] {
                auto key = std::string("key/") + std::to_string(thread);
                std::string value(4173, 'a');
                for (unsigned round = 0; round <= 50; ++round) {
                    value = changed(value, round);
                    fqdb_store_atomic_info info{};
                    ok(fqdb_store_put(db, slice(key), slice(value), 0, &info, round % 13 == 0));
                }
            }));
        for (auto &writer : writers)
            writer.get();
        ok(fqdb_store_sync(db));
        fqdb_store_close(db);
        db = open(path, false, 0);
        for (unsigned thread = 0; thread < 4; ++thread) {
            auto key = std::string("key/") + std::to_string(thread);
            fqdb_buffer value{};
            ok(fqdb_store_get(db, slice(key), &value));
            auto expected = changed(std::string(4173, 'a'), 50);
            const bool same = value.size == expected.size() &&
                              !std::memcmp(value.data, expected.data(), expected.size());
            fqdb_buffer_free(&value);
            check(same, "concurrent save recovery");
        }
        fqdb_store_close(db);
    }
#ifndef __SANITIZE_THREAD__
    auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
    for (int point : {1, 2, 3, 4, 9}) {
        Fixture killed;
        auto dbpath = killed.name("crash");
        check(child(executable, "--crash", killed.root, "unused", point) == 86,
              "child did not reach crash point");
        auto db = open(dbpath, false, 0);
        fqdb_buffer b{};
        ok(fqdb_store_get(db, {"k", 1}, &b));
        std::string actual(static_cast<const char *>(b.data), b.size);
        fqdb_buffer_free(&b);
        auto seed = std::string(4173, 'a'), updated = changed(seed, point == 9 ? 5 : 1);
        check(point == 1   ? actual == seed
              : point == 2 ? (actual == seed || actual == updated)
                           : actual == updated,
              "crash recovery prefix");
        fqdb_store_close(db);
    }
#endif
    std::cout << "{\"ok\":true,\"multi_records\":" << multi
              << ",\"cold_reopen\":true,\"concurrent_saves\":true"
#ifndef __SANITIZE_THREAD__
              << ",\"abrupt_exit_recovery\":true}\n";
#else
              << ",\"abrupt_exit_recovery\":\"skipped under TSan\"}\n";
#endif
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
