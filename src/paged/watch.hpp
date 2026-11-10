#pragma once
#include "history.hpp"

namespace fengqudb::paged {
inline fqdb_store_watch_options watch_defaults() {
    return {sizeof(fqdb_store_watch_options), 1, 0, 256, FQDB_MAX_TRANSACTION_BYTES};
}
struct ChangeBatch {
    struct Item {
        Bytes key;
        Value value;
        Ref ref;
    };
    std::shared_ptr<Core> core;
    Charge charge;
    uint64_t maximum, payload = 0;
    std::vector<Item> items;
    std::vector<fqdb_store_change_row> rows;
    fqdb_store_change_batch_info info{};
    explicit ChangeBatch(std::shared_ptr<Core> owner, uint64_t limit, uint32_t flags)
        : core(std::move(owner)), charge(core->pin_budget, {}, sizeof(ChangeBatch) + 128),
          maximum(limit) {
        info.flags = flags;
    }
    void account(uint64_t bytes, size_t capacity) {
        auto total = sizeof(ChangeBatch) + 128 + bytes +
                     capacity * (sizeof(Item) + sizeof(fqdb_store_change_row));
        require(total <= maximum, FQDB_LIMIT);
        charge.resize(total);
    }
    void add(WalOperation &operation, Generation &generation, uint64_t limit) {
        auto next = items.size() + 1;
        require(next <= UINT32_MAX, FQDB_LIMIT);
        auto capacity =
            next > items.capacity() ? std::max<size_t>(8, items.capacity() * 2) : items.capacity();
        const bool value = !operation.result.deleted() && !(info.flags & FQDB_WATCH_METADATA_ONLY);
        const auto key_bytes = operation.key.capacity() + 1;
        // Charge before allocating vectors or materializing a historical value.
        auto allowance = value ? sizeof(Data) + 64 + 2 * operation.result.size : 0;
        account(payload + key_bytes + allowance, capacity);
        items.reserve(capacity);
        rows.reserve(capacity);
        Value bytes;
        if (value)
            bytes = generation.value(operation.result, limit, false, false);
        auto retained = key_bytes + (bytes ? sizeof(Data) + 64 + bytes->capacity() : 0);
        account(payload + retained, std::max(items.capacity(), rows.capacity()));
        items.push_back({std::move(operation.key), std::move(bytes), operation.result});
        payload += retained;
    }
    void finish() {
        for (const auto &item : items)
            rows.push_back(
                {{item.key.data(), item.key.size()},
                 {item.value ? item.value->data() : nullptr, item.value ? item.value->size() : 0},
                 item.ref.expires,
                 static_cast<uint32_t>(item.ref.deleted() ? FQDB_DELETE : FQDB_PUT),
                 0});
        info.rows = rows.data();
        info.count = static_cast<uint32_t>(rows.size());
    }
};
class Watch {
    std::shared_ptr<Core> core_;
    Charge count_, pins_;
    std::shared_ptr<WatchHub> hub_;
    std::shared_ptr<WatchLease> lease_ = std::make_shared<WatchLease>();
    Bytes prefix_;
    fqdb_store_watch_options options_;
    std::mutex mutex_;
    HistoryReader::Position position_, pending_position_;
    std::shared_ptr<ChangeBatch> pending_;
    void ready() const {
        require(!lease_->cancelled.load(std::memory_order_acquire), FQDB_CANCELLED);
    }
    static std::shared_ptr<const View> history_view(const std::shared_ptr<const View> &source) {
        auto view = std::make_shared<View>();
        view->generation = source->generation;
        view->reader_epoch = source->reader_epoch;
        view->history = source->history;
        view->checkpoint = source->checkpoint;
        view->version = source->version;
        view->durable = source->durable;
        view->limit = source->limit;
        return view; // No persistent index/delta roots need to be pinned by a WAL reader.
    }

  public:
    Watch(std::shared_ptr<Core> core, uint64_t from, Bytes prefix, fqdb_store_watch_options options)
        : core_(std::move(core)), count_(core_->snapshot_budget, {}, 1),
          pins_(core_->pin_budget, {},
                sizeof(Watch) + sizeof(WatchLease) + sizeof(View) + prefix.capacity() + 256),
          prefix_(std::move(prefix)), options_(options) {
        std::lock_guard<WriterGate> lock(core_->writer);
        auto current = core_->current();
        if (from == FQDB_HISTORY_CURRENT)
            from = current->version;
        HistoryReader reader(history_view(current), from);
        position_ = reader.position();
        lease_->version.store(from, std::memory_order_relaxed);
        hub_ = core_->watches.lock();
        if (!hub_)
            hub_ = std::make_shared<WatchHub>(core_->pin_budget);
        uint64_t minimum = current->version, active = 0;
        hub_->retain(minimum, active);
        require(active < 64, FQDB_LIMIT);
        hub_->readers.push_back(lease_);
        core_->watches = hub_;
    }
    std::shared_ptr<ChangeBatch> next(uint64_t timeout) {
        std::unique_lock<std::mutex> operation(mutex_, std::try_to_lock);
        require(operation.owns_lock(), FQDB_LOCKED);
        ready();
        require(timeout <= static_cast<uint64_t>(INT64_MAX / 1000000), FQDB_INVALID_ARGUMENT);
        auto start = std::chrono::steady_clock::now();
        auto delay = std::chrono::milliseconds(timeout);
        require(delay <= std::chrono::steady_clock::time_point::max() - start,
                FQDB_INVALID_ARGUMENT);
        auto deadline = start + delay;
        if (pending_)
            return pending_;
        auto check = [&] {
            ready();
            require(!timeout || std::chrono::steady_clock::now() < deadline, FQDB_TIMEOUT);
        };
        for (;;) {
            ready();
            auto current = core_->current();
            if (current->version > position_.version) {
                HistoryReader reader(history_view(current), position_.version, &position_);
                auto result =
                    std::make_shared<ChangeBatch>(core_, options_.max_batch_bytes, options_.flags);
                for (uint32_t scan = 0; scan < options_.max_scan_commits; ++scan) {
                    auto record = reader.next(check);
                    if (!record)
                        break;
                    maintenance_at(60);
                    uint64_t features = 0;
                    result->info.unix_ms = read_wal(
                        *record->generation, record->frame,
                        [&](WalOperation &item) {
                            check();
                            if (!(options_.flags & FQDB_WATCH_INCLUDE_SYSTEM) && (features & 1) &&
                                system_key(item.key))
                                return;
                            if (item.key.size() >= prefix_.size() &&
                                item.key.compare(0, prefix_.size(), prefix_) == 0)
                                result->add(item, *record->generation, record->frame.end);
                        },
                        &features);
                    if (!result->items.empty())
                        break;
                }
                check();
                result->info.version = reader.version();
                result->info.durable_version = current->durable;
                result->finish();
                pending_position_ = reader.position();
                pending_ = std::move(result);
                return pending_;
            }
            if (!timeout)
                return {};
            std::unique_lock<std::mutex> lock(hub_->mutex);
            if (!hub_->changed.wait_until(lock, deadline, [&] {
                    return lease_->cancelled.load(std::memory_order_acquire) ||
                           core_->current()->version > position_.version;
                }))
                throw Error(FQDB_TIMEOUT);
        }
    }
    void ack(uint64_t version) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        require(lock.owns_lock(), FQDB_LOCKED);
        ready();
        if (version == position_.version)
            return;
        require(pending_ && version == pending_->info.version, FQDB_INVALID_ARGUMENT);
        position_ = pending_position_;
        lease_->version.store(version, std::memory_order_release);
        pending_.reset();
    }
    void cancel() {
        lease_->cancelled.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(hub_->mutex);
        hub_->changed.notify_all();
    }
};
} // namespace fengqudb::paged
