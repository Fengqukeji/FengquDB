#include <fengqudb_diagnostics.hpp>
#include <fengqudb_store.hpp>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        std::string file = argv[1];
        fengqudb::DiagnosticsOptions options;
        options.min_level = FQDB_LOG_INFO;
        fengqudb::Diagnostics log(file + ".logs", options);
        fengqudb::Store db(file);
        db.configure_write({fengqudb::StoreWriteMode::Reliable, 100,
                            1048576}); // This consumer exercises synchronous calls.
        db.put_if_changed("key", "value");
        if (!log.flush())
            return 2;
        auto status = log.status();
        if (status.empty())
            return 3;
        std::cout << "PASS installed diagnostics C++17 consumer\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
