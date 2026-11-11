// 复杂查询基准：分别统计请求编码、核心执行和结果解码耗时，覆盖同参数重复、换参数、
// 两条查询交替和多线程场景。构建时找到 SQLite 就用预编译语句执行同语义查询作对照，
// 并逐行核对两边结果。数据先预热，测量的是缓存内的查询成本，不含磁盘 I/O。
#include "fengqudb_select.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef FQDB_BENCH_SQLITE
#include <sqlite3.h>
#endif

namespace fq = fengqudb;
using F = fq::QueryField;
using E = fq::QueryExpr;
using V = fq::QueryValue;
using Op = fq::QueryOp;
using A = fq::QueryAggregate;
using Clock = std::chrono::steady_clock;

static void check(bool ok, const std::string &what) {
    if (!ok)
        throw std::runtime_error(what);
}
static double us(Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

static const char *const classes[] = {"mage", "warrior", "rogue", "priest"};
static uint32_t class_of(uint64_t id) {
    return uint32_t((id * 2654435761ull >> 7) % 4);
}
static uint64_t level_of(uint64_t id) {
    return (id * 40503ull >> 3) % 100;
}
static int64_t gold_of(uint64_t id) {
    return int64_t((id * 7919) % 100000) - 1000;
}
static std::string name_of(uint64_t id) {
    return "Name-" + std::to_string(id);
}

// 一组可变参数；换参数场景按序轮换，确保相邻两次请求字节不同。
struct Param {
    uint32_t cls;
    uint64_t lo, hi;
    int64_t gold;
};
static std::vector<Param> make_params() {
    std::vector<Param> out;
    for (uint32_t i = 0; i < 64; ++i) {
        uint64_t lo = (i * 7) % 60;
        out.push_back({i % 4, lo, lo + 20 + i % 20, int64_t((i * 1237) % 60000)});
    }
    return out;
}

// --- 查询定义（FengquDB 与 SQLite 必须保持同一语义） ---
static fq::QueryColumn col(F field, A aggregate = A::None) {
    fq::QueryColumn c;
    c.field = std::move(field);
    c.aggregate = aggregate;
    return c;
}
static fq::QueryOrder ord(F field, bool descending = false) {
    fq::QueryOrder o;
    o.field = std::move(field);
    o.descending = descending;
    return o;
}
static fq::StoreQuery q_topn(const Param &p) {
    fq::StoreQuery q;
    q.prefix = "p/";
    q.where = E::all({E::compare(F::field(2, 4), Op::Eq, V::text(classes[p.cls])),
                      E::compare(F::field(3, 1), Op::Ge, V::uint(p.lo)),
                      E::compare(F::field(3, 1), Op::Le, V::uint(p.hi)),
                      E::compare(F::field(4, 5), Op::Ge, V::integer(p.gold))});
    q.columns = {col(F::field(1, 1)), col(F::field(4, 5))};
    q.order = {ord(F::field(4, 5), true), ord(F::field(1, 1))};
    q.limit = 20;
    return q;
}
static const char *s_topn = "SELECT id, gold FROM p WHERE class=?1 AND level BETWEEN ?2 AND ?3 "
                            "AND gold>=?4 ORDER BY gold DESC, id ASC LIMIT 20";
static fq::StoreQuery q_scan(int64_t gold) {
    fq::StoreQuery q;
    q.prefix = "p/";
    q.where = E::compare(F::field(4, 5), Op::Gt, V::integer(gold));
    q.columns = {col(F::field(1, 1))};
    q.order = {ord(F::field(1, 1))};
    q.limit = 100;
    return q;
}
static const char *s_scan = "SELECT id FROM p WHERE gold>?1 ORDER BY id LIMIT 100";
static fq::StoreQuery q_group() {
    fq::StoreQuery q;
    q.prefix = "p/";
    q.group = {F::field(2, 4)};
    q.columns = {col(F::field(2, 4)), col(F::field(1, 1), A::CountAll), col(F::field(4, 5), A::Sum)};
    q.order = {ord(F::column(0))};
    return q;
}
static const char *s_group = "SELECT class, COUNT(*), SUM(gold) FROM p GROUP BY class ORDER BY class";
static fq::StoreQuery q_join(uint32_t cls, uint64_t level) {
    fq::StoreQuery q;
    q.prefix = "p/";
    q.where = E::all({E::compare(F::field(2, 4), Op::Eq, V::text(classes[cls])),
                      E::compare(F::field(3, 1), Op::Eq, V::uint(level))});
    fq::QueryJoin j;
    j.prefix = "o/";
    j.left = {F::field(1, 1)};
    j.right = {F::field(1, 1, 1)};
    q.joins = {j};
    q.columns = {col(F::field(1, 1)), col(F::field(2, 1, 1))};
    q.limit = 4096;
    return q;
}
static const char *s_join = "SELECT p.id, o.amount FROM p JOIN o ON o.owner=p.id "
                            "WHERE p.class=?1 AND p.level=?2 LIMIT 4096";
static fq::StoreQuery q_text(Op op, const char *pattern) {
    fq::StoreQuery q;
    q.prefix = "p/";
    q.where = E::compare(F::field(5, 4), op, V::text(pattern));
    q.columns = {col(F::field(1, 1))};
    q.limit = 4096;
    return q;
}
// GLOB 使用 BINARY 排序规则，能像 FengquDB 的 LIKE 字面前缀一样走 name 索引。
static const char *s_like = "SELECT id FROM p WHERE name GLOB 'Name-12*' LIMIT 4096";
static const char *s_contains = "SELECT id FROM p WHERE instr(name, '777')>0 LIMIT 4096";

// --- FengquDB 执行与分段计时 ---
struct Phase {
    double encode = 0, execute = 0, decode = 0;
    uint64_t calls = 0, rows = 0;
};
static fq::StoreQueryResult run(fq::Store &db, const fq::StoreQuery &q, Phase &ph) {
    auto t0 = Clock::now();
    auto request = q.encode();
    auto t1 = Clock::now();
    // 默认走 Store 级只读快照路径；FQDB_BENCH_TXN=1 时走原事务路径作对照。
    static const bool txn_path = [] {
        const char *v = std::getenv("FQDB_BENCH_TXN");
        return v && *v == '1';
    }();
    fq::StoreCommand command{FQDB_SELECT, {}, {}, request};
    auto response = txn_path ? db.execute(command) : db.select_command(command);
    auto t2 = Clock::now();
    check(response.value.has_value(), "missing select response");
    fq::StoreQueryResult result(*response.value);
    auto t3 = Clock::now();
    ph.encode += us(t1 - t0);
    ph.execute += us(t2 - t1);
    ph.decode += us(t3 - t2);
    ++ph.calls;
    ph.rows += result.rows.size();
    return result;
}
static std::string text(const V &v) {
    switch (v.kind) {
    case fq::QueryKind::UInt:
    case fq::QueryKind::Bool:
        return std::to_string(v.unsigned_value);
    case fq::QueryKind::Int:
        return std::to_string(v.signed_value);
    case fq::QueryKind::Real: {
        char b[64];
        std::snprintf(b, sizeof b, "%.17g", v.real_value);
        return b;
    }
    case fq::QueryKind::Bytes:
        return v.bytes;
    default:
        return "null";
    }
}
static std::vector<std::string> rows_of(const fq::StoreQueryResult &r) {
    std::vector<std::string> out;
    for (auto &row : r.rows) {
        std::string line;
        for (auto &v : row.values)
            line += text(v) + "|";
        out.push_back(std::move(line));
    }
    return out;
}

// FQDB_BENCH_ONLY=名称[,名称...] 时只运行这些负载（用于 perf 剖析和快速 A/B）；为空则全部运行。
// 名称可写成 workload 或 engine:workload。
static bool selected(const char *engine, const char *name) {
    const char *only = std::getenv("FQDB_BENCH_ONLY");
    if (!only || !*only)
        return true;
    std::string list = std::string(",") + only + ",";
    return list.find("," + std::string(name) + ",") != std::string::npos ||
           list.find("," + std::string(engine) + ":" + name + ",") != std::string::npos;
}
// 过滤列表里没有任何 sqlite 负载时，跳过 SQLite 装数据和结果核对。
static bool sqlite_selected() {
    const char *only = std::getenv("FQDB_BENCH_ONLY");
    return !only || !*only || std::string(only).find("sqlite:") != std::string::npos;
}
// 每轮至少 200 ms，取 5 轮中位数；分段耗时为全部调用的平均值。
template <class Op_> static void bench(const char *engine, const char *name, Op_ &&op) {
    if (!selected(engine, name))
        return;
    Phase warm;
    for (int i = 0; i < 3; ++i)
        op(warm);
    std::vector<double> samples;
    Phase total;
    for (int round = 0; round < 5; ++round) {
        Phase ph;
        auto start = Clock::now();
        do
            op(ph);
        while (Clock::now() - start < std::chrono::milliseconds(200) || ph.calls < 3);
        samples.push_back(us(Clock::now() - start) / double(ph.calls));
        total.encode += ph.encode;
        total.execute += ph.execute;
        total.decode += ph.decode;
        total.calls += ph.calls;
        total.rows += ph.rows;
    }
    std::sort(samples.begin(), samples.end());
    auto n = double(total.calls);
    std::printf("{\"engine\":\"%s\",\"workload\":\"%s\",\"us_per_query\":%.2f,\"qps\":%.0f,"
                "\"encode_us\":%.2f,\"execute_us\":%.2f,\"decode_us\":%.2f,\"rows\":%.1f}\n",
                engine, name, samples[2], 1e6 / samples[2], total.encode / n, total.execute / n,
                total.decode / n, double(total.rows) / n);
    std::fflush(stdout);
}
// 多线程共享同一个 Store 句柄，每轮 300 ms，取 5 轮总吞吐中位数。
static void bench_threads(const char *name, unsigned threads,
                          const std::function<void(unsigned, uint64_t, Phase &)> &op) {
    if (!selected("fengqudb", name))
        return;
    std::vector<double> samples;
    for (int round = 0; round < 5; ++round) {
        std::atomic<bool> stop{false}, failed{false};
        std::atomic<uint64_t> calls{0};
        std::vector<std::thread> pool;
        auto start = Clock::now();
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                Phase ph;
                uint64_t i = 0;
                try {
                    while (!stop.load(std::memory_order_relaxed))
                        op(t, i++, ph);
                } catch (...) {
                    failed = true;
                }
                calls += ph.calls;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop = true;
        for (auto &th : pool)
            th.join();
        check(!failed, std::string("thread failure in ") + name);
        samples.push_back(double(calls) / (us(Clock::now() - start) / 1e6));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("{\"engine\":\"fengqudb\",\"workload\":\"%s\",\"threads\":%u,\"qps\":%.0f}\n", name,
                threads, samples[2]);
    std::fflush(stdout);
}

#ifdef FQDB_BENCH_SQLITE
struct Sql {
    sqlite3 *db = nullptr;
    explicit Sql(const std::string &path) {
        check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "sqlite open");
    }
    ~Sql() {
        sqlite3_close(db);
    }
    void exec(const char *sql) {
        char *error = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : "sqlite exec";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
};
struct Stmt {
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql) {
        check(sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK, sql);
    }
    ~Stmt() {
        sqlite3_finalize(s);
    }
    // 执行已绑定的语句；读取每列值以计入与 FengquDB 解码相当的消费成本。
    uint64_t run(std::vector<std::string> *out = nullptr) {
        uint64_t rows = 0;
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            ++rows;
            std::string line;
            for (int c = 0, n = sqlite3_column_count(s); c < n; ++c) {
                switch (sqlite3_column_type(s, c)) {
                case SQLITE_INTEGER:
                    if (out)
                        line += std::to_string(sqlite3_column_int64(s, c)) + "|";
                    else
                        (void)sqlite3_column_int64(s, c);
                    break;
                case SQLITE_TEXT:
                    line.assign(reinterpret_cast<const char *>(sqlite3_column_text(s, c)),
                                size_t(sqlite3_column_bytes(s, c)));
                    if (out)
                        line += "|";
                    break;
                default:
                    if (out)
                        line += "null|";
                }
            }
            if (out)
                out->push_back(std::move(line));
        }
        check(rc == SQLITE_DONE, "sqlite step");
        sqlite3_reset(s);
        return rows;
    }
};
static void load_sqlite(Sql &sql, uint64_t players) {
    sql.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF; PRAGMA cache_size=-262144;"
             "CREATE TABLE p(id INTEGER PRIMARY KEY, class TEXT NOT NULL, level INTEGER NOT NULL,"
             " gold INTEGER NOT NULL, name TEXT NOT NULL, pad BLOB NOT NULL);"
             "CREATE TABLE o(id INTEGER PRIMARY KEY, owner INTEGER NOT NULL, amount INTEGER NOT NULL);"
             "BEGIN;");
    std::string pad(256, 'x');
    {
        Stmt ins(sql.db, "INSERT INTO p VALUES(?1,?2,?3,?4,?5,?6)");
        for (uint64_t id = 0; id < players; ++id) {
            auto name = name_of(id);
            sqlite3_bind_int64(ins.s, 1, int64_t(id));
            sqlite3_bind_text(ins.s, 2, classes[class_of(id)], -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins.s, 3, int64_t(level_of(id)));
            sqlite3_bind_int64(ins.s, 4, gold_of(id));
            sqlite3_bind_text(ins.s, 5, name.data(), int(name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins.s, 6, pad.data(), int(pad.size()), SQLITE_STATIC);
            ins.run();
        }
        Stmt order(sql.db, "INSERT INTO o VALUES(?1,?2,?3)");
        for (uint64_t i = 0; i < players * 2; ++i) {
            sqlite3_bind_int64(order.s, 1, int64_t(i));
            sqlite3_bind_int64(order.s, 2, int64_t(i / 2));
            sqlite3_bind_int64(order.s, 3, int64_t((i * 13) % 5000));
            order.run();
        }
    }
    sql.exec("COMMIT; CREATE INDEX p_class_level ON p(class, level); CREATE INDEX p_name ON p(name);"
             "CREATE INDEX o_owner ON o(owner); ANALYZE;");
}
#endif

static void load_store(fq::Store &db, uint64_t players) {
    std::string pad(256, 'x');
    for (uint64_t base = 0; base < players; base += 1000) {
        auto tx = db.transaction();
        for (uint64_t id = base; id < std::min(players, base + 1000); ++id) {
            fq::wire::Writer w;
            w.uint_field(1, id);
            w.bytes_field(2, classes[class_of(id)]);
            w.uint_field(3, level_of(id));
            w.sint_field(4, gold_of(id));
            w.bytes_field(5, name_of(id));
            w.bytes_field(6, pad);
            tx.put("p/" + std::to_string(id), w.data());
        }
        check(bool(tx.commit()), "player load commit");
    }
    for (uint64_t base = 0; base < players * 2; base += 2000) {
        auto tx = db.transaction();
        for (uint64_t i = base; i < std::min(players * 2, base + 2000); ++i) {
            fq::wire::Writer w;
            w.uint_field(1, i / 2);
            w.uint_field(2, (i * 13) % 5000);
            tx.put("o/" + std::to_string(i), w.data());
        }
        check(bool(tx.commit()), "order load commit");
    }
    db.define_compound_index("class-level", "p/", {{2, 4}, {3, 1}}, 2);
    // SQLite 的 id 是 rowid 天然有序；为公平起见 FengquDB 也提供 id 有序索引。
    db.define_compound_index("id", "p/", {{1, 1}}, 3);
    db.define_compound_index("name", "p/", {{5, 4}}, 2);
    db.define_compound_index("owner", "o/", {{1, 1}}, 2);
    db.checkpoint();
}

int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 4)
            throw std::runtime_error("usage: fqdb_store_select_bench NEW_DIRECTORY [players] [threads]");
        auto root = std::filesystem::u8path(argv[1]);
        uint64_t players = argc > 2 ? std::stoull(argv[2]) : 50000;
        unsigned threads = argc > 3 ? unsigned(std::stoul(argv[3])) : 8;
        check(std::filesystem::create_directory(root), "benchmark directory must be new");
        fq::StoreOptions options;
        options.durability = fq::Durability::buffered;
        // 与 SQLite cache_size=256 MiB 对齐，避免把冷读当成查询执行成本。
        options.cache_bytes = 256ull << 20;
        options.pinned_bytes = 256ull << 20;
        auto db = fq::Store::open_at(root.u8string(), "main", options);
        load_store(db, players);
        const auto params = make_params();
        std::printf("{\"players\":%llu,\"orders\":%llu,\"threads\":%u}\n",
                    (unsigned long long)players, (unsigned long long)players * 2, threads);

        // 记录每类查询实际选中的访问路径，便于解释性能变化。
        auto plan = [&](const char *name, const fq::StoreQuery &q) {
            auto r = fq::explain(db, q);
            for (auto &a : r.access)
                std::printf("{\"plan\":\"%s\",\"source\":%u,\"strategy\":\"%s\",\"index\":\"%s\","
                            "\"ordered\":%d,\"covering\":%d,\"sampled\":%llu}\n",
                            name, a.source, a.strategy.c_str(), a.index.c_str(), int(a.ordered),
                            int(a.covering), (unsigned long long)a.sampled_candidates);
        };
        plan("topn", q_topn(params[0]));
        plan("scan", q_scan(90000));
        plan("group", q_group());
        plan("join", q_join(1, 42));
        plan("like", q_text(Op::Like, "Name-12%"));
        plan("contains", q_text(Op::Contains, "777"));
        // 场景对照：同参数重复（单条缓存可命中）、换参数、两条查询交替。
        auto same = q_topn(params[0]);
        bench("fengqudb", "topn_same_params", [&](Phase &ph) { run(db, same, ph); });
        size_t next = 0;
        bench("fengqudb", "topn_varying_params",
              [&](Phase &ph) { run(db, q_topn(params[next++ % params.size()]), ph); });
        // 公平对照：与 topn_varying_params 使用同一组参数，但每组参数先执行一次填充单条缓存，
        // 只统计紧接着的第二次（命中缓存）。execute_us 与换参负载之差即编译/选计划成本。
        next = 0;
        bench("fengqudb", "topn_varying_cached", [&](Phase &ph) {
            auto q = q_topn(params[next++ % params.size()]);
            Phase untimed;
            run(db, q, untimed);
            run(db, q, ph);
        });
        // 同候选集换字面量：class/level 固定，gold 下限低于全部数据，候选行和结果完全相同，
        // 只有请求字节变化。与 topn_same_params 之差就是编译+选计划成本，排除了 CPU 缓存冷热差异。
        next = 0;
        bench("fengqudb", "topn_varying_literal", [&](Phase &ph) {
            auto p = params[0];
            p.gold = -2000 - int64_t(next++ % 64);
            run(db, q_topn(p), ph);
        });
        {
            auto p = params[0];
            p.gold = -2000;
            auto fixed = q_topn(p);
            bench("fengqudb", "topn_same_literal", [&](Phase &ph) { run(db, fixed, ph); });
        }
        // 隔离编译/选计划成本：主键点查只触及 64 个热键，数据全部在缓存内。
        // point_varying 与 point_same 之差基本就是每次重新编译的成本。
        auto point = [](uint64_t id) {
            fq::StoreQuery q;
            q.prefix = "p/";
            q.where = E::compare(F::key(), Op::Eq, V::text("p/" + std::to_string(id)));
            q.columns = {col(F::field(1, 1)), col(F::field(4, 5))};
            q.limit = 1;
            return q;
        };
        auto point_same = point(7);
        bench("fengqudb", "point_same", [&](Phase &ph) { run(db, point_same, ph); });
        next = 0;
        bench("fengqudb", "point_varying", [&](Phase &ph) { run(db, point(next++ % 64), ph); });
        auto other = q_scan(90000);
        bool flip = false;
        bench("fengqudb", "topn_scan_alternating", [&](Phase &ph) {
            run(db, (flip = !flip) ? same : other, ph);
        });
        auto prepared_same = fq::StorePreparedQuery(same);
        bench("fengqudb", "topn_prepared_same", [&](Phase &ph) {
            auto t1 = Clock::now();
            auto r = prepared_same.execute(db);
            ph.execute += us(Clock::now() - t1);
            ++ph.calls;
            ph.rows += r.rows.size();
        });
        // 执行器负载：均为同参数重复，衡量核心执行本身。
        auto scan = q_scan(90000);
        bench("fengqudb", "scan_filter_top100", [&](Phase &ph) { run(db, scan, ph); });
        auto group = q_group();
        bench("fengqudb", "group_count_sum", [&](Phase &ph) { run(db, group, ph); });
        auto join = q_join(1, 42);
        bench("fengqudb", "index_join", [&](Phase &ph) { run(db, join, ph); });
        next = 0;
        bench("fengqudb", "index_join_varying", [&](Phase &ph) {
            auto i = next++;
            run(db, q_join(uint32_t(i % 4), i % 100), ph);
        });
        next = 0;
        bench("fengqudb", "index_join_varying_cached", [&](Phase &ph) {
            auto i = next++;
            auto q = q_join(uint32_t(i % 4), i % 100);
            Phase untimed;
            run(db, q, untimed);
            run(db, q, ph);
        });
        auto like = q_text(Op::Like, "Name-12%");
        bench("fengqudb", "like_prefix", [&](Phase &ph) { run(db, like, ph); });
        auto contains = q_text(Op::Contains, "777");
        bench("fengqudb", "contains_scan", [&](Phase &ph) { run(db, contains, ph); });
        // 上限参照：同一前缀只数键、不读业务载荷，代表"不取值"的扫描每行成本下限。
        {
            fq::StoreQuery q;
            q.prefix = "p/";
            q.columns = {col(F::key(), A::CountAll)};
            bench("fengqudb", "count_keys_only", [&](Phase &ph) { run(db, q, ph); });
        }

        bench_threads("mt_topn_same_params", threads,
                      [&](unsigned, uint64_t, Phase &ph) { run(db, same, ph); });
        bench_threads("mt_topn_varying_params", threads, [&](unsigned t, uint64_t i, Phase &ph) {
            run(db, q_topn(params[(t * 7 + i) % params.size()]), ph);
        });

#ifdef FQDB_BENCH_SQLITE
        if (!sqlite_selected())
            return 0;
        auto path = (root / "sqlite.db").u8string();
        Sql sql(path);
        load_sqlite(sql, players);
        Stmt topn(sql.db, s_topn), sscan(sql.db, s_scan), sgroup(sql.db, s_group),
            sjoin(sql.db, s_join), slike(sql.db, s_like), scontains(sql.db, s_contains);
        auto bind_topn = [&](const Param &p) {
            sqlite3_bind_text(topn.s, 1, classes[p.cls], -1, SQLITE_STATIC);
            sqlite3_bind_int64(topn.s, 2, int64_t(p.lo));
            sqlite3_bind_int64(topn.s, 3, int64_t(p.hi));
            sqlite3_bind_int64(topn.s, 4, p.gold);
        };
        auto bind_join = [&](uint32_t cls, uint64_t level) {
            sqlite3_bind_text(sjoin.s, 1, classes[cls], -1, SQLITE_STATIC);
            sqlite3_bind_int64(sjoin.s, 2, int64_t(level));
        };
        sqlite3_bind_int64(sscan.s, 1, 90000);
        // 结果核对：有序查询逐行比较，无序查询排序后比较。
        auto same_rows = [&](const char *what, std::vector<std::string> a, std::vector<std::string> b,
                             bool ordered) {
            if (!ordered) {
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
            }
            check(a == b, std::string("result mismatch: ") + what + " fengqudb=" +
                              std::to_string(a.size()) + " sqlite=" + std::to_string(b.size()));
        };
        Phase ignore;
        for (auto &p : params) {
            std::vector<std::string> expected;
            bind_topn(p);
            topn.run(&expected);
            same_rows("topn", rows_of(run(db, q_topn(p), ignore)), expected, true);
        }
        std::vector<std::string> expected;
        sscan.run(&expected);
        same_rows("scan", rows_of(run(db, scan, ignore)), expected, true);
        expected.clear();
        sgroup.run(&expected);
        same_rows("group", rows_of(run(db, group, ignore)), expected, true);
        expected.clear();
        bind_join(1, 42);
        sjoin.run(&expected);
        same_rows("join", rows_of(run(db, join, ignore)), expected, false);
        expected.clear();
        slike.run(&expected);
        same_rows("like", rows_of(run(db, like, ignore)), expected, false);
        expected.clear();
        scontains.run(&expected);
        same_rows("contains", rows_of(run(db, contains, ignore)), expected, false);
        std::printf("{\"check\":\"sqlite_results_equal\",\"ok\":true}\n");

        auto count = [](Phase &ph, uint64_t rows) {
            ++ph.calls;
            ph.rows += rows;
        };
        bind_topn(params[0]);
        bench("sqlite", "topn_same_params", [&](Phase &ph) { count(ph, topn.run()); });
        next = 0;
        bench("sqlite", "topn_varying_params", [&](Phase &ph) {
            bind_topn(params[next++ % params.size()]);
            count(ph, topn.run());
        });
        bench("sqlite", "scan_filter_top100", [&](Phase &ph) { count(ph, sscan.run()); });
        bench("sqlite", "group_count_sum", [&](Phase &ph) { count(ph, sgroup.run()); });
        bind_join(1, 42);
        bench("sqlite", "index_join", [&](Phase &ph) { count(ph, sjoin.run()); });
        next = 0;
        bench("sqlite", "index_join_varying", [&](Phase &ph) {
            auto i = next++;
            bind_join(uint32_t(i % 4), i % 100);
            count(ph, sjoin.run());
        });
        bench("sqlite", "like_prefix", [&](Phase &ph) { count(ph, slike.run()); });
        bench("sqlite", "contains_scan", [&](Phase &ph) { count(ph, scontains.run()); });
#endif
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
