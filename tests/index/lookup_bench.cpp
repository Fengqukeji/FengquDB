// Minimal A/B: same matching IDs, native inverted lookup vs unindexed metadata
// scan. This measures candidate positioning, not ANN quality or other engines.
#include "fengqudb_index.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>

namespace idx = fengqudb::index;
using Clock = std::chrono::steady_clock;
int main() {
    auto path = std::filesystem::current_path() /
                ("index-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
    try {
        {
            fengqudb::Store store(path.string());
            idx::Client index(store, "benchmark", "one");
            {
                auto tx = store.transaction();
                for (unsigned i = 0; i < 2400; ++i) {
                    idx::Document d;
                    d.id = std::to_string(10000 + i);
                    bool match = i % 100 == 0;
                    d.texts = {
                        {"body",
                         match ? "needle storage engine native" : "common storage engine native",
                         1}};
                    d.metadata = {{"selected", idx::Value::boolean(match)}};
                    index.upsert(tx, d);
                }
                if (!tx.commit(true))
                    throw std::runtime_error("fixture commit failed");
            }
            auto view = store.snapshot();
            idx::Query inverted;
            inverted.text = "needle";
            idx::Query scanning;
            scanning.operation = idx::Query::List;
            scanning.where = idx::Filter::eq("selected", idx::Value::boolean(true));
            auto a = index.query(view, inverted), b = index.query(view, scanning);
            if (a.hits.size() != b.hits.size())
                throw std::runtime_error("A/B count mismatch");
            for (size_t i = 0; i < a.hits.size(); ++i)
                if (a.hits[i].id != b.hits[i].id)
                    throw std::runtime_error("A/B IDs mismatch");
            std::vector<double> inverted_us, scan_us;
            for (unsigned i = 0; i < 90; ++i) {
                auto run = [&](const idx::Query &q, std::vector<double> &samples) {
                    auto begin = Clock::now();
                    auto result = index.query(view, q);
                    auto us =
                        std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
                    if (result.hits.size() != 8)
                        throw std::runtime_error("unexpected hit count");
                    if (i >= 10)
                        samples.push_back(us);
                };
                if (i % 2) {
                    run(inverted, inverted_us);
                    run(scanning, scan_us);
                } else {
                    run(scanning, scan_us);
                    run(inverted, inverted_us);
                }
            }
            std::sort(inverted_us.begin(), inverted_us.end());
            std::sort(scan_us.begin(), scan_us.end());
            std::cout << std::fixed << std::setprecision(2)
                      << "{\"records\":2400,\"matched\":24,\"top_k\":8,\"samples\":80,"
                      << "\"inverted_median_us\":" << inverted_us[40]
                      << ",\"scan_median_us\":" << scan_us[40]
                      << ",\"inverted_p95_us\":" << inverted_us[76]
                      << ",\"scan_p95_us\":" << scan_us[76]
                      << ",\"median_speedup\":" << scan_us[40] / inverted_us[40]
                      << ",\"inverted_records_read\":" << a.records_scanned
                      << ",\"scan_records_read\":" << b.records_scanned << "}\n";
            idx::Query common;
            common.text = "storage";
            common.candidate_budget = 3000;
            auto uncompacted = index.query(view, common);
            for (;;) {
                auto tx = store.transaction();
                auto state = index.maintain(tx);
                if (!tx.commit(true))
                    throw std::runtime_error("compaction commit");
                if (state.finished)
                    break;
            }
            auto compacted_view = store.snapshot();
            auto compacted = index.query(compacted_view, common);
            for (size_t i = 0; i < uncompacted.hits.size(); ++i)
                if (uncompacted.hits[i].id != compacted.hits[i].id ||
                    uncompacted.hits[i].score != compacted.hits[i].score)
                    throw std::runtime_error("block-max A/B mismatch");
            std::vector<double> plain_us, block_us;
            for (unsigned i = 0; i < 50; ++i) {
                auto run = [&](const auto &snapshot, std::vector<double> &samples) {
                    auto begin = Clock::now();
                    auto result = index.query(snapshot, common);
                    if (result.hits.size() != 8)
                        throw std::runtime_error("block-max hit count");
                    if (i >= 10)
                        samples.push_back(
                            std::chrono::duration<double, std::micro>(Clock::now() - begin)
                                .count());
                };
                if (i % 2) {
                    run(view, plain_us);
                    run(compacted_view, block_us);
                } else {
                    run(compacted_view, block_us);
                    run(view, plain_us);
                }
            }
            std::sort(plain_us.begin(), plain_us.end());
            std::sort(block_us.begin(), block_us.end());
            std::cout << "{\"case\":\"common_term_block_max\",\"records\":2400,\"top_k\":8,"
                         "\"samples\":40,"
                      << "\"plain_median_us\":" << plain_us[20]
                      << ",\"block_median_us\":" << block_us[20]
                      << ",\"median_speedup\":" << plain_us[20] / block_us[20]
                      << ",\"plain_records_read\":" << uncompacted.records_scanned
                      << ",\"block_records_read\":" << compacted.records_scanned << "}\n";
        }
        std::filesystem::remove_all(path);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
