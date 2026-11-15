#include "fengqudb_store.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

using Clock = std::chrono::steady_clock;
static void check(bool condition) {
    if (!condition)
        throw std::runtime_error("Store benchmark data check failed");
}
static uint64_t peak_private_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    check(GetProcessMemoryInfo(GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                               sizeof(memory)) != 0);
    return memory.PeakPagefileUsage;
#else
    return 0;
#endif
}
template <class F> static void measure(const char *name, size_t count, F &&operation) {
    std::vector<double> samples;
    samples.reserve(count);
    auto start = Clock::now();
    for (size_t i = 0; i < count; ++i) {
        auto before = Clock::now();
        operation(i);
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - before).count());
    }
    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"workload\":\"" << name << "\",\"operations\":" << count
              << ",\"ops_per_second\":" << count / elapsed
              << ",\"p99_us\":" << samples[std::min(count - 1, count * 99 / 100)]
              << ",\"peak_private_bytes\":" << peak_private_bytes() << "}\n";
}
static void cleanup(const std::filesystem::path &path) {
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
        auto name = entry.path().filename().u8string();
        check(!entry.is_symlink() && entry.is_regular_file() &&
              (name == "CURRENT" || name == "LOCK" ||
               (name.size() == 57 && name.substr(0, 4) == "gen-" && name.substr(53) == ".fdb") ||
               (name.size() == 21 && name[0] == 'g' &&
                (name.substr(17) == ".fdb" || name.substr(17) == ".blk"))));
        files.push_back(entry.path());
    }
    for (const auto &file : files)
        check(std::filesystem::remove(file));
    check(std::filesystem::remove(path));
}
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("usage: fqdb_store_bench NEW_DIRECTORY");
        auto root = std::filesystem::u8path(argv[1]);
        check(std::filesystem::create_directory(root));
        auto path = root / "store";
        std::string payload(256 * 1024, 'x'), small(64, 's'), scan_value(64 * 1024, 'r');
        fengqudb::StoreOptions options;
        options.durability = fengqudb::Durability::buffered;
        options.cache_bytes = 8 * 1024 * 1024;
        std::cout << std::fixed << std::setprecision(3);
        {
            fengqudb::Store store(path.u8string(), options);
            {
                auto tx = store.transaction();
                tx.put("raw", small);
                for (unsigned i = 0; i < 512; ++i)
                    tx.execute({FQDB_HASH_SET, "small", std::to_string(i), small});
                for (unsigned i = 0; i < 16; ++i)
                    tx.execute({FQDB_HASH_SET, "scan-large", std::to_string(i), scan_value});
                tx.execute({FQDB_HASH_SET, "large", "field", payload});
                check(bool(tx.commit()));
            }
            // Warm page/value caches before measuring the hot read paths.
            check(store.hash_get("small", "1")->view() == small);
            check(store.get("raw")->view() == small);
            measure("raw_get_hit", 500000,
                    [&](size_t) { check(store.get("raw")->view() == small); });
            measure("raw_get_miss", 200000, [&](size_t) { check(!store.get("missing")); });
            measure("hash_get_hit", 500000,
                    [&](size_t) { check(store.hash_get("small", "1")->view() == small); });
            measure("hash_get_miss", 200000,
                    [&](size_t) { check(!store.hash_get("small", "missing")); });
            measure("hash_get_missing_scope", 100000,
                    [&](size_t) { check(!store.hash_get("missing", "field")); });
            measure("hash_insert_buffered", 1000, [&](size_t i) {
                check(store.execute({FQDB_HASH_SET, "insert", std::to_string(i), small}).changed);
            });
            measure("set_add_buffered", 1000, [&](size_t i) {
                check(store.execute({FQDB_SET_ADD, "set", std::to_string(i)}).changed);
            });
            measure("hash_overwrite_256k_buffered", 128, [&](size_t i) {
                payload[0] = i % 2 ? 'a' : 'b';
                check(store.execute({FQDB_HASH_SET, "large", "field", payload}).changed);
            });
            measure("hash_get_command_256k", 1024, [&](size_t) {
                check(store.execute({FQDB_HASH_GET, "large", "field"}).value == payload);
            });
            measure("hash_scan_256", 1024, [&](size_t) {
                auto result = store.execute({FQDB_HASH_SCAN, "small"});
                check(result.rows.size() == 256 && result.count == 512);
            });
            measure("hash_scan_1m", 256, [&](size_t) {
                auto result = store.execute({FQDB_HASH_SCAN, "scan-large"});
                check(result.rows.size() == 16 && result.rows.back().value == scan_value);
            });
            measure("kv_insert_buffered", 1000, [&](size_t i) {
                auto tx = store.transaction();
                tx.put("kv/" + std::to_string(i), small);
                check(bool(tx.commit()));
            });
            // The same fixed durability is used for both binaries. BUFFERED
            // workloads above exclude this explicit final reliable flush.
            store.sync();
            check(store.execute({FQDB_COLLECTION_SIZE, "insert"}).count == 1000);
            check(store.execute({FQDB_COLLECTION_SIZE, "set"}).count == 1000);
        }
        {
            fengqudb::Store reopened(path.u8string(), options);
            check(reopened.get("kv/999")->view() == small);
            check(reopened.hash_get("large", "field")->view() == payload);
            check(reopened.execute({FQDB_COLLECTION_SIZE, "set"}).count == 1000);
            reopened.verify();
        }
        cleanup(path);
        check(std::filesystem::remove(root));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
