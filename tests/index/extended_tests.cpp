#include "fengqudb_index.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>

namespace idx = fengqudb::index;
static unsigned checks = 0;
static void expect(bool ok, const char *why) {
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
static void commit(fengqudb::StoreTransaction &tx) {
    auto r = tx.commit(true);
    expect(bool(r), "commit");
}
static void prepare(fengqudb::Store &store, const idx::Client &index) {
    idx::MaintenanceOptions options;
    options.operation = idx::MaintenanceOptions::RebuildGraph;
    options.max_records = 256;
    for (unsigned i = 0;; ++i) {
        auto tx = store.transaction();
        auto state = index.maintain(tx, options);
        commit(tx);
        expect(i < 64, "graph prepare is bounded");
        if (state.finished)
            break;
    }
}
static std::vector<std::string> ids(const idx::Result &r) {
    std::vector<std::string> o;
    for (const auto &h : r.hits)
        o.emplace_back(h.id);
    return o;
}
static void text_tests(fengqudb::Store &store) {
    idx::Client index(store, "extended", "all"), visible(store, "extended", "visible");
    {
        auto tx = store.transaction();
        for (unsigned i = 0; i < 160; ++i) {
            idx::Document d;
            d.id = "record" + std::to_string(1000 + i);
            d.texts = {
                {"body",
                 i % 2 ? "needle alpha beta transaction" : "needle alpha x beta transactions", 1}};
            d.metadata = {{"visible", idx::Value::boolean(i % 3 == 0)}};
            index.upsert(tx, d);
            if (i % 3 == 0)
                visible.upsert(tx, d);
        }
        idx::Document d;
        d.id = "chinese";
        d.texts = {{"body", "中文数据库，可靠持久化", 1}};
        index.upsert(tx, d);
        commit(tx);
    }
    idx::Query q;
    q.text = "needle";
    q.limit = 160;
    q.include_explain = true;
    auto old = store.snapshot();
    auto before = index.query(old, q);
    expect(before.hits.size() == 160, "uncompacted hits");
    unsigned batches = 0;
    for (;;) {
        auto tx = store.transaction();
        idx::MaintenanceOptions m;
        m.max_terms = 2;
        auto state = index.maintain(tx, m);
        commit(tx);
        expect(++batches < 64, "maintenance resume terminates");
        if (state.finished)
            break;
        expect(!state.next_term.empty(), "persistent term cursor");
    }
    auto view = store.snapshot();
    auto after = index.query(view, q);
    expect(ids(before) == ids(after), "compaction preserves ranking");
    expect(after.blocks_visited >= 2 && after.layout_generation > before.layout_generation,
           "compressed blocks used");
    for (size_t i = 0; i < after.hits.size(); ++i) {
        expect(after.hits[i].score == before.hits[i].score, "compaction score exact");
        double sum = 0;
        for (const auto &d : after.hits[i].details)
            sum += d.contribution;
        expect(std::abs(sum - after.hits[i].score) < 1e-12, "explain equals score");
    }
    q.limit = 8;
    auto blocked = index.query(view, q);
    auto original = index.query(old, q);
    expect(ids(blocked) == ids(original) && blocked.records_scanned < original.records_scanned,
           "block maximum skips work without changing exact top K");
    expect(!blocked.total_hits, "skipped blocks do not claim exact count");
    bool within_term = false;
    for (unsigned count = 0;; ++count) {
        auto tx = store.transaction();
        idx::MaintenanceOptions m;
        m.max_terms = 1;
        m.max_postings = 17;
        auto state = index.maintain(tx, m);
        commit(tx);
        within_term |= !state.next_id.empty();
        expect(count < 256, "tiny-budget compaction eventually completes");
        if (state.finished)
            break;
    }
    q.limit = 160;
    expect(within_term && ids(index.query(store.snapshot(), q)) == ids(before),
           "compaction resumes inside old blocks without losing their tails");
    q = {};
    q.phrases = {{"alpha beta", 0, "body"}};
    q.limit = 160;
    expect(index.query(view, q).hits.size() == 80, "exact primary-token phrase");
    q.phrases[0].max_gap = 1;
    expect(index.query(view, q).hits.size() == 160, "proximity allows one intervening token");
    q = {};
    q.text = "needle";
    q.excluded_terms = {"x"};
    q.limit = 160;
    expect(index.query(view, q).hits.size() == 80, "lexical NOT");
    q.excluded_terms.clear();
    q.scoring_fields = {"missing"};
    expect(index.query(view, q).hits.empty(), "field-restricted search");
    q = {};
    q.text = "trans";
    q.prefix_last = true;
    q.limit = 160;
    expect(index.query(view, q).hits.size() == 160, "prefix vocabulary expansion");
    q = {};
    q.text = "数据库";
    q.include_positions = true;
    q.snippet_bytes = 24;
    auto chinese = index.query(view, q);
    expect(chinese.hits.size() == 1 && !chinese.hits[0].positions.empty(), "Chinese offsets");
    std::string body = "中文数据库，可靠持久化";
    for (const auto &p : chinese.hits[0].positions) {
        expect(p.begin < p.end && p.end <= body.size(), "offset range");
        expect(p.snippet ==
                   std::string_view(body).substr(size_t(p.snippet_begin), p.snippet.size()),
               "snippet bound to original bytes");
        expect(p.snippet.empty() || (static_cast<unsigned char>(p.snippet.front()) & 0xc0) != 0x80,
               "UTF8 snippet boundary");
    }
    q = {};
    q.text = "needle alpha";
    q.limit = 160;
    q.statistics_policy = 1;
    q.where = idx::Filter::eq("visible", idx::Value::boolean(true));
    auto filtered = index.query(view, q);
    q.statistics_policy = 0;
    q.where = {};
    auto oracle = visible.query(view, q);
    expect(filtered.record_count == oracle.record_count,
           "statistics do not reveal hidden corpus counts");
    expect(ids(filtered) == ids(oracle) && filtered.statistics_scanned == 161,
           "filtered statistics isolate hidden records");
    for (size_t i = 0; i < oracle.hits.size(); ++i)
        expect(std::abs(filtered.hits[i].score - oracle.hits[i].score) < 1e-12,
               "filtered BM25 score oracle");
    q.statistics_policy = 1;
    q.where = idx::Filter::eq("visible", idx::Value::boolean(true));
    q.scan_budget = 2;
    bool limited = false;
    try {
        (void)index.query(view, q);
    } catch (const fengqudb::Error &e) {
        limited = e.code() == FQDB_LIMIT;
    }
    expect(limited, "statistics obey scan budget");
    q.allow_partial = true;
    auto partial = index.query(view, q);
    expect(!partial.complete() && partial.hits.empty() && partial.match_evidence == 0,
           "partial statistics never score a false complete set");
    {
        auto tx = store.transaction();
        idx::Document d;
        d.id = "record1000";
        d.projection_version = 2;
        d.texts = {{"body", "replacement", 1}};
        index.upsert(tx, d, 1);
        index.erase(tx, "record1001", 1, 2, 1);
        commit(tx);
    }
    q = {};
    q.text = "needle";
    q.limit = 160;
    auto updated = index.query(store.snapshot(), q);
    expect(updated.hits.size() == 158, "delta and tombstones override compressed blocks");
    expect(index.query(old, q).hits.size() == 160, "old snapshot sees pre-compaction projection");
    {
        auto tx = store.transaction();
        tx.put("must-rollback", "domain");
        idx::MaintenanceOptions m;
        m.memory_budget = 4096;
        bool failed = false;
        try {
            (void)index.maintain(tx, m);
        } catch (const fengqudb::Error &e) {
            failed = e.code() == FQDB_LIMIT;
        }
        expect(failed, "maintenance memory budget");
        expect(tx.commit(true).status == fengqudb::Status::cancelled,
               "maintenance error cancels domain writes");
    }
    expect(!store.get("must-rollback"), "maintenance rollback atomic");
    for (;;) {
        auto tx = store.transaction();
        idx::MaintenanceOptions m;
        m.operation = idx::MaintenanceOptions::Verify;
        m.max_records = 64;
        auto state = index.maintain(tx, m);
        commit(tx);
        if (state.finished)
            break;
    }
}
static void vector_tests(fengqudb::Store &store) {
    constexpr unsigned count = 4096, dimension = 24;
    idx::Client index(store, "vectors", "all");
    std::mt19937 rng(177);
    std::vector<std::vector<float>> corpus(count, std::vector<float>(dimension));
    auto begin = std::chrono::steady_clock::now();
    for (unsigned first = 0; first < count; first += 128) {
        auto tx = store.transaction();
        for (unsigned i = first; i < std::min(count, first + 128); ++i) {
            for (auto &f : corpus[i])
                f = static_cast<float>(int(rng() % 2001) - 1000) / 1000;
            idx::Document d;
            d.id = "vector" + std::to_string(10000 + i);
            d.vector = corpus[i];
            d.vector_space = "model";
            d.metadata = {{"selected", idx::Value::boolean(i % 8 == 0)}};
            index.upsert(tx, d);
        }
        commit(tx);
    }
    double ingest = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    prepare(store, index);
    double built = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    auto view = store.snapshot();
    double exact_us = 0, ann_us = 0;
    unsigned found = 0, total = 0;
    for (unsigned i = 0; i < 16; ++i) {
        idx::Query q;
        q.dense = true;
        q.vector = corpus[i * 29];
        q.vector_space = "model";
        q.candidate_budget = count + 1;
        auto at = std::chrono::steady_clock::now();
        auto exact = index.query(view, q);
        exact_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - at)
                        .count();
        q.approximate = true;
        q.ef_search = 96;
        at = std::chrono::steady_clock::now();
        auto ann = index.query(view, q);
        ann_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - at)
                      .count();
        expect(ann.complete() && ann.ranking_kind == 1 && ann.graph_visited > 0,
               "HNSW actual path");
        expect(ann.hits.size() == 8, "HNSW top K");
        auto want = ids(exact);
        std::set<std::string> wanted(want.begin(), want.end());
        for (const auto &h : ann.hits) {
            if (wanted.contains(std::string(h.id)))
                ++found;
            ++total;
            auto n = static_cast<unsigned>(std::stoul(std::string(h.id.substr(6)))) - 10000;
            double dot = 0, a = 0, b = 0;
            for (unsigned j = 0; j < dimension; ++j) {
                dot += double(q.vector[j]) * corpus[n][j];
                a += double(q.vector[j]) * q.vector[j];
                b += double(corpus[n][j]) * corpus[n][j];
            }
            expect(std::abs(h.score - dot / std::sqrt(a * b)) < 1e-6, "FP32 authoritative rerank");
        }
    }
    double recall = double(found) / total;
    expect(recall >= .95, "HNSW recall gate");
    {
        auto tx = store.transaction();
        for (unsigned i = 0; i < 16; ++i)
            index.erase(tx, "vector" + std::to_string(10000 + i), 1, 2, 1);
        commit(tx);
    }
    idx::Query q;
    q.approximate = true;
    q.vector = corpus[0];
    q.vector_space = "model";
    auto current = store.snapshot();
    for (const auto &h : index.query(current, q).hits)
        expect(std::stoul(std::string(h.id.substr(6))) >= 10016, "deleted graph node not returned");
    q.where = idx::Filter::eq("selected", idx::Value::text("wrong type"));
    auto none = index.query(current, q);
    expect(none.hits.empty() && none.match_evidence == 2, "empty ANN validated exactly");
    std::cout << "HNSW count=" << count << " recall@8=" << recall << " ingest_s=" << ingest
              << " build_s=" << built << " exact_us=" << exact_us / 16 << " ann_us=" << ann_us / 16
              << "\n";
}
static std::string boundary_tests(fengqudb::Store &store) {
    idx::Client binary(store, "binary", "p");
    std::vector<std::string> names = {std::string("a\0b", 3), std::string("a\0a", 3),
                                      std::string("\xff", 1), std::string("\0", 1)};
    {
        auto tx = store.transaction();
        for (const auto &id : names) {
            idx::Document d;
            d.id = id;
            d.texts = {{"body", "binaryneedle", 1}};
            binary.upsert(tx, d);
        }
        commit(tx);
    }
    {
        auto tx = store.transaction();
        (void)binary.maintain(tx);
        commit(tx);
    }
    std::sort(names.begin(), names.end());
    idx::Query q;
    q.text = "binaryneedle";
    expect(ids(binary.query(store.snapshot(), q)) == names,
           "compressed binary IDs preserve bytewise order");
    for (unsigned dimension = 1; dimension < 20; ++dimension) {
        idx::Client index(store, "tails", std::to_string(dimension));
        idx::Document d;
        d.id = "one";
        d.vector_space = "m";
        std::vector<float> values;
        for (unsigned i = 0; i < dimension; ++i) {
            d.vector.push_back(float(i + 1) * (i % 2 ? .5f : -1.f));
            values.push_back(float(i + 2) * .3f);
        }
        {
            auto tx = store.transaction();
            index.upsert(tx, d);
            commit(tx);
        }
        q = {};
        q.dense = true;
        q.vector = values;
        q.vector_space = "m";
        double a = 0, b = 0, dot = 0;
        for (unsigned i = 0; i < dimension; ++i) {
            a += double(d.vector[i]) * d.vector[i];
            b += double(values[i]) * values[i];
            dot += double(d.vector[i]) * values[i];
        }
        auto result = index.query(store.snapshot(), q);
        expect(std::abs(result.hits.front().score - dot / std::sqrt(a * b)) < 1e-6,
               "SIMD/scalar tail oracle");
    }
    idx::Client duplicates(store, "quantization", "p");
    for (unsigned first = 0; first < 1100; first += 100) {
        auto tx = store.transaction();
        for (unsigned i = first; i < std::min(1100u, first + 100); ++i) {
            idx::Document d;
            d.id = "near" + std::to_string(10000 + i);
            d.vector_space = "m";
            d.vector = {1, 1 + float(i) * .000001f};
            duplicates.upsert(tx, d);
        }
        commit(tx);
    }
    q = {};
    prepare(store, duplicates);
    q.dense = true;
    q.vector_space = "m";
    q.vector = {1, 1.0011f};
    q.candidate_budget = 1200;
    auto snapshot = store.snapshot();
    auto exact = duplicates.query(snapshot, q);
    q.approximate = true;
    auto tied = duplicates.query(snapshot, q);
    expect(ids(exact) == ids(tied) && tied.ranking_kind == 0 && tied.graph_visited > 0,
           "indistinguishable INT8 navigation falls back exactly");
    // Close/reopen can resume bounded graph repair from its persistent cursor.
    idx::MaintenanceOptions repair;
    repair.operation = idx::MaintenanceOptions::RebuildGraph;
    repair.max_records = 100;
    auto tx = store.transaction();
    auto r = duplicates.maintain(tx, repair);
    expect(!r.finished && !r.next_id.empty(), "graph repair checkpoint");
    commit(tx);
    {
        auto update = store.transaction();
        idx::Document changed;
        changed.id = "near10005";
        changed.projection_version = 2;
        changed.vector = {0, 1};
        changed.vector_space = "m";
        duplicates.upsert(update, changed, 1);
        duplicates.erase(update, "near10006", 1, 2, 1);
        duplicates.erase(update, "near11050", 1, 2, 1);
        idx::Document before_cursor;
        before_cursor.id = "near09999";
        before_cursor.vector = {.3f, 1};
        before_cursor.vector_space = "m";
        duplicates.upsert(update, before_cursor);
        commit(update);
    }
    return r.next_id;
}
int main() {
    auto path = std::filesystem::current_path() /
                ("index-extended-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::string checkpoint;
        {
            fengqudb::Store store(path.string());
            text_tests(store);
            vector_tests(store);
            checkpoint = boundary_tests(store);
        }
        {
            fengqudb::Store store(path.string());
            idx::Client index(store, "vectors", "all");
            idx::Query q;
            q.operation = idx::Query::Stats;
            expect(index.query(store.snapshot(), q).vector_count == 4080,
                   "extended data survives reopen");
            idx::Client pending(store, "quantization", "p");
            idx::MaintenanceOptions repair;
            repair.operation = idx::MaintenanceOptions::RebuildGraph;
            repair.max_records = 100;
            auto old_graph = store.snapshot();
            idx::Query nearest;
            nearest.approximate = true;
            nearest.vector_space = "m";
            nearest.vector = {0, 1};
            nearest.candidate_budget = 1200;
            auto old_ids = ids(pending.query(old_graph, nearest));
            for (unsigned count = 0;; ++count) {
                auto tx = store.transaction();
                auto state = pending.maintain(tx, repair);
                commit(tx);
                expect(count < 12, "graph repair resumes and completes");
                if (!count)
                    expect(state.next_id > checkpoint, "reopen resumes after committed checkpoint");
                if (state.finished)
                    break;
            }
            expect(pending.query(store.snapshot(), nearest).hits.front().id == "near10005",
                   "writes during shadow build reach published graph");
            idx::MaintenanceOptions gc;
            gc.operation = idx::MaintenanceOptions::CollectGraphs;
            gc.max_records = 256;
            uint64_t reclaimed = 0;
            for (unsigned count = 0;; ++count) {
                auto tx = store.transaction();
                auto state = pending.maintain(tx, gc);
                commit(tx);
                reclaimed += state.reclaimed;
                expect(count < 20, "bounded graph garbage collection");
                if (state.finished)
                    break;
            }
            expect(reclaimed >= 1101, "obsolete graph keys reclaimed");
            expect(ids(pending.query(old_graph, nearest)) == old_ids,
                   "old snapshot survives graph publication and collection");
        }
        std::filesystem::remove_all(path);
        std::cout << "PASS extended index " << checks << " checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << " fixture=" << path << '\n';
        return 1;
    }
}
