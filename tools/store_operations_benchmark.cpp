#include "fengqudb_store.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;
static uint64_t checksum = 0;
template <class F> void measure(const char *name, size_t count, size_t values, F operation) {
    std::vector<double> samples;
    for (unsigned round = 0; round < 5; ++round) {
        auto start = Clock::now();
        for (size_t i = 0; i < count; ++i)
            operation();
        samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
                          (count * values));
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"workload\":\"" << name << "\",\"ns_per_value\":" << samples[2]
              << ",\"values_per_second\":" << 1e9 / samples[2] << "}\n";
}
static void check(bool condition) {
    if (!condition)
        throw std::runtime_error("Store operations benchmark mismatch");
}
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("usage: fqdb_store_operations_bench NEW_DIRECTORY");
        auto root = std::filesystem::u8path(argv[1]);
        check(std::filesystem::create_directory(root));
        fengqudb::StoreOptions options;
        options.durability = fengqudb::Durability::buffered;
        auto db = fengqudb::Store::open_at(root.u8string(), "main", options);
        std::string value(256, 'v');
        std::vector<std::string> keys;
        for (unsigned i = 0; i < 32; ++i)
            keys.push_back("key/" + std::to_string(i));
        std::vector<std::string_view> inputs;
        auto seed = db.transaction();
        for (const auto &key : keys) {
            seed.put(key, value);
            inputs.push_back(key);
        }
        check(bool(seed.commit()));
        db.checkpoint();
        for (const auto &key : keys)
            check(db.get(key)->view() == value);
        auto prepared = db.prepare_key(keys[0]);
        char buffer[256];
        uint64_t required = 0;
        measure("get_view_256B", 100000, 1, [&] { checksum += db.get(keys[0])->view().size(); });
        measure("prepared_get_into_256B", 100000, 1, [&] {
            check(prepared.get_into(buffer, sizeof(buffer), required));
            checksum += required;
        });
        measure("get_32_individual", 4000, 32, [&] {
            for (auto key : inputs)
                checksum += db.get(key)->view().size();
        });
        measure("get_32_batch", 4000, 32, [&] {
            auto result = db.get_many(inputs);
            for (uint32_t i = 0; i < result.size(); ++i)
                checksum += result[i].value->size();
        });
        std::string large(65536, 'L');
        auto batch = db.prepare_batch({{"large", large}});
        measure("put_64KiB", 100, 1, [&] {
            auto tx = db.transaction();
            tx.put("large", large);
            check(bool(tx.commit()));
        });
        measure("prepared_put_64KiB", 100, 1, [&] { db.put_batch(batch); });
        db.sync();
        db.verify();
        check(db.get("large")->view() == large && required == sizeof(buffer) && checksum != 0);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
