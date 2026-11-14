#include "fengqudb_index.hpp"
#include <iostream>

int main(int argc, char **argv) {
    try {
        // Keep one Store alive in your application's storage service.
        fengqudb::Store store(argc > 1 ? argv[1] : "index-example-data");
        fengqudb::index::Client index(store, "knowledge", "public");
        fengqudb::index::Document d;
        d.id = "manual-1";
        d.texts = {{"title", "数据库备份", 2}, {"body", "事务完成后执行备份", 1}};
        d.metadata = {{"kind", fengqudb::index::Value::text("manual")}};
        d.source_locator = "manual-1";
        auto tx = store.transaction();
        tx.put("original/manual-1", "原文或业务自己的序列化数据");
        index.upsert(tx, d);
        auto commit = tx.commit(true);
        if (!commit)
            throw fengqudb::Error(static_cast<fqdb_status>(commit.status));
        fengqudb::index::Query q;
        q.text = "备份";
        q.where = fengqudb::index::Filter::eq("kind", fengqudb::index::Value::text("manual"));
        auto snapshot = store.snapshot();
        auto result = index.query(snapshot, q);
        for (const auto &hit : result.hits)
            std::cout << hit.id << " score=" << hit.score << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
