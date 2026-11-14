#include "fengqudb_index.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace idx = fengqudb::index;
static void check(bool value) {
    if (!value)
        throw std::runtime_error("C++ index expectation failed");
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
        std::filesystem::remove_all(root);
        std::cout << "PASS C++ index convenience API\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
