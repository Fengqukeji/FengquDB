#include "fengqudb_index.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static_assert(static_cast<unsigned>(fengqudb::index::MaintenanceOptions::DropPartition) == 5,
              "retain the published Index maintenance contract");
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("fqdb-sdk-version-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check(fs::create_directory(root), "fixture collision");
        {
            const auto encoded = (root / "main").u8string();
            const std::string path(reinterpret_cast<const char *>(encoded.data()), encoded.size());
            fengqudb::Store db(path);
            const auto initial = db.version();
            auto original = db.snapshot();
            auto transaction = db.transaction();
            check(original.version() == initial && transaction.version() == initial,
                  "snapshot/transaction did not capture the published version");
            transaction.put("one", "value");
            auto committed = transaction.commit(true);
            check(bool(committed) && db.version() == committed.version && db.version() > initial,
                  "Store version did not advance to the accepted commit");
            check(original.version() == initial, "old snapshot version advanced");
            auto latest = db.snapshot();
            auto base = db.transaction();
            const auto captured = base.version();
            db.put("two", "other", 0, true);
            check(db.version() > captured && base.version() == captured &&
                      latest.version() == captured,
                  "captured versions followed a later Store commit");
            // Compile and call the Index C++ facade against the packaged
            // bridge-enabled Core, with no source-tree library or header.
            fengqudb::index::Client index(db, "sdk-smoke", "public");
            fengqudb::index::Query query;
            query.text = "absent";
            auto result = index.query(latest, query);
            check(result.hits.empty(), "empty index unexpectedly matched");
        }
        fs::remove_all(root);
        std::cout << "PASS binary-only C++ SDK Store/snapshot/transaction version and Index bridge\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "; fixture=" << root << '\n';
        return 1;
    }
}
