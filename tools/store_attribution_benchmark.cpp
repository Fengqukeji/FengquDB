// Attribution benchmark: per-row cost of the basic operations, on identical data, for
// FengquDB, SQLite (when built with FQDB_BENCH_SQLITE) and a plain in-memory array.
// It answers "where does the gap start": storage layer (scan/point read), protobuf field
// parse, or the query executor on top. Everything runs warm, single thread.
//   usage: fqdb_store_attribution_bench NEW_DIRECTORY [rows] [pad_bytes]
#include "fengqudb_select.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#ifdef FQDB_BENCH_SQLITE
#include <sqlite3.h>
#endif

namespace fq = fengqudb;
using Clock = std::chrono::steady_clock;
static volatile uint64_t sink;

static std::string make_value(uint64_t id, size_t pad) {
    fq::wire::Writer w;
    w.uint_field(1, id);
    w.bytes_field(2, id % 2 ? "mage" : "warrior");
    w.uint_field(3, id % 100);
    w.sint_field(4, int64_t(id) * 7 - 1000);
    w.bytes_field(5, "Name-" + std::to_string(id));
    w.bytes_field(6, std::string(pad, 'x'));
    return w.take();
}
// Reads the whole record like the query engine does (last occurrence wins), returns field 4.
static uint64_t parse_gold(std::string_view v) {
    fq::wire::Reader r(v);
    fq::wire::Field f;
    uint64_t out = 0;
    while (r.next(f)) {
        if (f.number == 4 && f.type == fq::wire::Type::Varint) {
            uint64_t x = 0;
            r.varint(x);
            out = x;
        } else if (!r.skip(f))
            return 0;
    }
    return out;
}
// True when field 5 (the name) contains "777". Parses like the engine: the whole record,
// last occurrence wins. Mirrors the contains_scan workload of the query benchmark.
static bool contains_name(std::string_view v) {
    fq::wire::Reader r(v);
    fq::wire::Field f;
    std::string_view name;
    while (r.next(f)) {
        if (f.number == 5 && f.type == fq::wire::Type::Bytes) {
            if (!r.bytes(name))
                return false;
        } else if (!r.skip(f))
            return false;
    }
    return name.find("777") != std::string_view::npos;
}
static std::string key_of(uint64_t id) {
    char b[24];
    std::snprintf(b, sizeof b, "p/%08llu", (unsigned long long)id);
    return b;
}
// Median of 7 runs after 2 warmups, in nanoseconds per row.
template <class F> static void measure(const char *name, size_t rows, F run) {
    for (int i = 0; i < 2; ++i)
        sink += run();
    std::vector<double> samples;
    for (int i = 0; i < 7; ++i) {
        auto t0 = Clock::now();
        sink += run();
        samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() /
                          double(rows));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("%-34s %8.1f ns/row\n", name, samples[3]);
    std::fflush(stdout);
}

#ifdef FQDB_BENCH_SQLITE
struct Stmt {
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(sql);
    }
    ~Stmt() {
        sqlite3_finalize(s);
    }
};
#endif

int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::runtime_error("usage: fqdb_store_attribution_bench NEW_DIRECTORY [rows] [pad]");
        const uint64_t rows = argc > 2 ? std::stoull(argv[2]) : 50000;
        const size_t pad = argc > 3 ? std::stoull(argv[3]) : 256;
        auto root = std::filesystem::u8path(argv[1]);
        if (!std::filesystem::create_directory(root))
            throw std::runtime_error("directory must be new");
        std::vector<std::string> values(rows);
        for (uint64_t i = 0; i < rows; ++i)
            values[i] = make_value(i, pad);
        std::vector<uint64_t> order(rows);
        for (uint64_t i = 0; i < rows; ++i)
            order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937_64(7));
        std::printf("rows=%llu value_bytes=%zu\n", (unsigned long long)rows, values[0].size());

        // Floor: a contiguous in-memory array, no storage layer at all.
        measure("memory: scan, no parse", rows, [&] {
            uint64_t s = 0;
            for (auto &v : values)
                s += v.size();
            return s;
        });
        measure("memory: scan + parse field", rows, [&] {
            uint64_t s = 0;
            for (auto &v : values)
                s += parse_gold(v);
            return s;
        });
        measure("memory: random access + parse", rows, [&] {
            uint64_t s = 0;
            for (auto i : order)
                s += parse_gold(values[i]);
            return s;
        });

        fq::StoreOptions options;
        options.durability = fq::Durability::buffered;
        options.cache_bytes = 256ull << 20;
        options.pinned_bytes = 256ull << 20;
        auto db = fq::Store::open_at(root.u8string(), "main", options);
        for (uint64_t base = 0; base < rows; base += 1000) {
            auto tx = db.transaction();
            for (uint64_t i = base; i < std::min(rows, base + 1000); ++i)
                tx.put(key_of(i), values[i]);
            if (!tx.commit())
                throw std::runtime_error("load commit");
        }
        db.checkpoint();
        std::vector<std::string> keys(rows);
        for (uint64_t i = 0; i < rows; ++i)
            keys[i] = key_of(i);

        measure("fengqudb: scan, no parse", rows, [&] {
            auto snapshot = db.snapshot();
            auto cursor = snapshot.scan("p/", "p0");
            uint64_t s = 0;
            while (auto row = cursor.next())
                s += row->value.size();
            return s;
        });
        measure("fengqudb: scan + parse field", rows, [&] {
            auto snapshot = db.snapshot();
            auto cursor = snapshot.scan("p/", "p0");
            uint64_t s = 0;
            while (auto row = cursor.next())
                s += parse_gold(row->value);
            return s;
        });
        measure("fengqudb: random get + parse", rows, [&] {
            auto snapshot = db.snapshot();
            uint64_t s = 0;
            for (auto i : order) {
                auto value = snapshot.get(keys[i]);
                s += parse_gold(value->view());
            }
            return s;
        });
        measure("fengqudb: random get_many(64)", rows, [&] {
            auto snapshot = db.snapshot();
            uint64_t s = 0;
            for (size_t at = 0; at < order.size(); at += 64) {
                std::vector<std::string_view> batch;
                for (size_t k = at; k < std::min(order.size(), at + 64); ++k)
                    batch.push_back(keys[order[k]]);
                auto got = snapshot.get_many(batch);
                for (uint32_t k = 0; k < batch.size(); ++k)
                    if (auto v = got[k].value)
                        s += parse_gold(*v);
            }
            return s;
        });

        measure("memory: scan + contains(name)", rows, [&] {
            uint64_t s = 0;
            for (auto &v : values)
                s += contains_name(v);
            return s;
        });
        measure("fengqudb: scan + contains(name)", rows, [&] {
            auto snapshot = db.snapshot();
            auto cursor = snapshot.scan("p/", "p0");
            uint64_t s = 0;
            while (auto row = cursor.next())
                s += contains_name(row->value);
            return s;
        });
        // The same contains-filter through the query engine (index-free full scan), so the cost of
        // the executor on top of the raw cursor is measured in one program on identical rows.
        {
            fq::StoreQuery q;
            q.prefix = "p/";
            q.where = fq::QueryExpr::compare(fq::QueryField::field(5, 4), fq::QueryOp::Contains,
                                             fq::QueryValue::text("777"));
            fq::QueryColumn column;
            column.field = fq::QueryField::field(1, 1);
            q.columns = {column};
            q.limit = 4096;
            measure("fengqudb: query engine, contains", rows, [&] {
                return uint64_t(fq::select(db, q).rows.size());
            });
        }

#ifdef FQDB_BENCH_SQLITE
        sqlite3 *sql = nullptr;
        if (sqlite3_open((root / "sqlite.db").u8string().c_str(), &sql) != SQLITE_OK)
            throw std::runtime_error("sqlite open");
        sqlite3_exec(sql,
                     "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF; PRAGMA cache_size=-262144;"
                     "CREATE TABLE t(k TEXT PRIMARY KEY, v BLOB NOT NULL) WITHOUT ROWID;"
                     "CREATE TABLE r(id INTEGER PRIMARY KEY, v BLOB NOT NULL); BEGIN;",
                     nullptr, nullptr, nullptr);
        {
            Stmt a(sql, "INSERT INTO t VALUES(?1,?2)"), b(sql, "INSERT INTO r VALUES(?1,?2)");
            for (uint64_t i = 0; i < rows; ++i) {
                sqlite3_bind_text(a.s, 1, keys[i].data(), int(keys[i].size()), SQLITE_STATIC);
                sqlite3_bind_blob(a.s, 2, values[i].data(), int(values[i].size()), SQLITE_STATIC);
                sqlite3_step(a.s);
                sqlite3_reset(a.s);
                sqlite3_bind_int64(b.s, 1, int64_t(i));
                sqlite3_bind_blob(b.s, 2, values[i].data(), int(values[i].size()), SQLITE_STATIC);
                sqlite3_step(b.s);
                sqlite3_reset(b.s);
            }
        }
        sqlite3_exec(sql, "COMMIT;", nullptr, nullptr, nullptr);
        Stmt scan(sql, "SELECT k, v FROM t WHERE k >= 'p/' AND k < 'p0'");
        Stmt scan_rowid(sql, "SELECT v FROM r");
        Stmt point(sql, "SELECT v FROM t WHERE k = ?1"), point_rowid(sql, "SELECT v FROM r WHERE id = ?1");
        auto view = [](sqlite3_stmt *s, int col) {
            return std::string_view(static_cast<const char *>(sqlite3_column_blob(s, col)),
                                    size_t(sqlite3_column_bytes(s, col)));
        };
        measure("sqlite(text pk): scan, no parse", rows, [&] {
            uint64_t s = 0;
            while (sqlite3_step(scan.s) == SQLITE_ROW)
                s += view(scan.s, 1).size();
            sqlite3_reset(scan.s);
            return s;
        });
        measure("sqlite(text pk): scan + parse", rows, [&] {
            uint64_t s = 0;
            while (sqlite3_step(scan.s) == SQLITE_ROW)
                s += parse_gold(view(scan.s, 1));
            sqlite3_reset(scan.s);
            return s;
        });
        measure("sqlite(rowid): scan + parse", rows, [&] {
            uint64_t s = 0;
            while (sqlite3_step(scan_rowid.s) == SQLITE_ROW)
                s += parse_gold(view(scan_rowid.s, 0));
            sqlite3_reset(scan_rowid.s);
            return s;
        });
        measure("sqlite(text pk): random get+parse", rows, [&] {
            uint64_t s = 0;
            for (auto i : order) {
                sqlite3_bind_text(point.s, 1, keys[i].data(), int(keys[i].size()), SQLITE_STATIC);
                if (sqlite3_step(point.s) == SQLITE_ROW)
                    s += parse_gold(view(point.s, 0));
                sqlite3_reset(point.s);
            }
            return s;
        });
        measure("sqlite(rowid): random get+parse", rows, [&] {
            uint64_t s = 0;
            for (auto i : order) {
                sqlite3_bind_int64(point_rowid.s, 1, int64_t(i));
                if (sqlite3_step(point_rowid.s) == SQLITE_ROW)
                    s += parse_gold(view(point_rowid.s, 0));
                sqlite3_reset(point_rowid.s);
            }
            return s;
        });
        // Column layout of the same rows. A query that never reads the padding column does not
        // touch its bytes in SQLite; this is the case of the contains scan in the query benchmark.
        sqlite3_exec(sql,
                     "CREATE TABLE m(id INTEGER PRIMARY KEY, name TEXT NOT NULL, pad BLOB NOT NULL);"
                     "BEGIN;",
                     nullptr, nullptr, nullptr);
        {
            Stmt ins(sql, "INSERT INTO m VALUES(?1,?2,?3)");
            const std::string padding(pad, 'x');
            for (uint64_t i = 0; i < rows; ++i) {
                auto name = "Name-" + std::to_string(i);
                sqlite3_bind_int64(ins.s, 1, int64_t(i));
                sqlite3_bind_text(ins.s, 2, name.data(), int(name.size()), SQLITE_TRANSIENT);
                sqlite3_bind_blob(ins.s, 3, padding.data(), int(padding.size()), SQLITE_STATIC);
                sqlite3_step(ins.s);
                sqlite3_reset(ins.s);
            }
        }
        sqlite3_exec(sql, "COMMIT;", nullptr, nullptr, nullptr);
        Stmt name_only(sql, "SELECT name FROM m"), name_pad(sql, "SELECT name, pad FROM m");
        measure("sqlite(columns): contains, name only", rows, [&] {
            uint64_t s = 0;
            while (sqlite3_step(name_only.s) == SQLITE_ROW)
                s += view(name_only.s, 0).find("777") != std::string_view::npos;
            sqlite3_reset(name_only.s);
            return s;
        });
        measure("sqlite(columns): contains, +pad read", rows, [&] {
            uint64_t s = 0;
            while (sqlite3_step(name_pad.s) == SQLITE_ROW) {
                s += view(name_pad.s, 0).find("777") != std::string_view::npos;
                s += static_cast<unsigned char>(view(name_pad.s, 1)[0]);
            }
            sqlite3_reset(name_pad.s);
            return s;
        });
        sqlite3_finalize(name_only.s);
        name_only.s = nullptr;
        sqlite3_finalize(name_pad.s);
        name_pad.s = nullptr;
        sqlite3_finalize(scan.s);
        scan.s = nullptr;
        sqlite3_finalize(scan_rowid.s);
        scan_rowid.s = nullptr;
        sqlite3_finalize(point.s);
        point.s = nullptr;
        sqlite3_finalize(point_rowid.s);
        point_rowid.s = nullptr;
        sqlite3_close(sql);
#endif
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
