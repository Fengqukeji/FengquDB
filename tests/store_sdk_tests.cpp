#include "fengqudb_store.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
extern "C" int fqdb_store_c_header_smoke(void);
static void check(bool value) {
    if (!value)
        throw std::runtime_error("Store shared SDK test failed");
}
int main() {
    try {
        check(fqdb_store_c_header_smoke() == 0);
        auto defaults = fengqudb::StoreWriteOptions{};
        check(defaults.mode == fengqudb::StoreWriteMode::Performance && defaults.period_ms == 50 &&
              defaults.threshold_bytes == 1048576 && defaults.native().period_ms == 50);
        auto path = std::filesystem::temp_directory_path() /
                    ("fqdb-store-sdk-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(path));
        {
            fengqudb::Store store(path.u8string());
            check(store.write_options().mode == fengqudb::StoreWriteMode::Performance &&
                  store.write_options().period_ms == 50);
            auto pending = store.submit_put("hello", "initial");
            store.sync();
            check(bool(pending.wait()));
            store.configure_write({fengqudb::StoreWriteMode::Reliable, 100, 1048576});
            auto tx = store.transaction();
            tx.put("hello", "world");
            tx.put("", "");
            check(bool(tx.commit()));
            auto snapshot = store.snapshot();
            auto value = snapshot.get("hello");
            check(value && value->view() == "world" && store.get("")->view().empty());
            auto pointer = value->view().data();
            auto again = store.get("hello");
            check(again && again->view().data() == pointer);
            auto write = store.transaction();
            write.patch("hello", 0, 5, "updated");
            check(bool(write.commit()));
            check(value->view() == "world" && store.get("hello")->view() == "updated");
            check(store.purge_expired().removed_keys == 0);
            store.checkpoint();
            auto version = store.info().version;
            check(store.compact().final_version == version && store.verify().stored_keys == 2);
            check(snapshot.get("hello")->view() == "world");
            store.execute({FQDB_HASH_SET, "sdk", "field", "first"});
            auto hash = store.hash_get("sdk", "field");
            store.execute({FQDB_HASH_SET, "sdk", "field", "second"});
            check(hash && hash->view() == "first" &&
                  store.hash_get("sdk", "field")->view() == "second");
        }
        {
            fengqudb::StoreOptions options;
            options.max_snapshots = 1;
            fengqudb::Store reopened(path.u8string(), options);
            check(reopened.get("hello")->view() == "updated");
            auto only_snapshot = reopened.snapshot();
            // Direct one-shot reads, including Hash, need no snapshot slot.
            check(reopened.hash_get("sdk", "field")->view() == "second");
            check(!reopened.hash_get("sdk", "missing") && !reopened.hash_get("absent", "field"));
            check(only_snapshot.get("hello")->view() == "updated");
        }
        // Every entry is a known fixture file; refuse cleanup if anything else exists.
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(path)) {
            auto name = entry.path().filename().u8string();
            check(
                entry.is_regular_file() &&
                (name == "CURRENT" || name == "LOCK" || name == "WRITE" ||
                 (name.size() == 57 && name.substr(0, 4) == "gen-" && name.substr(53) == ".fdb") ||
                 (name.size() == 21 && name[0] == 'g' && name.substr(17) == ".fdb")));
            files.push_back(entry.path());
        }
        for (const auto &file : files)
            check(std::filesystem::remove(file));
        check(std::filesystem::remove(path));
        std::cout << "PASS shared Store C ABI and C++ SDK, immutable views and restart\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
