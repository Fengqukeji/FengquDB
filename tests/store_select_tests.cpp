#include "store_test_support.hpp"
#include "fengqudb_select.hpp"
#include "paged/select.hpp"
#include <random>
using fengqudb::explain;
using fengqudb::QueryAggregate;
using fengqudb::QueryExpr;
using fengqudb::QueryField;
using fengqudb::QueryJoin;
using fengqudb::QueryJoinKind;
using fengqudb::QueryKind;
using fengqudb::QueryOp;
using fengqudb::QueryValue;
using fengqudb::select;
using fengqudb::Status;
using fengqudb::StoreCommand;
using fengqudb::StoreQuery;
using fengqudb::StoreQueryResult;
namespace wire = fengqudb::wire;
using F = QueryField;
using E = QueryExpr;
using V = QueryValue;
using A = QueryAggregate;
static std::string doc(unsigned id, bool missing = false) {
    wire::Writer w;
    w.uint_field(1, id);
    w.bytes_field(2, id % 2 ? "mage" : "warrior");
    w.uint_field(3, id % 20);
    w.sint_field(4, int64_t(id) * 11 - 100);
    w.bytes_field(5, "Name-" + std::to_string(id));
    w.bytes_field(6, std::string(4096, 'p'));
    w.uint_field(11, id % 2);
    if (!missing)
        w.uint_field(10, id % 3);
    wire::Writer nested;
    nested.uint_field(1, id + 1);
    w.bytes_field(8, nested.data());
    return w.take();
}
static StoreQuery query() {
    StoreQuery q;
    q.prefix = "p/";
    q.columns = {{F::field(1, 1), A::None, {}}};
    return q;
}
static std::vector<uint64_t> ids(const StoreQueryResult &r) {
    std::vector<uint64_t> out;
    for (auto &row : r.rows)
        out.push_back(row.values[0].unsigned_value);
    return out;
}
static void seed(Store &db, unsigned count = 64) {
    auto tx = db.transaction();
    for (unsigned i = 0; i < count; ++i)
        tx.put("p/" + std::to_string(i), doc(i, i % 4 == 0));
    commit(tx);
    db.define_compound_index("class-level", "p/", {{2, 4}, {3, 1}}, 2);
    db.define_compound_index("id", "p/", {{1, 1}}, 3);
    db.checkpoint();
}
static void conditions_order_and_covering() {
    Fixture f;
    Store db(f.name("conditions"));
    seed(db);
    auto q = query();
    q.where = E::all({E::compare(F::field(2, 4), QueryOp::Eq, V::text("mage")),
                      E::compare(F::field(3, 1), QueryOp::Ge, V::uint(5)),
                      E::compare(F::field(3, 1), QueryOp::Le, V::uint(15))});
    q.order = {{F::field(3, 1), true, true}};
    q.limit = 7;
    auto r = select(db, q);
    check(r.rows.size() == 7 && r.rows[0].values[0].unsigned_value == 55, "indexed range/top N");
    q.columns = {{F::field(2, 4), A::None, {}}, {F::field(3, 1), A::None, {}}};
    r = select(db, q);
    check(r.stats.payload_reads == 0 && r.access[0].covering,
          "covering query read business payload");
    q = query();
    q.where = E::one_of(F::field(1, 1), {V::uint(3), V::uint(3), V::integer(3), V::uint(8)});
    q.order = {{F::field(1, 1)}};
    check(ids(select(db, q)) == std::vector<uint64_t>({3, 8}),
          "IN duplicate/type-normalized value returned twice");
    q.where = E::any({E::compare(F::field(1, 1), QueryOp::Lt, V::uint(3)),
                      E::compare(F::field(1, 1), QueryOp::Le, V::uint(5))});
    check(ids(select(db, q)) == std::vector<uint64_t>({0, 1, 2, 3, 4, 5}),
          "overlapping OR returned duplicates");
    q = query();
    q.where = E::negate(E::one_of(F::field(10, 1), {}));
    check(select(db, q).rows.size() == 64, "NOT IN empty lost missing fields");
    q.where = E::negate(E::compare(F::field(10, 1), QueryOp::Eq, V::uint(1)));
    for (auto id : ids(select(db, q)))
        check(id % 4 && id % 3 != 1, "missing-field three-valued NOT");
    q = query();
    q.columns = {{F{0, 1, {8, 1}}, A::None, {}}};
    q.limit = 1;
    check(select(db, q).rows[0].values[0].unsigned_value == 1, "nested protobuf path");
    q = query();
    q.where = E::compare(F::key(), QueryOp::Eq, V::text("p/3"));
    q.allow_scan = false;
    r = select(db, q);
    check(ids(r) == std::vector<uint64_t>{3} && r.stats.candidates == 1 &&
              r.access[0].strategy == "primary_key",
          "primary key query scanned prefix");
    db.define_compound_index("name", "p/", {{5, 4}}, 2);
    q = query();
    q.where = E::compare(F::field(5, 4), QueryOp::Like, V::text("Name-1%"));
    r = select(db, q);
    check(r.rows.size() == 11 && !r.access[0].needs_scan, "LIKE prefix not bounded by index");
    q = query();
    q.order = {{F::field(1, 1), true, true}};
    q.limit = 4;
    r = select(db, q);
    check(ids(r) == std::vector<uint64_t>({63, 62, 61, 60}) && r.stats.candidates <= 5 &&
              !r.stats.complete,
          "ordered index did not stop at Top N");
    q = query();
    q.where = E::compare(F::field(4, 5), QueryOp::Gt, V::integer(0));
    q.allow_scan = false;
    check(explain(db, q).access[0].needs_scan, "explain hid prohibited scan");
    check(status([&] { select(db, q); }) == FQDB_UNSUPPORTED, "forbidden scan executed");
    db.define_compound_index("bool", "p/", {{11, 11}}, 2);
    q = query();
    q.where = E::compare(F::field(11, 11), QueryOp::Eq, V::boolean(true));
    q.columns = {{F::field(11, 11), A::None, {}}};
    r = select(db, q);
    check(r.rows.size() == 32 && r.stats.payload_reads == 0 &&
              r.rows[0].values[0].kind == QueryKind::Bool,
          "boolean covering index");
    q = query();
    q.limit = 5;
    auto first = select(db, q);
    q.after = first.cursor;
    auto page = select(db, q);
    check(page.rows.size() == 5 && page.stats.candidates <= 7,
          "keyset page rescanned prior prefix");
}
static void sparse_indices_paging_and_conflicts() {
    Fixture f;
    Store db(f.name("sparse"));
    seed(db);
    db.define_compound_index("optional", "p/", {{2, 4}, {10, 1}}, 0);
    auto q = query();
    q.where = E::compare(F::field(2, 4), QueryOp::Eq, V::text("warrior"));
    q.index_hint = "optional";
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT,
          "sparse index silently omitted missing values");
    q.index_hint.clear();
    check(select(db, q).rows.size() == 32, "auto planner omitted missing optional fields");
    q = query();
    q.limit = 9;
    q.order = {{F::field(4, 5), true, true}, {F::field(1, 1), false, true}};
    std::vector<uint64_t> found;
    for (;;) {
        auto page = select(db, q);
        auto values = ids(page);
        found.insert(found.end(), values.begin(), values.end());
        if (page.cursor.empty())
            break;
        q.after = page.cursor;
    }
    std::vector<uint64_t> expected;
    for (unsigned i = 64; i > 0; --i)
        expected.push_back(i - 1);
    check(found == expected, "independent sort cursor duplicated/lost records");
    q.prefix = "other/";
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT,
          "cursor accepted different query");
    q = query();
    q.where = E::compare(F::field(1, 1), QueryOp::Eq, V::uint(3));
    q.columns.push_back({F::field(5, 4), A::None, {}});
    auto tx = db.transaction();
    select(tx, q);
    auto bytes = doc(3);
    auto needle = std::string("Name-3");
    bytes.replace(bytes.find(needle), needle.size(), "Name-X");
    put(db, "p/3", bytes);
    tx.put("derived", "stale");
    check(tx.commit().status == Status::conflict, "query lost non-indexed field dependency");
    auto private_tx = db.transaction();
    auto mark = private_tx.savepoint();
    private_tx.put("p/3", doc(99));
    q = query();
    q.where = E::compare(F::field(1, 1), QueryOp::Eq, V::uint(99));
    check(ids(select(private_tx, q)) == std::vector<uint64_t>{99},
          "private index overlay not visible");
    private_tx.rollback_to(mark);
    check(select(private_tx, q).rows.empty(), "query overlay leaked after rollback");
}
static void groups_numeric_and_nulls() {
    Fixture f;
    Store db(f.name("groups"));
    seed(db);
    auto q = query();
    auto type = F::field(2, 4), gold = F::field(4, 5);
    q.columns = {{type, A::None, {}}, {gold, A::CountAll, {}}, {gold, A::Sum, {}},
                 {gold, A::Min, {}},  {gold, A::Max, {}},      {gold, A::Avg, {}}};
    q.group = {type};
    q.order = {{F::column(0)}};
    q.having = E::compare(F::column(1), QueryOp::Gt, V::uint(30));
    auto r = select(db, q);
    check(r.rows.size() == 2, "GROUP BY/HAVING");
    for (auto &row : r.rows) {
        bool mage = row.values[0].bytes == "mage";
        int64_t total = 0;
        for (unsigned i = 0; i < 64; ++i)
            if (bool(i % 2) == mage)
                total += int64_t(i) * 11 - 100;
        check(row.values[1].unsigned_value == 32 && row.values[2].signed_value == total &&
                  std::abs(row.values[5].real_value - double(total) / 32) < 1e-9,
              "aggregate value");
    }
    q = query();
    q.where.op = QueryOp::False;
    q.columns = {{gold, A::CountAll, {}}, {gold, A::Sum, {}}, {gold, A::Avg, {}}};
    r = select(db, q);
    check(r.rows.size() == 1 && r.rows[0].values[0].unsigned_value == 0 &&
              r.rows[0].values[1].kind == QueryKind::Null &&
              r.rows[0].values[2].kind == QueryKind::Null,
          "empty global aggregate");
    q = query();
    q.columns = {{F::field(10, 1), A::Count, {}, true}};
    r = select(db, q);
    check(r.rows[0].values[0].unsigned_value == 3, "COUNT DISTINCT including null");
    q = query();
    q.columns = {{type, A::None, {}}};
    q.distinct = true;
    q.limit = 1;
    auto first = select(db, q);
    check(first.rows.size() == 1 && !first.cursor.empty(), "DISTINCT first page");
    q.after = first.cursor;
    auto second = select(db, q);
    check(second.rows.size() == 1 &&
              first.rows[0].values[0].bytes != second.rows[0].values[0].bytes &&
              second.cursor.empty(),
          "DISTINCT cursor duplicated logical row");
    wire::Writer v;
    v.uint_field(1, UINT64_MAX);
    db.put_if_changed("n/max", v.data());
    v.clear();
    v.uint_field(1, UINT64_MAX - 1);
    db.put_if_changed("n/previous", v.data());
    q = query();
    q.prefix = "n/";
    q.where = E::compare(F::field(1, 1), QueryOp::Eq, V::uint(UINT64_MAX));
    check(select(db, q).rows.size() == 1, "uint64 equality lost precision");
    q.where = E::compare(F::field(1, 1), QueryOp::Lt, V::real(std::ldexp(1.0, 64)));
    check(select(db, q).rows.size() == 2, "uint64/double boundary");
    q.where = {};
    q.columns = {{F::field(1, 1), A::Sum, {}}};
    check(status([&] { select(db, q); }) == FQDB_CONSTRAINT, "SUM overflow silently wrapped");
}
static void joins_and_patterns() {
    Fixture f;
    Store db(f.name("joins"));
    seed(db, 16);
    auto tx = db.transaction();
    for (unsigned i = 0; i < 8; ++i) {
        wire::Writer w;
        w.uint_field(1, i % 4);
        w.uint_field(2, i);
        tx.put("o/" + std::to_string(i), w.data());
    }
    commit(tx);
    QueryJoin j;
    j.prefix = "o/";
    j.left = {F::field(1, 1)};
    j.right = {F::field(1, 1, 1)};
    auto q = query();
    q.joins = {j};
    q.columns.push_back({F::field(2, 1, 1), A::None, {}});
    auto hash = select(db, q);
    check(hash.rows.size() == 8 && hash.access.back().strategy == "hash_join", "bounded hash join");
    db.define_compound_index("owner", "o/", {{1, 1}}, 2);
    auto indexed = select(db, q);
    check(indexed.rows.size() == 8 && indexed.access.back().strategy == "index_join",
          "index nested-loop join");
    q.joins[0].kind = QueryJoinKind::Left;
    check(select(db, q).rows.size() == 20, "left outer join row count");
    q.where = E::test(F::field(2, 1, 1), QueryOp::IsNull);
    check(select(db, q).rows.size() == 12, "left join null filter");
    q.where = {};
    q.columns.resize(1);
    q.joins[0].kind = QueryJoinKind::Semi;
    check(select(db, q).rows.size() == 4, "semi join");
    q.joins[0].kind = QueryJoinKind::Anti;
    check(select(db, q).rows.size() == 12, "anti join");
    q = query();
    auto name = F::field(5, 4);
    q.where = E::compare(name, QueryOp::Regex, V::text("^Name-(?:[0-9]|1[0-2])$"));
    check(select(db, q).rows.size() == 13, "bounded regex alternation/group");
    wire::Writer v;
    v.bytes_field(5, u8"张三");
    v.uint_field(1, 99);
    db.put_if_changed("u/name", v.data());
    q.prefix = "u/";
    q.where = E::compare(name, QueryOp::Like, V::text(u8"张_"));
    check(select(db, q).rows.size() == 1, "LIKE underscore split UTF-8 character");
    q.where = E::compare(name, QueryOp::Fuzzy, V::text(u8"张四"));
    q.where.values.push_back(V::uint(1));
    check(select(db, q).rows.size() == 1, "UTF-8 fuzzy edit distance");
    v.clear();
    v.bytes_field(5, std::string(20000, 'a') + "!");
    v.uint_field(1, 1);
    db.put_if_changed("r/adversarial", v.data());
    q.prefix = "r/";
    q.where = E::compare(name, QueryOp::Regex, V::text("(a+)+$"));
    q.max_work = 1000000;
    check(select(db, q).rows.empty(), "regex adversarial match");
    q.max_work = 32;
    check(status([&] { select(db, q); }) == FQDB_LIMIT, "regex ignored work budget");
    q.max_work = 1000000;
    q.where = E::compare(name, QueryOp::Regex, V::text("(a)\\1"));
    check(status([&] { select(db, q); }) == FQDB_UNSUPPORTED,
          "backreference accepted by linear matcher");
    v.clear();
    v.bytes_field(1, "p/3");
    db.put_if_changed("keyjoin/1", v.data());
    q = query();
    q.prefix = "keyjoin/";
    q.columns = {{F::field(1, 1, 1), A::None, {}}};
    QueryJoin direct;
    direct.prefix = "p/";
    direct.left = {F::field(1, 4)};
    direct.right = {F::key(1)};
    q.joins = {direct};
    auto primary = select(db, q);
    check(ids(primary) == std::vector<uint64_t>{3} &&
              primary.access.back().strategy == "primary_key_join",
          "primary-key join");
    q.joins[0].right = {F::field(1, 1, 1)};
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT,
          "incompatible join types accepted");
}
static void lifecycle_and_concurrency() {
    Fixture f;
    auto path = f.name("lifecycle");
    {
        Store db(path);
        seed(db);
        auto q = query();
        q.where = E::compare(F::field(1, 1), QueryOp::Eq, V::uint(3));
        q.columns = {{F::field(1, 1), A::CountAll, {}}};
        auto run = [&] {
            for (unsigned i = 0; i < 32; ++i)
                check(select(db, q).rows[0].values[0].unsigned_value == 1,
                      "concurrent cached query");
        };
        std::vector<std::future<void>> jobs;
        for (unsigned i = 0; i < 4; ++i)
            jobs.push_back(std::async(std::launch::async, run));
        for (auto &job : jobs)
            job.get();
        db.verify();
    }
    {
        auto core = std::make_shared<p::Core>(path, p::defaults());
        core->open();
        auto before = core->pin_budget->used();
        for (unsigned n = 0; n < 5; ++n) {
            p::Transaction tx(core, 0);
            auto q = query();
            if (n == 1)
                q.max_candidates = 1;
            if (n == 2)
                q.max_work = 1;
            if (n == 3)
                q.memory_bytes = 1048576;
            if (n == 4) {
                q.memory_bytes = 1048576;
                q.where.op = QueryOp::And;
                for (unsigned i = 0; i < 16; ++i)
                    q.where.children.push_back(
                        E::compare(F::field(5, 4), QueryOp::Like, V::text(std::string(32, '_'))));
            }
            auto body = q.encode();
            auto wire = StoreCommand{FQDB_SELECT, {}, {}, body}.encode();
            try {
                p::Charge retained;
                auto result = p::select_query(tx, p::FeatureRequest(wire), retained);
                check(n == 0 || n == 3,
                      ("failing query unexpectedly succeeded: " + std::to_string(n)).c_str());
            } catch (const p::Error &e) {
                check((n == 1 || n == 2 || n == 4) && e.status == FQDB_LIMIT,
                      "wrong query failure");
            }
        }
        std::atomic_store_explicit(&core->select_plan_cache,
                                   std::shared_ptr<const p::select::PlanCache>{},
                                   std::memory_order_release);
        std::atomic_store_explicit(&core->select_program_cache,
                                   std::shared_ptr<const p::select::Program>{},
                                   std::memory_order_release);
        check(core->pin_budget->used() == before && core->transaction_budget->used() == 0,
              "query leaked budget or program lease");
    }
}
static void cache_schema_and_metadata_only() {
    Fixture f;
    Store db(f.name("plan-schema"));
    seed(db);
    auto q = query();
    q.where = E::compare(F::field(3, 1), QueryOp::Ge, V::uint(10));
    q.columns = {
        {F::field(3, 1), A::Sum, {}}, {F::field(3, 1), A::Avg, {}}, {F::field(3, 1), A::Count, {}}};
    db.define_compound_index("level", "p/", {{3, 1}}, 2);
    auto covered = select(db, q);
    check(covered.stats.payload_reads == 0 && covered.access[0].covering,
          "covering aggregate read business payload");
    db.drop_index("level");
    auto scanned = select(db, q);
    check(scanned.access[0].needs_scan &&
              scanned.rows[0].values[0].unsigned_value == covered.rows[0].values[0].unsigned_value,
          "cached plan survived schema drop");
    db.define_compound_index("level", "p/", {{3, 1}}, 2);
    check(select(db, q).stats.payload_reads == 0, "cached scan survived schema redefinition");

    fengqudb::StoreOptions options;
    options.cache_bytes = 0;
    options.pinned_bytes = 65536;
    Store large(f.name("metadata"), options);
    put(large, "big/1", std::string(131072, 'x'));
    large.checkpoint();
    q = {};
    q.prefix = "big/";
    q.columns = {{F::key(), A::None, {}}};
    auto r = select(large, q);
    check(r.rows.size() == 1 && r.rows[0].values[0].bytes == "big/1" && r.stats.payload_reads == 0,
          "metadata query pinned the complete value");
    q.columns = {{F::record(), A::None, {}}};
    check(status([&] { select(large, q); }) == FQDB_LIMIT,
          "payload query bypassed tiny pin budget");
}
// The Store-level snapshot path must match the transaction path and reject non-query ops.
static void store_snapshot_path() {
    Fixture f;
    Store db(f.name("snapshot-path"));
    seed(db);
    auto q = query();
    q.where = E::all({E::compare(F::field(2, 4), QueryOp::Eq, V::text("mage")),
                      E::compare(F::field(3, 1), QueryOp::Ge, V::uint(5))});
    q.columns.push_back({F::field(4, 5), A::None, {}});
    q.order = {{F::field(4, 5), true, true}, {F::field(1, 1), false, true}};
    auto tx = db.transaction();
    auto in_tx = select(tx, q);
    auto direct = select(db, q);
    check(!direct.rows.empty() && ids(direct) == ids(in_tx) &&
              direct.rows[0].values[1].signed_value == in_tx.rows[0].values[1].signed_value,
          "store snapshot select differs from transaction select");
    check(direct.stats.payload_reads == in_tx.stats.payload_reads,
          "snapshot path changed payload reads");
    auto prepared = fengqudb::StorePreparedQuery(q);
    check(ids(prepared.execute(db)) == ids(direct), "prepared store select differs");
    check(!explain(db, q).access.empty(), "store explain lost access plan");
    // Private writes are visible only through the transaction path.
    tx.put("p/1000", doc(1001));
    auto all = query();
    all.limit = 4096;
    check(select(tx, all).rows.size() == 65 && select(db, all).rows.size() == 64,
          "private write visibility differs between paths");
    // The snapshot path records no dependencies; the transaction path still conflicts.
    put(db, "p/1", doc(1));
    check(select(db, all).rows.size() == 64, "snapshot select after write");
    check(tx.commit().status == Status::conflict, "transaction query lost dependency");
    fengqudb::StoreCommand bad{FQDB_COLLECTION_SIZE, "h"};
    check(status([&] { db.select_command(bad); }) == FQDB_INVALID_ARGUMENT,
          "store select accepted a non-query operation");
    fqdb_buffer out{reinterpret_cast<void *>(1), 1};
    check(fqdb_store_select(nullptr, {nullptr, 0}, &out) == FQDB_INVALID_ARGUMENT &&
              out.data == nullptr && out.size == 0,
          "null store select");
    check(fqdb_store_select(nullptr, {nullptr, 0}, nullptr) == FQDB_INVALID_ARGUMENT,
          "null output select");
}
// 窄索引过滤扫描：结果必须等于按主记录逐行判断的模型，且索引更新和私有写入后仍然正确。
static void filter_scan_matches_model() {
    Fixture f;
    Store db(f.name("filter-scan"));
    seed(db);
    db.define_compound_index("name", "p/", {{5, 4}}, 2);
    db.checkpoint();
    auto q = query();
    q.where = E::compare(F::field(5, 4), QueryOp::Contains, V::text("1"));
    q.limit = 4096;
    auto sorted_ids = [](const StoreQueryResult &r) {
        auto out = ids(r);
        std::sort(out.begin(), out.end());
        return out;
    };
    auto model = [](unsigned replaced_key, unsigned replaced_id) {
        std::vector<uint64_t> out;
        for (unsigned key = 0; key < 64; ++key) {
            unsigned id = key == replaced_key ? replaced_id : key;
            if (std::to_string(id).find('1') != std::string::npos)
                out.push_back(id);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto r = select(db, q);
    check(r.access[0].strategy == "index_filter_scan", "filter scan was not chosen");
    check(sorted_ids(r) == model(999, 0), "filter scan result differs from model");
    // 索引随更新维护：改写一行的名称后，扫描结果必须同步变化。
    put(db, "p/2", doc(131));
    r = select(db, q);
    check(r.access[0].strategy == "index_filter_scan" && sorted_ids(r) == model(2, 131),
          "filter scan missed an indexed update");
    // 事务内有私有写入时索引尚未反映，必须改走主记录扫描并看到私有写入。
    auto tx = db.transaction();
    tx.put("p/3", doc(141));
    auto in_tx = select(tx, q);
    check(in_tx.access[0].strategy != "index_filter_scan", "filter scan used with private writes");
    auto expected = model(2, 131);
    expected.push_back(141);
    std::sort(expected.begin(), expected.end());
    check(sorted_ids(in_tx) == expected, "private write invisible to filter scan fallback");
    // 小结果且无排序时仍用主键序扫描，保留凑满即停。
    auto small = query();
    small.where = q.where;
    small.limit = 5;
    check(select(db, small).access[0].strategy == "prefix_scan", "small limit lost early stop");
}
// 规则缓存跨 Program 复用（换字面量的查询）时，目录变更后必须立即看到新的索引集合。
static void rules_reuse_respects_catalog_epoch() {
    Fixture f;
    Store db(f.name("rules-reuse"));
    seed(db);
    auto at = [&](unsigned level) {
        auto q = query();
        q.where = E::compare(F::field(3, 1), QueryOp::Ge, V::uint(level));
        return select(db, q);
    };
    auto first = at(10);
    check(first.access[0].strategy != "index_range", "unexpected index before definition");
    check(at(11).access[0].strategy != "index_range", "reused rules changed the plan");
    db.define_compound_index("level", "p/", {{3, 1}}, 2);
    auto indexed = at(12);
    check(indexed.access[0].strategy == "index_range" && indexed.access[0].index == "level",
          "reused rules missed a newly defined index");
    check(at(13).access[0].index == "level", "second query after define lost the index");
    db.drop_index("level");
    check(at(14).access[0].strategy != "index_range", "reused rules kept a dropped index");
    // 不同字面量的结果必须与按主记录逐行判断的模型一致。
    for (unsigned level : {0u, 7u, 15u, 19u, 20u}) {
        std::vector<uint64_t> expected;
        for (unsigned i = 0; i < 64; ++i)
            if (i % 20 >= level)
                expected.push_back(i);
        auto got = ids(at(level));
        std::sort(got.begin(), got.end());
        check(got == expected, "literal-varying query result differs from model");
    }
}
// LIKE 的字面量前缀快路径必须与通用 NFA 结果一致。
static void like_prefix_fast_path_matches_nfa() {
    Fixture f;
    Store db(f.name("like-prefix"));
    seed(db);
    auto run = [&](const char *pattern) {
        auto q = query();
        q.where = E::compare(F::field(5, 4), QueryOp::Like, V::text(pattern));
        q.limit = 4096;
        auto got = ids(select(db, q));
        std::sort(got.begin(), got.end());
        return got;
    };
    auto starting = [](char digit, size_t min_digits) {
        std::vector<uint64_t> out;
        for (unsigned i = 0; i < 64; ++i) {
            auto text = std::to_string(i);
            if (text[0] == digit && text.size() >= min_digits)
                out.push_back(i);
        }
        return out;
    };
    std::vector<uint64_t> all;
    for (unsigned i = 0; i < 64; ++i)
        all.push_back(i);
    check(run("Name-1%") == starting('1', 1), "like prefix fast path");
    check(run("Name-6%") == starting('6', 1), "like prefix fast path 6");
    check(run("Name-1_%") == starting('1', 2), "like with underscore uses the pattern engine");
    check(run("%") == all && run("Name-%") == all, "like match-all prefix");
    check(run("Name-1\\%%").empty(), "escaped percent must stay literal");
    check(run("Name-1") == std::vector<uint64_t>{1}, "like without wildcard must match exactly");
    check(run("name-1%").empty(), "like prefix must be case sensitive");
    check(status([&] { run("Name-1\\"); }) == FQDB_INVALID_ARGUMENT, "dangling escape accepted");
}
// 非唯一复合索引直接从键尾解主键：主键含零字节（需转义）时结果仍必须正确。
static void index_scan_decodes_owner_from_key() {
    Fixture f;
    Store db(f.name("owner-from-key"));
    seed(db);
    db.define_compound_index("level", "p/", {{3, 1}}, 2);
    const std::string odd("p/z\0y\xff", 6);
    put(db, odd, doc(70));
    auto q = query();
    q.columns = {{F::key(), A::None, {}}, {F::field(1, 1), A::None, {}}};
    q.where = E::compare(F::field(3, 1), QueryOp::Ge, V::uint(10));
    q.limit = 4096;
    auto r = select(db, q);
    check(r.access[0].strategy == "index_range", "owner-from-key query did not use the index");
    bool found = false;
    size_t expected = 0;
    for (unsigned i = 0; i < 64; ++i)
        expected += (i % 20 >= 10);
    expected += (70 % 20 >= 10);
    for (auto &row : r.rows)
        if (row.values[0].bytes == odd) {
            found = true;
            check(row.values[1].unsigned_value == 70, "wrong row for key with zero byte");
        }
    check(found && r.rows.size() == expected, "index scan lost or duplicated rows");
    // 覆盖路径（只取索引字段与主键）同样从键尾取主键。
    q.columns = {{F::key(), A::None, {}}, {F::field(3, 1), A::None, {}}};
    auto covered = select(db, q);
    check(covered.access[0].covering && covered.stats.payload_reads == 0 &&
              covered.rows.size() == expected,
          "covering scan with key-tail owner differs");
}
// repeated 字段：条件在任一元素满足时为真；打包/未打包标量、嵌套消息子字段和字符串都按元素匹配。
static void repeated_fields_match_any_element() {
    Fixture f;
    Store db(f.name("repeated"));
    constexpr unsigned count = 64;
    auto unpacked = [](unsigned i) {
        return i % 4 == 0 ? std::vector<uint64_t>{}
                          : std::vector<uint64_t>{i % 7, (i * 3) % 11, 20 + i % 5};
    };
    auto packed = [](unsigned i) {
        return i % 5 == 0 ? std::vector<uint64_t>{} : std::vector<uint64_t>{i % 9, (i * 2) % 13};
    };
    auto messages = [](unsigned i) {
        std::vector<std::pair<uint64_t, uint64_t>> out;
        for (unsigned k = 0; k < i % 3; ++k)
            out.push_back({(i * 5 + k) % 17, (k + i) % 6});
        return out;
    };
    auto tags = [](unsigned i) {
        return i % 6 == 0 ? std::vector<std::string>{}
                          : std::vector<std::string>{"tag-" + std::to_string(i % 10),
                                                     "tag-" + std::to_string((i + 3) % 10)};
    };
    {
        auto tx = db.transaction();
        for (unsigned i = 0; i < count; ++i) {
            wire::Writer w;
            w.uint_field(1, i);
            for (auto v : unpacked(i))
                w.uint_field(12, v);
            if (!packed(i).empty()) {
                wire::Writer inner;
                for (auto v : packed(i))
                    inner.varint(v);
                w.bytes_field(13, inner.data());
            }
            for (auto &m : messages(i)) {
                wire::Writer inner;
                inner.uint_field(1, m.first);
                inner.uint_field(2, m.second);
                w.bytes_field(14, inner.data());
            }
            for (auto &t : tags(i))
                w.bytes_field(15, t);
            tx.put("p/" + std::to_string(i), w.take());
        }
        commit(tx);
    }
    auto rep = [](uint32_t number, uint32_t type) { return F::field(number, type).any(); };
    auto nested = [](uint32_t sub) {
        auto field = F::field(14, 1);
        field.path = {14, sub};
        return field.any();
    };
    auto run = [&](const E &where) {
        auto q = query();
        q.where = where;
        q.limit = 4096;
        auto got = ids(select(db, q));
        std::sort(got.begin(), got.end());
        return got;
    };
    auto model = [&](auto predicate) {
        std::vector<uint64_t> out;
        for (unsigned i = 0; i < count; ++i)
            if (predicate(i))
                out.push_back(i);
        return out;
    };
    auto has = [](const std::vector<uint64_t> &list, uint64_t v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    auto item_is = [&](unsigned i, uint64_t v) {
        for (auto &m : messages(i))
            if (m.first == v)
                return true;
        return false;
    };
    auto tag_is = [&](unsigned i, const std::string &v) {
        auto list = tags(i);
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    auto a = E::compare(rep(12, 1), QueryOp::Eq, V::uint(5));
    check(run(a) == model([&](unsigned i) { return has(unpacked(i), 5); }) && !run(a).empty(),
          "unpacked repeated equality");
    auto b = E::compare(rep(13, 1), QueryOp::Eq, V::uint(7));
    check(run(b) == model([&](unsigned i) { return has(packed(i), 7); }) && !run(b).empty(),
          "packed repeated equality");
    auto c = E::compare(nested(1), QueryOp::Eq, V::uint(3));
    check(run(c) == model([&](unsigned i) { return item_is(i, 3); }) && !run(c).empty(),
          "repeated nested message field");
    auto d = E::compare(rep(15, 4), QueryOp::Eq, V::text("tag-3"));
    check(run(d) == model([&](unsigned i) { return tag_is(i, "tag-3"); }) && !run(d).empty(),
          "repeated string equality");
    auto contains = E::compare(rep(15, 4), QueryOp::Contains, V::text("g-7"));
    check(run(contains) == model([&](unsigned i) { return tag_is(i, "tag-7"); }),
          "repeated string contains");
    check(run(E::compare(rep(12, 1), QueryOp::Ge, V::uint(20))) ==
              model([&](unsigned i) { return !unpacked(i).empty(); }),
          "repeated range comparison");
    check(run(E::negate(a)) == model([&](unsigned i) { return !has(unpacked(i), 5); }),
          "NOT over a repeated condition means no element matches");
    check(run(E::any({a, b})) ==
              model([&](unsigned i) { return has(unpacked(i), 5) || has(packed(i), 7); }),
          "OR of repeated conditions");
    check(run(E::all({a, E::compare(F::field(1, 1), QueryOp::Lt, V::uint(30))})) ==
              model([&](unsigned i) { return i < 30 && has(unpacked(i), 5); }),
          "repeated combined with a plain condition");
    check(run(E::one_of(rep(12, 1), {V::uint(1), V::uint(2)})) ==
              model([&](unsigned i) { return has(unpacked(i), 1) || has(unpacked(i), 2); }),
          "repeated IN");
    check(run(E::test(rep(12, 1), QueryOp::Exists)) ==
                  model([&](unsigned i) { return !unpacked(i).empty(); }) &&
              run(E::test(rep(12, 1), QueryOp::IsNull)) ==
                  model([&](unsigned i) { return unpacked(i).empty(); }),
          "repeated Exists / IsNull mean non-empty / empty");
    // 磁盘索引只记录标量的最后一个值：repeated 条件不能由索引判断，否则会漏行。
    db.define_compound_index("rep-last", "p/", {{12, 1}}, 0);
    db.checkpoint();
    auto q = query();
    q.where = a;
    q.limit = 4096;
    auto indexed = select(db, q);
    auto got = ids(indexed);
    std::sort(got.begin(), got.end());
    check(got == model([&](unsigned i) { return has(unpacked(i), 5); }) &&
              indexed.access[0].strategy != "index_range",
          "repeated condition used a scalar index");
    // 对照：同一字段按普通标量查询仍只看最后一个值，并可走索引。
    check(run(E::compare(F::field(12, 1), QueryOp::Eq, V::uint(5))) ==
              model([&](unsigned i) { return !unpacked(i).empty() && unpacked(i).back() == 5; }),
          "plain scalar query lost last-value semantics");
    // 不允许的位置与混用：排序、输出列、分组、键、同路径混用。
    q = query();
    q.where = a;
    q.order = {{rep(12, 1), false, true}};
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT, "repeated accepted in ORDER BY");
    q = query();
    q.columns = {{rep(12, 1), A::None, {}}};
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT, "repeated accepted as a column");
    q = query();
    q.group = {rep(12, 1)};
    q.columns = {{rep(12, 1), A::None, {}}};
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT, "repeated accepted in GROUP BY");
    q = query();
    q.where = E::all({a, E::compare(F::field(12, 1), QueryOp::Eq, V::uint(5))});
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT,
          "same path used as repeated and plain");
    q = query();
    auto key_repeated = F::key();
    key_repeated.repeated = true;
    q.where = E::compare(key_repeated, QueryOp::Eq, V::text("p/1"));
    check(status([&] { select(db, q); }) == FQDB_INVALID_ARGUMENT, "repeated key accepted");
}
int main() {
    try {
        repeated_fields_match_any_element();
        index_scan_decodes_owner_from_key();
        like_prefix_fast_path_matches_nfa();
        rules_reuse_respects_catalog_epoch();
        filter_scan_matches_model();
        conditions_order_and_covering();
        store_snapshot_path();
        sparse_indices_paging_and_conflicts();
        groups_numeric_and_nulls();
        joins_and_patterns();
        lifecycle_and_concurrency();
        cache_schema_and_metadata_only();
        std::cout << "PASS generic native query conditions, plans, Top N, groups, joins, patterns, "
                     "conflicts and budgets\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
