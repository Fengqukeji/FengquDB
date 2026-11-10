#include "store_test_support.hpp"
#include "paged/query.hpp"
#include <map>
#include <random>
#include <tuple>

using fengqudb::StoreIndexQuery;
static std::string document(std::string_view group, int64_t score,
                            std::string_view payload = "data") {
    fengqudb::wire::Writer out;
    out.bytes_field(1, group);
    out.sint_field(2, score);
    out.bytes_field(3, payload);
    return out.take();
}
static std::string group_field(std::string_view group) {
    fengqudb::wire::Writer out;
    out.bytes_field(1, group);
    return out.take();
}
static std::string score_field(int64_t score) {
    fengqudb::wire::Writer out;
    out.sint_field(2, score);
    return out.take();
}
static std::vector<std::string> keys(const fengqudb::StoreCommandResult &result) {
    std::vector<std::string> out;
    for (auto &row : result.rows)
        out.push_back(row.key);
    return out;
}
static void ranges_private_and_constraints() {
    Fixture fixture;
    auto path = fixture.name("source");
    {
        Store db(path);
        db.put_many({{"d/1", document("a", -5)},
                     {"d/2", document("a", 0)},
                     {"d/3", document("a", 5)},
                     {"d/4", document("b", 0)}});
        db.define_compound_index("group-score", "d/", {{1, 4}, {2, 5}}, 3);
        db.define_compound_index("group-score", "d/", {{1, 4}, {2, 5}}, 3);
        check(status([&] { db.define_compound_index("group-score", "d/", {{2, 5}, {1, 4}}, 3); }) ==
                  FQDB_ALREADY_EXISTS,
              "changed compound definition accepted");
        StoreIndexQuery q;
        q.equal = group_field("a");
        q.lower = score_field(0);
        q.upper = score_field(5);
        q.upper_inclusive = true;
        auto first = db.query_index("group-score", q, {}, 1);
        check(keys(first) == std::vector<std::string>{"d/2"} && !first.cursor.empty(),
              "compound range first page");
        check(keys(db.query_index("group-score", q, first.cursor)) ==
                  std::vector<std::string>{"d/3"},
              "compound range continuation");
        q.reverse = true;
        auto reverse = db.query_index("group-score", q, {}, 1);
        check(keys(reverse) == std::vector<std::string>{"d/3"} &&
                  keys(db.query_index("group-score", q, reverse.cursor)) ==
                      std::vector<std::string>{"d/2"},
              "reverse cursor");
        q.reverse = false;
        q.lower_exclusive = true;
        check(keys(db.query_index("group-score", q)) == std::vector<std::string>{"d/3"},
              "exclusive lower boundary");
        q.upper_inclusive = false;
        check(db.query_index("group-score", q).rows.empty(), "equal excluded boundaries");
        q.upper = score_field(0);
        check(db.query_index("group-score", q).rows.empty(),
              "same-value open interval was not empty");
        q = {};
        q.equal = group_field("a");
        q.fields = {3, 3};
        auto projected = db.query_index("group-score", q);
        fengqudb::wire::Writer selected;
        selected.bytes_field(3, "data");
        check(projected.rows.size() == 3 && projected.rows[0].value == selected.data(),
              "query projection");
        auto tx = db.transaction();
        auto mark = tx.savepoint();
        tx.put("d/1", document("c", 9));
        tx.erase("d/2");
        tx.put("d/new", document("a", 7));
        check(keys(tx.query_index("group-score", q)) == std::vector<std::string>({"d/3", "d/new"}),
              "query omitted private changes");
        tx.put("d/1", document("a", -8));
        tx.put("d/new", document("b", 8));
        check(keys(tx.query_index("group-score", q)) == std::vector<std::string>({"d/1", "d/3"}),
              "repeated overlay leaked stale entries");
        tx.rollback_to(mark);
        check(tx.query_index("group-score", q).rows.size() == 3,
              "query overlay affected savepoint");
        tx.put("d/1", document("a", 0));
        tx.put("d/2", document("a", -5));
        check(keys(tx.query_index("group-score", q)) ==
                  std::vector<std::string>({"d/2", "d/1", "d/3"}),
              "private unique swap");
        commit(tx);
        db.verify();
        auto duplicate = db.transaction();
        duplicate.put("d/dup", document("a", 5));
        check(status([&] { duplicate.query_index("group-score", q); }) == FQDB_CONSTRAINT,
              "private unique violation");
        check(duplicate.commit().status == fengqudb::Status::constraint, "unique commit violation");
        auto phantom = db.transaction();
        phantom.query_index("group-score", q);
        phantom.put("unrelated", "read dependency");
        db.put_if_changed("d/add", document("a", 8));
        check(phantom.commit().status == fengqudb::Status::conflict, "index phantom dependency");
        check(status([&] {
                  StoreIndexQuery bad;
                  bad.equal = score_field(0);
                  db.query_index("group-score", bad);
              }) == FQDB_INVALID_ARGUMENT,
              "non-leading equality accepted");
        check(status([&] { db.define_compound_index("bad", "d/", {{1, 4}, {1, 4}}); }) ==
                  FQDB_INVALID_ARGUMENT,
              "duplicate descriptor accepted");
        auto ttl = db.transaction();
        ttl.put("d/expired", document("a", 99), 1);
        commit(ttl);
        check(db.query_index("group-score", q).rows.size() == 4, "expired tuple visible");
        auto old = db.transaction();
        auto before = keys(old.query_index("group-score", q));
        db.compact();
        check(keys(old.query_index("group-score", q)) == before, "old query across compaction");
        db.verify();
    }
    Store reopened(path);
    check(reopened.query_index("group-score").rows.size() == 5, "compound restart");
    reopened.drop_index("group-score");
    reopened.define_index("group-score", "d/", 2, 5);
    check(reopened.query_index("group-score").rows.size() == 5, "compound drop/redefine as legacy");
    reopened.verify();
}
static void projections_and_budget() {
    Fixture fixture;
    fengqudb::StoreOptions options;
    options.transaction_bytes = 1024 * 1024;
    auto path = fixture.name("projection");
    {
        Store seed(path);
        put(seed, "large", document("x", 1, std::string(2 * 1024 * 1024, 'p')));
    }
    Store db(path, options);
    db.define_index("group", "", 1, 4);
    check(db.project("large", {1}) == group_field("x"), "small projection charged entire payload");
    check(db.project("large", {}).empty(), "empty projection");
    StoreIndexQuery q;
    q.equal = group_field("x");
    q.fields = {1};
    check(db.query_index("group", q).rows[0].value == group_field("x"), "indexed small projection");
    q.fields.clear();
    q.include_values = true;
    auto budget = db.info().transaction_bytes;
    check(status([&] { db.query_index("group", q); }) == FQDB_LIMIT &&
              db.info().transaction_bytes == budget,
          "query result budget leak");
    db.drop_index("group");
    fengqudb::wire::Writer bytes;
    bytes.bytes_field(3, "first");
    bytes.uint_field(9, 17);
    bytes.bytes_field(3, "last");
    put(db, "repeated", bytes.data());
    fengqudb::wire::Writer expected;
    expected.bytes_field(3, "first");
    expected.bytes_field(3, "last");
    check(db.project("repeated", {3}) == expected.data(), "projection lost repeated order");
    fengqudb::wire::Writer packed;
    packed.bytes_field(5, std::string(1, 3));
    check(db.execute({FQDB_RECORD_PROJECT, {}, "repeated", packed.data()}).value == expected.data(),
          "packed projection selector");
    put(db, "malformed", std::string("\x1a\x05x", 3));
    check(status([&] { db.project("malformed", {}); }) == FQDB_CONSTRAINT,
          "projection accepted malformed protobuf");
    check(status([&] { db.project("missing", {1}); }) == FQDB_NOT_FOUND, "projection missing key");
}
static void source_path_dependencies_and_limits() {
    Fixture fixture;
    auto path = fixture.name("query-source-path");
    {
        Store db(path);
        auto tx = db.transaction();
        for (unsigned i = 0; i < 64; ++i)
            tx.put("d/" + std::to_string(1000 + i), document("g", i, "old"));
        commit(tx);
        db.define_compound_index("group-score", "d/", {{1, 4}, {2, 5}}, 3);
        db.checkpoint();
        StoreIndexQuery query;
        query.equal = group_field("g");
        query.fields = {3};
        auto dependent = db.transaction();
        check(dependent.query_index("group-score", query).rows.size() == 64,
              "source path projected result count");
        // Index membership does not change. The source record dependency must
        // still reject a derived write based on the old non-indexed payload.
        db.put_if_changed("d/1010", document("g", 10, "new"));
        dependent.put("derived", "stale");
        check(dependent.commit().status == fengqudb::Status::conflict,
              "source path lost non-indexed field dependency");
        query.reverse = true;
        query.fields.clear();
        query.include_values = true;
        auto rows = db.query_index("group-score", query);
        check(rows.rows.size() == 64 && rows.rows.front().key == "d/1063" &&
                  rows.rows[53].value == document("g", 10, "new"),
              "source path failed reverse/arbitrary source order");
    }
    for (bool cached : {false, true}) {
        auto opts = p::defaults();
        if (!cached)
            opts.cache_bytes = 0;
        auto core = std::make_shared<p::Core>(path, opts);
        core->open();
        p::Transaction read(core, 0);
        // Enough for the existing schema and cursor, less than the optional
        // source path allowance. A cache optimization must not reject this.
        p::Charge fill(core->pin_budget, {}, opts.pinned_bytes - core->pin_budget->used() - 32768);
        StoreIndexQuery query;
        query.include_values = true;
        auto request =
            fengqudb::StoreCommand{FQDB_INDEX_QUERY, "group-score", {}, query.encode()}.encode();
        p::Charge result_charge;
        auto rows = p::query_index(read, p::FeatureRequest(request), result_charge);
        check(rows.rows.size() == 64 && read.reads.count("d/1010") &&
                  core->pin_budget->used() <= opts.pinned_bytes,
              "source path tight budget/cache-off fallback");
    }
}
static void lazy_payload_io_and_optional_fields() {
    Fixture fixture;
    auto path = fixture.name("lazy");
    {
        Store db(path);
        auto tx = db.transaction();
        for (unsigned i = 0; i < 12; ++i)
            tx.put("d/" + std::to_string(i), document("g", i, std::string(1024 * 1024, 'p')));
        commit(tx);
        db.define_compound_index("compound", "d/", {{1, 4}, {2, 5}});
        put(db, "d/missing", group_field("g"));
        check(db.query_index("compound").rows.size() == 12,
              "optional incomplete tuple was indexed");
        auto missing = db.transaction();
        missing.put("d/missing", document("g", 20));
        commit(missing);
        check(db.query_index("compound").rows.size() == 13, "completed tuple was not indexed");
        db.checkpoint();
    }
    auto opts = p::defaults();
    opts.cache_bytes = 0;
    auto core = std::make_shared<p::Core>(path, opts);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    fengqudb::StoreIndexQuery query;
    query.equal = group_field("g");
    auto encoded =
        fengqudb::StoreCommand{FQDB_INDEX_QUERY, "compound", {}, query.encode(), 0, 0, 5}.encode();
    p::Transaction read(core, 0);
    auto file = core->current()->generation->file;
    auto before = file->read_bytes.load();
    p::Charge key_charge, value_charge;
    auto result = p::query_index(read, p::FeatureRequest(encoded), key_charge);
    auto key_bytes = file->read_bytes.load() - before;
    check(result.rows.size() == 5 && key_bytes < 256 * 1024,
          "keys-only query loaded business payloads");
    query.include_values = true;
    auto value_query = query.encode();
    encoded =
        fengqudb::StoreCommand{FQDB_INDEX_QUERY, "compound", {}, value_query, 0, 0, 5}.encode();
    before = file->read_bytes.load();
    auto values = p::query_index(read, p::FeatureRequest(encoded), value_charge);
    check(values.rows.size() == 5 && file->read_bytes.load() - before >= 5 * 1024 * 1024,
          "included values did not load payloads");
}
static void scalar_order_and_random_model() {
    Fixture fixture;
    Store db(fixture.name("types"));
    for (uint32_t type = 1; type <= 10; ++type) {
        auto prefix = "t" + std::to_string(type) + "/";
        auto index = "i" + std::to_string(type);
        for (int value = -2; value <= 2; ++value) {
            fengqudb::wire::Writer bytes;
            if (type == 1 || type == 7 || type == 9) {
                auto v = static_cast<uint64_t>(value + 2);
                if (type == 1)
                    bytes.uint_field(1, v);
                else if (type == 7)
                    bytes.fixed64_field(1, v);
                else
                    bytes.fixed32_field(1, static_cast<uint32_t>(v));
            } else if (type == 2)
                bytes.uint_field(1, static_cast<uint64_t>(value));
            else if (type == 3)
                bytes.double_field(1, value);
            else if (type == 4)
                bytes.bytes_field(1, std::string(1, static_cast<char>(value + 2)));
            else if (type == 5)
                bytes.sint_field(1, value);
            else if (type == 6)
                bytes.float_field(1, static_cast<float>(value));
            else if (type == 8)
                bytes.fixed64_field(1, static_cast<uint64_t>(value));
            else
                bytes.fixed32_field(1, static_cast<uint32_t>(value));
            put(db, prefix + std::to_string(value + 2), bytes.data());
        }
        db.define_compound_index(index, prefix, {{1, type}}, 3);
        check(keys(db.query_index(index)) ==
                  std::vector<std::string>(
                      {prefix + "0", prefix + "1", prefix + "2", prefix + "3", prefix + "4"}),
              "typed tuple order");
    }
    db.define_compound_index("model", "m/", {{1, 4}, {2, 5}});
    std::map<std::string, std::pair<std::string, int64_t>> model;
    std::mt19937 random(81723);
    for (unsigned round = 0; round < 180; ++round) {
        auto key = "m/" + std::to_string(random() % 24);
        auto group = std::string(1, static_cast<char>('a' + random() % 3));
        int64_t score = static_cast<int>(random() % 21) - 10;
        if (random() % 5 == 0) {
            db.delete_many({key});
            model.erase(key);
        } else {
            db.put_if_changed(key, document(group, score));
            model[key] = {group, score};
        }
        StoreIndexQuery q;
        q.equal = group_field(group);
        q.lower = score_field(-4);
        q.upper = score_field(6);
        q.reverse = (round % 2) != 0;
        std::vector<std::pair<int64_t, std::string>> expected;
        for (const auto &item : model)
            if (item.second.first == group && item.second.second >= -4 && item.second.second < 6)
                expected.push_back({item.second.second, item.first});
        std::sort(expected.begin(), expected.end());
        if (q.reverse)
            std::reverse(expected.begin(), expected.end());
        std::vector<std::string> actual;
        std::string cursor;
        do {
            auto page = db.query_index("model", q, cursor, 3);
            for (auto &row : page.rows)
                actual.push_back(row.key);
            cursor = std::move(page.cursor);
        } while (!cursor.empty());
        std::vector<std::string> wanted;
        for (auto &item : expected)
            wanted.push_back(item.second);
        check(actual == wanted, "randomized compound range model");
    }
    db.checkpoint();
    db.verify();
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 5) {
            Store db(argv[2]);
            fqdb_store_test_crash(std::stoi(argv[4]));
            if (std::string_view(argv[1]) == "--build")
                db.define_compound_index("compound", "d/", {{1, 4}, {2, 5}}, 3);
            else
                db.put_if_changed("d/new", document("new", 3));
            return 88;
        }
        ranges_private_and_constraints();
        projections_and_budget();
        source_path_dependencies_and_limits();
        lazy_payload_io_and_optional_fields();
        scalar_order_and_random_model();
#ifndef __SANITIZE_THREAD__
        auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
        for (int point : {1, 2, 3, 4}) {
            Fixture fixture;
            auto path = fixture.name("write-crash");
            {
                Store db(path);
                db.define_compound_index("compound", "d/", {{1, 4}, {2, 5}}, 3);
            }
            check(child(executable, "--write", path, path, point) == 86,
                  "compound commit crash child");
            Store reopened(path);
            const bool present = bool(reopened.get("d/new"));
            check((point == 1 ? !present : point == 2 || present) &&
                      reopened.query_index("compound").rows.size() == (present ? 1u : 0u),
                  "compound crash partial index");
            reopened.verify();
        }
        for (int point : {45, 46}) {
            Fixture fixture;
            auto path = fixture.name("build-crash");
            {
                Store db(path);
                auto tx = db.transaction();
                for (unsigned i = 0; i < 96; ++i)
                    tx.put("d/" + std::to_string(i), document("a", i));
                commit(tx);
            }
            check(child(executable, "--build", path, path, point) == 86,
                  "compound build crash child");
            Store reopened(path);
            auto info = reopened.define_compound_index("compound", "d/", {{1, 4}, {2, 5}}, 3);
            check(info.copied_keys == (point == 45 ? 96u : 64u) &&
                      reopened.query_index("compound").rows.size() == 96,
                  "compound build did not resume");
            reopened.verify();
        }
#endif
        std::cout << "PASS compound typed indexes, range/reverse pagination, private query "
                     "overlays, raw projections, budgets and crash/restart\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
