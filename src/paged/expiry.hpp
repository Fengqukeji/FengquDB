#pragma once
#include "maintenance.hpp"

namespace fengqudb::paged {
inline fqdb_store_expiry_options expiry_defaults() {
    return {sizeof(fqdb_store_expiry_options), 1, 1024, 0, 0, 1048576};
}
// Expiry summaries live on the existing COW paths. No second all-key index,
// value decoding or hot-path cursor callback is needed for collection.
class ExpiryCursor {
    struct Level {
        std::shared_ptr<const Page> page;
        uint64_t offset;
        size_t at = 0;
    };
    const View &view_;
    std::optional<std::string_view> after_;
    bool from_disk_ = false;
    uint64_t cutoff_, pending_, ceiling_, visited_ = 0;
    std::function<void()> check_;
    std::vector<Level> disk_;
    std::vector<const DeltaNode *> memory_;
    void descend(const DeltaNode *node) {
        while (node && node->earliest <= cutoff_) {
            check_();
            memory_.push_back(node);
            node = node->left.get();
        }
    }

  public:
    ExpiryCursor(const View &view, uint64_t cutoff, std::function<void()> check,
                 std::optional<std::string_view> after = {})
        : view_(view), after_(after), cutoff_(cutoff), pending_(view.checkpoint.root),
          ceiling_(view.limit), check_(std::move(check)) {
        disk_.reserve(64);
        memory_.reserve(view.delta ? view.delta->key.height + 1 : 1);
        descend(view.delta.get());
    }
    uint64_t visited_pages() const noexcept {
        return visited_;
    }
    bool from_disk() const noexcept {
        return from_disk_;
    }
    std::optional<Metadata> next() {
        from_disk_ = false;
        while (!memory_.empty()) {
            check_();
            auto node = memory_.back();
            memory_.pop_back();
            descend(node->right.get());
            if (node->value.expired(cutoff_))
                return Metadata{node->key.view(), node->value};
        }
        from_disk_ = true;
        for (;;) {
            check_();
            if (pending_) {
                require(disk_.size() < 64 && pending_ < ceiling_);
                auto offset = pending_;
                pending_ = 0;
                auto page = view_.generation->page(offset, ceiling_);
                ++visited_;
                if (page->earliest <= cutoff_) {
                    size_t at = after_ ? page->lower(*after_) : 0;
                    if (after_) {
                        if (page->leaf) {
                            if (at < page->slots.size() &&
                                compare(page->slots[at].key, *after_) == 0)
                                ++at;
                        } else if (at && (at == page->slots.size() ||
                                          compare(page->slots[at].key, *after_) > 0))
                            --at;
                    }
                    disk_.push_back({std::move(page), offset, at});
                }
            }
            if (disk_.empty())
                return {};
            auto &level = disk_.back();
            if (level.at == level.page->slots.size()) {
                disk_.pop_back();
                continue;
            }
            const auto &slot = level.page->slots[level.at++];
            if (level.page->leaf) {
                // A non-expired replacement/deletion in delta must still hide
                // an expired disk row, even though it was pruned above.
                if (slot.value.expired(cutoff_) && !delta_find(view_.delta, slot.key))
                    return Metadata{slot.key, slot.value};
            } else if (slot.earliest <= cutoff_) {
                pending_ = slot.child;
                ceiling_ = level.offset;
            }
        }
    }
};
inline fqdb_store_expiry_info Core::purge_expired(const fqdb_store_expiry_options &settings,
                                                  fqdb_log_event *trace) {
    auto maintenance_settings = maintenance_defaults();
    maintenance_settings.timeout_ms = settings.timeout_ms;
    MaintenanceJob job(*this, maintenance_settings);
    auto base = job.capture();
    Transaction transaction(base, settings.timeout_ms);
    const bool resuming = expiry_resume_valid && expiry_generation.lock() == base->view->generation;
    ExpiryCursor cursor(
        *base->view, base->time, [&] { job.check(); },
        resuming ? std::optional<std::string_view>(expiry_resume) : std::nullopt);
    Bytes next_resume = resuming ? expiry_resume : Bytes{};
    bool next_valid = resuming;
    fqdb_store_expiry_info info{};
    info.cutoff_ms = base->time;
    info.version = base->view->version;
    uint64_t bytes = 0;
    while (auto row = cursor.next()) {
        auto cost = row->key.size() * 3 + sizeof(DeltaNode) + sizeof(WriteNode) + 512;
        if (info.removed_keys == settings.max_keys || cost > settings.max_batch_bytes - bytes) {
            require(info.removed_keys != 0, FQDB_LIMIT);
            info.more = 1;
            break;
        }
        if (cursor.from_disk()) {
            next_resume.assign(row->key);
            next_valid = true;
        }
        bytes += cost;
        transaction.erase(Bytes(row->key));
        ++info.removed_keys;
        info.removed_value_bytes += row->value.size;
    }
    if (!info.more) {
        // Finish the suffix, then revisit the prefix on the next call. New
        // expiry/updates/checkpoints behind the continuation cannot be lost.
        info.more = resuming ? 1 : 0;
        next_valid = false;
        next_resume.clear();
    }
    info.visited_pages = cursor.visited_pages();
    maintenance_at(30);
    job.check();
    // Normal OCC protects concurrent replacement, expiry extension and clear.
    // Checks are compiled away for ordinary commit callers; only this path
    // observes maintenance cancellation while waiting for the writer.
    info.version = commit_checked(transaction, trace, [&] { job.check(); });
    expiry_resume.swap(next_resume); // no allocations after a successful commit
    expiry_resume_valid = next_valid;
    expiry_generation = base->view->generation;
    if (trace) {
        trace->version = info.version;
        trace->operation_count = info.removed_keys;
    }
    return info;
}
} // namespace fengqudb::paged
