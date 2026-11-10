#include "fengqudb_store.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <vector>

static void check(bool value) {
    if (!value)
        throw std::runtime_error("query benchmark mismatch");
}
template <class F> static void measure(const char *name, size_t calls, F operation) {
    std::vector<double> samples;
    for (unsigned run = 0; run < 5; ++run) {
        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < calls; ++i)
            operation();
        samples.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count() /
            calls);
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"workload\":\"" << name << "\",\"us_per_query\":" << samples[2]
              << ",\"queries_per_second\":" << 1e6 / samples[2] << "}\n";
}
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("usage: fqdb_store_query_bench NEW_DIRECTORY");
        auto root = std::filesystem::u8path(argv[1]);
        check(std::filesystem::create_directory(root));
        fengqudb::StoreOptions options;
        options.durability = fengqudb::Durability::buffered;
        auto db = fengqudb::Store::open_at(root.u8string(), "main", options);
        auto tx = db.transaction();
        for (unsigned i = 0; i < 4096; ++i) {
            fengqudb::wire::Writer out;
            out.uint_field(1, i % 32);
            out.uint_field(2, i / 32);
            out.bytes_field(3, std::string(512, 'p'));
            tx.put("d/" + std::to_string(i), out.data());
        }
        check(bool(tx.commit()));
        db.define_compound_index("group-score", "d/", {{1, 1}, {2, 1}}, 3);
        db.checkpoint();
        fengqudb::wire::Writer equal, lower, upper;
        equal.uint_field(1, 7);
        lower.uint_field(2, 40);
        upper.uint_field(2, 80);
        fengqudb::StoreIndexQuery query;
        query.equal = equal.data();
        query.lower = lower.data();
        query.upper = upper.data();
        auto expected = db.query_index("group-score", query);
        check(expected.rows.size() == 40);
        auto scan = [&] {
            auto snapshot = db.snapshot();
            auto cursor = snapshot.scan("d/", "d0");
            std::vector<std::pair<uint64_t, std::string>> selected;
            while (auto row = cursor.next()) {
                fengqudb::wire::Reader reader(row->value);
                fengqudb::wire::Field field;
                uint64_t group = 0, score = 0;
                while (reader.next(field)) {
                    if (field.number == 1)
                        check(reader.varint(group));
                    else if (field.number == 2)
                        check(reader.varint(score));
                    else
                        check(reader.skip(field));
                }
                check(reader.done());
                if (group == 7 && score >= 40 && score < 80)
                    selected.push_back({score, std::string(row->key)});
            }
            std::sort(selected.begin(), selected.end());
            check(selected.size() == expected.rows.size());
            for (size_t i = 0; i < selected.size(); ++i)
                check(selected[i].second == expected.rows[i].key);
        };
        scan();
        measure("full_scan_filter_4096", 100, scan);
        measure("compound_range_keys_40", 1000,
                [&] { check(db.query_index("group-score", query).rows.size() == 40); });
        query.include_values = true;
        measure("compound_range_values_40", 1000, [&] {
            auto rows = db.query_index("group-score", query);
            check(rows.rows.size() == 40 && rows.rows[0].value.size() >= 512);
        });
        query.include_values = false;
        query.fields = {1, 2};
        measure("compound_range_project_40", 1000, [&] {
            auto rows = db.query_index("group-score", query);
            check(rows.rows.size() == 40 && rows.rows[0].value.size() < 16);
        });
        db.verify();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
