// Typed query builder (fengqudb_query.hpp): request encoding must equal the
// hand-written StoreQuery, and typed rows, aggregates, HAVING references, NULL
// access, pagination, joins, transactions and misuse errors must behave as documented.
#include "store_test_support.hpp"
#include "fengqudb_query.hpp"
#include <cmath>
#include <iostream>
namespace q = fengqudb::q;
namespace wire = fengqudb::wire;
using fengqudb::QueryExpr;
using fengqudb::QueryField;
using fengqudb::QueryOp;
using fengqudb::QueryValue;
using fengqudb::StoreQuery;

static std::string doc(unsigned id) {
    wire::Writer w;
    w.uint_field(1, id);
    w.bytes_field(2, id % 2 ? "mage" : "warrior");
    w.uint_field(3, id % 20);
    w.sint_field(4, int64_t(id) * 11 - 100);
    w.bytes_field(5, "Name-" + std::to_string(id));
    if (id % 4)
        w.uint_field(10, id % 3);
    return w.take();
}
static void seed(Store &db) {
    auto tx = db.transaction();
    for (unsigned i = 0; i < 64; ++i)
        tx.put("p/" + std::to_string(i), doc(i));
    for (unsigned i = 0; i < 8; ++i) {
        wire::Writer w;
        w.uint_field(1, i % 4);
        w.uint_field(2, i);
        tx.put("o/" + std::to_string(i), w.data());
    }
    commit(tx);
    db.define_compound_index("class-level", "p/", {{2, 4}, {3, 1}}, 2);
    db.checkpoint();
}

int main() {
    try {
        Fixture f;
        Store db(f.name("builder"));
        seed(db);
        auto id = q::u64(1), level = q::u64(3), opt = q::u64(10);
        auto cls = q::text(2), name = q::text(5);
        auto gold = q::s64(4);

        // Encoding equals the hand-written low-level request.
        auto top = q::from("p/")
                       .where(cls == "mage" && level.between(5, 15))
                       .select(id, gold)
                       .order_by(gold.desc(), id)
                       .limit(7);
        StoreQuery manual;
        manual.prefix = "p/";
        manual.where = QueryExpr::all(
            {QueryExpr::compare(QueryField::field(2, 4), QueryOp::Eq, QueryValue::text("mage")),
             QueryExpr::compare(QueryField::field(3, 1), QueryOp::Ge, QueryValue::uint(5)),
             QueryExpr::compare(QueryField::field(3, 1), QueryOp::Le, QueryValue::uint(15))});
        manual.columns.resize(2);
        manual.columns[0].field = QueryField::field(1, 1);
        manual.columns[1].field = QueryField::field(4, 5);
        manual.order.resize(2);
        manual.order[0].field = QueryField::field(4, 5);
        manual.order[0].descending = true;
        manual.order[1].field = QueryField::field(1, 1);
        manual.limit = 7;
        check(top.build().encode() == manual.encode(), "builder encoding differs from StoreQuery");

        // repeated: .any() encodes the same request as a hand-written repeated field.
        auto items = q::u64(12);
        auto typed_any = q::from("p/").where(items.any() == uint64_t(5)).select(id).limit(20);
        StoreQuery any_manual;
        any_manual.prefix = "p/";
        any_manual.where =
            QueryExpr::compare(QueryField::field(12, 1).any(), QueryOp::Eq, QueryValue::uint(5));
        any_manual.columns.resize(1);
        any_manual.columns[0].field = QueryField::field(1, 1);
        any_manual.limit = 20;
        check(typed_any.build().encode() == any_manual.encode(),
              "repeated builder encoding differs from StoreQuery");
        check(typed_any.build().encode() !=
                  q::from("p/").where(items == uint64_t(5)).select(id).limit(20).build().encode(),
              "repeated flag missing from builder encoding");

        // Typed rows.
        auto rows = q::run(db, top);
        check(rows.size() == 7, "typed top-N row count");
        int64_t previous = INT64_MAX;
        for (auto row : rows) {
            uint64_t v = row[id];
            int64_t g = row[gold];
            check(v % 2 == 1 && v % 20 >= 5 && v % 20 <= 15, "typed filter result");
            check(g == int64_t(v) * 11 - 100 && g <= previous, "typed value or order");
            previous = g;
        }
        check(top.prepare().run(db).size() == 7, "prepared builder query");

        // Aggregates, typed aggregate access and HAVING with two aggregate refs.
        auto groups = q::from("p/")
                          .group_by(cls)
                          .select(cls, q::count(), q::sum(gold))
                          .having(q::count() > 31 && q::sum(gold) > int64_t(8000))
                          .order_by(cls);
        auto g = q::run(db, groups);
        check(g.size() == 1 && g[0][cls] == "mage" && g[0][q::count()] == 32 &&
                  g[0][q::sum(gold)] == 8064,
              "group/having aggregate references");
        auto by_count = q::from("p/")
                            .group_by(cls)
                            .select(cls, q::count(), q::avg(gold))
                            .order_by(q::sum(gold).desc());
        check(status([&] { (void)by_count.build(); }) == FQDB_INVALID_ARGUMENT,
              "ordering by an unselected aggregate accepted");
        by_count.order_by(q::avg(gold).desc());
        auto ordered = q::run(db, by_count);
        // AVG uses compensated floating summation; compare with a tolerance.
        check(ordered.size() == 2 && ordered[0][cls] == "mage" &&
                  std::abs(ordered[0][q::avg(gold)] - 252.0) < 1e-9 &&
                  ordered[0][q::avg(gold)] > ordered[1][q::avg(gold)],
              "order by selected aggregate");

        // NULL access, IN, LIKE.
        auto missing = q::run(db, q::from("p/").where(opt.is_null()).select(id, opt).limit(100));
        check(missing.size() == 16, "is_null count");
        check(!missing[0].get(opt).has_value(), "missing field is not nullopt");
        check(status([&] { (void)missing[0][opt]; }) == FQDB_NOT_FOUND, "NULL operator[]");
        check(status([&] { (void)missing[0][gold]; }) == FQDB_INVALID_ARGUMENT,
              "unselected column read");
        check(q::run(db, q::from("p/").where(id.in({3, 8, 3})).select(id)).size() == 2, "in()");
        check(q::run(db, q::from("p/").where(name.like("Name-1%")).select(id)).size() == 11,
              "like()");
        check(q::run(db, q::from("p/").where(!(cls == "mage")).select(id)).size() == 32, "not");

        // Cursor pagination.
        auto page = q::from("p/").select(id).order_by(gold.desc()).limit(9);
        std::vector<uint64_t> seen;
        for (;;) {
            auto r = q::run(db, page);
            for (auto row : r)
                seen.push_back(row[id]);
            if (r.cursor().empty())
                break;
            page.after(r.cursor());
        }
        check(seen.size() == 64 && seen.front() == 63 && seen.back() == 0, "cursor pagination");

        // Equality join: field 1 of the first join source.
        auto amount = q::u64(2, 1);
        auto joined =
            q::run(db, q::from("p/").join("o/", id, q::u64(1, 1)).select(id, amount).limit(100));
        check(joined.size() == 8, "join row count");
        for (auto row : joined)
            check(row[amount] % 4 == row[id], "join pairing");

        // Store queries use a snapshot; transactions see private writes.
        auto all = q::from("p/").select(id).limit(4096);
        auto tx = db.transaction();
        tx.put("p/100", doc(100));
        check(q::run(tx, all).size() == 65 && q::run(db, all).size() == 64,
              "transaction/private visibility");

        // Aggregates are rejected in where().
        check(status([&] { q::from("p/").where(q::count() > 1); }) == FQDB_INVALID_ARGUMENT,
              "aggregate accepted in where()");
        db.verify();
        std::cout << "PASS typed query builder encoding, rows, aggregates, nulls, paging, joins\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
