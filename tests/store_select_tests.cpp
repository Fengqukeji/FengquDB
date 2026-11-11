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
int main() {
    try {
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
