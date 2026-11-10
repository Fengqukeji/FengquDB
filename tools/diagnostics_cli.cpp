#include "fengqudb_diagnostics.hpp"
#include <iostream>
int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: fqdb_diagnostics DIRECTORY status|query [JSON_ARGUMENTS]\n";
        return 1;
    }
    try {
        fengqudb::DiagnosticsOptions options;
        options.reader_only = true;
        fengqudb::Diagnostics log(argv[1], options);
        std::string command(argv[2]);
        if (command == "status")
            std::cout << log.status().str() << '\n';
        else if (command == "query")
            std::cout << log.query(argc > 3 ? argv[3] : "{}").str() << '\n';
        else
            return 1;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
