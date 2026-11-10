#include <fengqudb_store.hpp>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        fengqudb::StoreOptions options;
        options.segmented = true;
        auto db = fengqudb::Store::open_at(argv[1], "main", options);
        auto old = db.snapshot();
        fengqudb::wire::Writer encoded;
        encoded.fixed64_field(1, 42);
        encoded.bytes_field(2, "hello");
        auto tx = db.transaction();
        tx.put("item", encoded.data());
        auto result = tx.commit();
        if (!result || old.get("item"))
            return 2;
        auto value = db.get("item");
        if (!value || value->view() != encoded.data())
            return 3;
        db.sync();
        db.verify();
        std::cout << "PASS installed C++17 Store, segmented options, transaction and snapshot\n";
    } catch (const fengqudb::Error &error) {
        std::cerr << error.what() << '\n';
        return 4;
    }
}
