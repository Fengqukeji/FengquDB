#include <fengqudb_store.hpp>
#include <iostream>
int main(int argc, char **argv) {
    try {
        fengqudb::Store db(argc > 1 ? argv[1] : "example-db");
        auto tx = db.transaction();
        tx.put("player/42", "serialized protobuf bytes");
        tx.put("player/43", "another player");
        auto pending = tx.submit();
        if (!pending.wait()) return 3; // Console example; UI code should poll asynchronously.
        auto snapshot = db.snapshot();
        auto value = snapshot.get("player/42");
        if (!value || value->view() != "serialized protobuf bytes") return 2;
        db.sync();
        std::cout << value->view() << '\n';
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
