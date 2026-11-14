#include "fengqudb_index.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>

namespace idx = fengqudb::index;
static void check(bool value) {
    if (!value)
        throw std::runtime_error("C++ index expectation failed");
}
static void large_results(const std::filesystem::path &root) {
    fengqudb::Store store(root.string());
    idx::Client index(store, "bulk", "public");
    const std::string body = "bulkneedle " + std::string(900000, 'x');
    for (unsigned i = 0; i < 20; ++i) {
        idx::Document d;
        d.id = "row" + std::to_string(100 + i);
        d.texts = {{"summary", "bulkneedle", 1}, {"body", body, 1}};
        d.payload = "etag";
        d.source_locator = "source/" + d.id;
        d.metadata = {{"kind", idx::Value::text("large")}};
        d.vector = {1, 0, 0};
        d.vector_space = "bulk-v1";
        auto tx = store.transaction();
        index.upsert(tx, d);
        check(bool(tx.commit(true)));
    }
    auto view = store.snapshot();
    idx::Query q;
    q.text = "bulkneedle";
    q.memory_budget = 64ull << 20;
    for (unsigned limit : {19u, 20u}) {
        q.limit = limit;
        for (bool summary_only : {false, true}) {
            q.scoring_fields =
                summary_only ? std::vector<std::string>{"summary"} : std::vector<std::string>{};
            auto result = index.query(view, q);
            check(result.complete() && result.hits.size() == limit && result.total_hits == 20);
            size_t wire_size = 0;
            for (const auto &hit : result.hits) {
                wire_size += hit.document_wire.size();
                check(hit.payload == "etag" && hit.projection_version == 1);
                bool body_found = false, vector_found = false;
                fengqudb::wire::Reader r(hit.document_wire);
                fengqudb::wire::Field f;
                while (r.next(f)) {
                    check(f.number <= 12); // no private cached terms/field lengths
                    if (f.number == 5) {
                        std::string_view text;
                        check(r.bytes(text));
                        fengqudb::wire::Reader a(text);
                        fengqudb::wire::Field b;
                        while (a.next(b)) {
                            if (b.number == 2) {
                                std::string_view value;
                                check(a.bytes(value));
                                body_found |= value == body;
                            } else
                                check(a.skip(b));
                        }
                        check(a.done());
                    } else {
                        vector_found |= f.number == 7;
                        check(r.skip(f));
                    }
                }
                check(r.done() && body_found && vector_found);
            }
            check(wire_size > FQDB_MAX_VALUE_BYTES);
        }
    }
    q.result_projection = FQIDX_PROJECT_HEADER;
    auto header = index.query(view, q);
    check(header.result_projection == FQIDX_PROJECT_HEADER && header.hits.size() == 20);
    std::set<std::string_view> ids;
    for (const auto &hit : header.hits) {
        ids.insert(hit.id);
        check(hit.payload == "etag" && hit.source_locator.starts_with("source/"));
        check(hit.document_wire.size() < 1024);
        fengqudb::wire::Reader r(hit.document_wire);
        fengqudb::wire::Field f;
        while (r.next(f)) {
            check(f.number != 5 && f.number != 7 && f.number <= 12);
            check(r.skip(f));
        }
        check(r.done());
    }
    check(ids.size() == 20);
    for (auto projection : {FQIDX_PROJECT_FULL, FQIDX_PROJECT_HEADER}) {
        q.result_projection = static_cast<uint32_t>(projection);
        q.output_budget = 1;
        q.allow_partial = true;
        bool rejected = false;
        try {
            (void)index.query(view, q);
        } catch (const fengqudb::Error &e) {
            rejected = e.code() == FQDB_LIMIT;
        }
        check(rejected);
    }
    q = {};
    q.operation = idx::Query::Get;
    q.id = "row100";
    auto get = index.query(view, q);
    check(get.hits.size() == 1 && get.hits.front().document_wire.size() > body.size());
    q = {};
    q.after_id = "row100";
    bool rejected = false;
    try {
        (void)index.query(view, q);
    } catch (const fengqudb::Error &e) {
        rejected = e.code() == FQDB_INVALID_ARGUMENT;
    }
    check(rejected);
    fqdb_buffer copy{};
    std::string bytes(FQDB_MAX_VALUE_BYTES + 1, 'b');
    check(fqdb_buffer_copy({bytes.data(), bytes.size()}, &copy) == FQDB_OK);
    check(copy.size == bytes.size() && std::memcmp(copy.data, bytes.data(), bytes.size()) == 0);
    fqdb_buffer_free(&copy);
    check(!copy.data && !copy.size);
}
int main() {
    const auto root = std::filesystem::current_path() /
                      ("index-cpp-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        {
            fengqudb::Store store(root.string());
            idx::Client index(store, "project", "public");
            idx::Document document;
            document.id = "one";
            document.texts = {{"title", "transactional index", 2}, {"body", "native search", 1}};
            document.metadata = {{"kind", idx::Value::text("file")},
                                 {"line", idx::Value::integer(42)},
                                 {"wide", idx::Value::unsigned_integer(UINT64_MAX)}};
            document.vector = {1, 0};
            document.vector_space = "embedding-v1";
            document.payload = "caller bytes";
            {
                auto tx = store.transaction();
                tx.put("original", "same transaction");
                index.upsert(tx, document);
                idx::Query q;
                q.text = "native";
                auto result = index.query(tx, q);
                check(result.hits.size() == 1 && result.hits[0].payload == "caller bytes");
                check(bool(tx.commit(true)));
            }
            auto old = store.snapshot();
            {
                auto tx = store.transaction();
                idx::Patch patch;
                patch.id = "one";
                patch.expected_version = 1;
                patch.next_version = 2;
                patch.metadata.emplace("kind", idx::Value::text("record"));
                index.patch(tx, patch);
                check(bool(tx.commit(true)));
            }
            idx::Query q;
            q.text = "index";
            q.where = idx::Filter::all(
                {idx::Filter::eq("kind", idx::Value::text("file")),
                 idx::Filter::compare("line", idx::Filter::Greater, idx::Value::integer(10))});
            auto result = index.query(old, q);
            check(result.complete() && result.hits.size() == 1 && result.total_hits == 1);
            auto moved = std::move(result);
            check(moved.hits[0].id == "one" && moved.hits[0].projection_version == 1);
            auto current = store.snapshot();
            check(index.query(current, q).hits.empty());
            q = {};
            q.dense = true;
            q.vector = {1, 0};
            q.vector_space = "embedding-v1";
            check(index.query(current, q).hits.front().dense_similarity == 1);
            q.text = "must not be silently ignored";
            bool rejected = false;
            try {
                (void)index.query(current, q);
            } catch (const fengqudb::Error &e) {
                rejected = e.code() == FQDB_INVALID_ARGUMENT;
            }
            check(rejected);
            q = {};
            q.where = idx::Filter::eq("kind", idx::Value::text(std::string("\xff", 1)));
            rejected = false;
            try {
                (void)index.query(current, q);
            } catch (const fengqudb::Error &e) {
                rejected = e.code() == FQDB_INVALID_ARGUMENT;
            }
            check(rejected);
            {
                auto tx = store.transaction();
                index.erase(tx, "one", 2, 3, 1);
                check(bool(tx.commit(true)));
            }
            q = {};
            q.operation = idx::Query::List;
            check(index.query(store.snapshot(), q).hits.empty());
        }
        large_results(root / "large");
        std::filesystem::remove_all(root);
        std::cout << "PASS C++ index convenience API\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
