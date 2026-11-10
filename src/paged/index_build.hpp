#pragma once
#include "features.hpp"
#include "maintenance.hpp"

namespace fengqudb::paged {
inline void commit_retryable(Core &core, Transaction &tx, MaintenanceJob &job) {
    core.commit_checked(tx, nullptr, [&] { job.check(); });
}
inline void clean_index_kind(Core &core, const Bytes &name, const char *kind, MaintenanceJob &job) {
    auto prefix = system_prefix() + kind + component(name);
    for (;;) {
        Transaction tx(core.shared_from_this(), 0);
        enable_features(tx);
        auto rows = feature_scan(tx, prefix, 32);
        if (rows.empty())
            return;
        for (const auto &row : rows)
            tx.erase(row.key);
        commit_retryable(core, tx, job);
    }
}
inline void clean_index(Core &core, const Bytes &name, MaintenanceJob &job) {
    clean_index_kind(core, name, "I", job);
    clean_index_kind(core, name, "J", job);
}
inline fqdb_store_maintenance_info build_index(Core &core, IndexRule rule, bool drop,
                                               const fqdb_store_maintenance_options &options) {
    MaintenanceJob job(core, options);
    auto schema_key = catalog_prefix() + component(rule.name);
    auto progress_key = system_prefix() + "B" + component(rule.name);
    fqdb_store_maintenance_info info{};
    info.snapshot_version = core.current()->version;
    {
        Transaction tx(core.shared_from_this(), 0);
        enable_features(tx);
        auto existing = maybe_get(tx, schema_key);
        if (drop) {
            require(bool(existing), FQDB_NOT_FOUND);
            tx.erase(schema_key);
            tx.erase(progress_key);
            if (feature_scan(tx, catalog_prefix(), 65).empty())
                tx.erase(catalog_epoch());
            else
                tx.put(catalog_epoch(), "", 0);
            commit_retryable(core, tx, job);
            clean_index(core, rule.name, job);
            info.final_version = core.current()->version;
            return info;
        }
        if (existing) {
            IndexRule old(**existing);
            require(old.prefix == rule.prefix && old.field == rule.field && old.type == rule.type &&
                        old.flags == rule.flags && old.parts == rule.parts,
                    FQDB_ALREADY_EXISTS);
            if (old.ready) {
                info.final_version = core.current()->version;
                return info;
            }
        } else {
            require(feature_scan(tx, catalog_prefix(), 65).size() < 64, FQDB_LIMIT);
            // A crashed prior drop may have left unreachable rows of this name.
            clean_index(core, rule.name, job);
            tx.erase(progress_key);
            tx.put(schema_key, rule.encode(), 0);
            tx.put(catalog_epoch(), "", 0);
            commit_retryable(core, tx, job);
        }
    }
    auto captured = job.capture();
    maintenance_at(45);
    std::optional<Bytes> resume;
    if (auto ref = captured->view->find(progress_key); ref && !ref->deleted()) {
        auto bytes = captured->view->generation->value(*ref, captured->view->limit);
        require(!bytes->empty() && (*bytes)[0] == 1);
        resume = bytes->substr(1);
    }
    ViewCursor cursor(*captured->view,
                      resume ? std::string_view(*resume) : std::string_view(rule.prefix));
    std::vector<Bytes> keys;
    uint64_t bytes = 0;
    auto flush = [&] {
        if (keys.empty())
            return;
        for (unsigned retry = 0;; ++retry) {
            job.check();
            Transaction tx(core.shared_from_this(), 0);
            enable_features(tx);
            tx.depend(catalog_epoch());
            for (const auto &key : keys) {
                auto value = maybe_get(tx, key);
                if (!value)
                    continue;
                auto reference = tx.base->view->find(key);
                index_one(tx, rule, key, {}, 0, std::string_view(**value), reference->expires);
            }
            tx.put(progress_key, Bytes(1, 1) + keys.back(), 0);
            try {
                commit_retryable(core, tx, job);
                break;
            } catch (const Error &error) {
                if (error.status != FQDB_CONFLICT || retry >= 31)
                    throw;
            }
        }
        maintenance_at(46);
        info.copied_keys += keys.size();
        keys.clear();
        bytes = 0;
    };
    while (auto row = cursor.next()) {
        job.check();
        if (row->key.substr(0, rule.prefix.size()) != rule.prefix)
            break;
        if (resume && compare(row->key, *resume) <= 0)
            continue;
        if (system_key(row->key) || row->value.deleted())
            continue;
        if (!keys.empty() &&
            (keys.size() == 32 || bytes + row->key.size() * 4 + 512 > core.options.delta_bytes / 4))
            flush();
        keys.emplace_back(row->key);
        bytes += row->key.size() * 4 + 512;
    }
    flush();
    Transaction finish(core.shared_from_this(), 0);
    enable_features(finish);
    rule.ready = 1;
    finish.erase(progress_key);
    finish.put(schema_key, rule.encode(), 0);
    finish.put(catalog_epoch(), "", 0);
    commit_retryable(core, finish, job);
    info.final_version = core.current()->version;
    return info;
}
} // namespace fengqudb::paged
