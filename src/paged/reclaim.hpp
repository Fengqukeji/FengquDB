#pragma once
#include "maintenance.hpp"

namespace fengqudb::paged {
struct RetiredReadEpochs {
    Charge charge;
    std::vector<std::weak_ptr<ReadEpoch>> readers;
    explicit RetiredReadEpochs(const std::shared_ptr<Budget> &budget)
        : charge(budget, {}, sizeof(*this) + 128) {}
    uint64_t allocation() const noexcept {
        return sizeof(*this) + 128 + readers.capacity() * sizeof(readers[0]);
    }
    bool expired() const noexcept {
        return std::all_of(readers.begin(), readers.end(),
                           [](const auto &reader) { return reader.expired(); });
    }
};
// Mark immutable physical references once, then punch only unreachable blocks
// below the durable floor boundary. New writes append beyond that boundary;
// patches can inherit only references reachable at it or newer appended bytes.
class ReclaimSession {
    struct Node {
        uint64_t offset = 0, ceiling = 0, maximum = 0, earliest = 0;
        unsigned depth = 0;
        std::optional<Bytes> minimum, upper;
    };
    Core &core_;
    std::shared_ptr<const View> source_;
    std::shared_ptr<const RetiredReadEpochs> old_epochs_;
    Charge pins_;
    uint64_t scratch_limit_, quantum_ = reclaim_block_bytes, queued_keys_ = 0;
    std::vector<uint64_t> marked_;
    std::vector<Node> stack_;
    Node node_;
    std::shared_ptr<const Page> page_;
    size_t slot_ = 0;
    Ref pending_;
    bool node_pending_ = false, pending_ref_ = false, started_ = false, marked_all_ = false,
         complete_ = false;
    uint64_t entries_ = 0, keys_ = 0, bytes_ = 0, sweep_ = reclaim_block_bytes;
    uint64_t range_end_ = 0;
    void check(MaintenanceJob &job) const {
        job.check();
        require(core_.reclaim_state.load(std::memory_order_acquire) != 2, FQDB_CANCELLED);
    }
    std::unique_lock<WriterGate> writer(MaintenanceJob &job) const {
        std::unique_lock<WriterGate> lock(core_.writer, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(2)))
            check(job);
        check(job);
        return lock;
    }
    static uint64_t key_memory(const Node &node) noexcept {
        return (node.minimum ? node.minimum->capacity() + 1 : 0) +
               (node.upper ? node.upper->capacity() + 1 : 0);
    }
    void account(uint64_t extra = 0) const {
        const auto used = sizeof(*this) + marked_.capacity() * sizeof(marked_[0]) +
                          stack_.capacity() * sizeof(Node) + queued_keys_ + key_memory(node_) +
                          (page_ ? page_->allocation() : 0) +
                          (old_epochs_ ? old_epochs_->allocation() : 0);
        require(used <= scratch_limit_ && extra <= scratch_limit_ - used, FQDB_LIMIT);
    }
    void push(Node node) {
        const auto key_bytes = key_memory(node);
        account(key_bytes);
        if (stack_.size() == stack_.capacity()) {
            auto capacity = std::max<size_t>(16, stack_.capacity() * 2);
            require(capacity <= scratch_limit_ / 4 / sizeof(Node), FQDB_LIMIT);
            account((capacity - stack_.capacity()) * sizeof(Node));
            stack_.reserve(capacity);
        }
        queued_keys_ += key_bytes;
        stack_.push_back(std::move(node));
        account();
    }
    void mark(uint64_t offset, uint64_t size) {
        if (!size)
            return;
        require(offset <= source_->limit && size <= source_->limit - offset);
        const auto first = offset / quantum_, last = (offset + size - 1) / quantum_;
        for (auto word = first / 64; word <= last / 64; ++word) {
            const auto low = word == first / 64 ? first % 64 : 0;
            const auto high = word == last / 64 ? last % 64 : 63;
            const auto left = UINT64_MAX << low;
            const auto right = high == 63 ? UINT64_MAX : (UINT64_C(1) << (high + 1)) - 1;
            marked_[static_cast<size_t>(word)] |= left & right;
        }
    }
    bool live(uint64_t block) const noexcept {
        return (marked_[static_cast<size_t>(block / 64)] & (UINT64_C(1) << (block % 64))) != 0;
    }
    void start(MaintenanceJob &job) {
        auto lock = writer(job);
        require(!core_.poisoned, FQDB_RECOVERY_REQUIRED);
        check(job);
        core_.sync_business_locked();
        auto old = core_.current();
        auto retention = history_pins(*old);
        if (auto hub = core_.watches.lock())
            hub->retain(retention.minimum, retention.active);
        require(!retention.active || retention.minimum == old->version, FQDB_LOCKED);
        auto generation = old->generation;
        try {
            while (!generation->file->prepare_sparse_slice()) {
                lock.unlock();
                maintenance_at(86); // No writer or file-data lock is held here.
                generation->file->sync_materialization();
                std::this_thread::yield();
                lock = writer(job);
                require(!core_.poisoned, FQDB_RECOVERY_REQUIRED);
                require(core_.current()->generation == generation, FQDB_CONFLICT);
            }
        } catch (const Error &error) {
            if (error.status == FQDB_COMMIT_UNKNOWN) {
                if (!lock.owns_lock())
                    lock = writer(job);
                core_.poisoned = true;
            }
            throw;
        }
        // Writes may have advanced while copying. Seal the latest durable view,
        // and recheck history retention before publishing a reclaim floor.
        core_.sync_business_locked();
        old = core_.current();
        retention = history_pins(*old);
        if (auto hub = core_.watches.lock())
            hub->retain(retention.minimum, retention.active);
        require(!retention.active || retention.minimum == old->version, FQDB_LOCKED);
        require(old->checkpoint.sequence <= UINT64_MAX - 2, FQDB_LIMIT);
        auto next = std::make_shared<View>(*old);
        auto epoch = std::make_shared<ReadEpoch>();
        auto retired = std::make_shared<RetiredReadEpochs>(core_.pin_budget);
        size_t surviving = 0;
        if (generation->retired_readers)
            for (const auto &reader : generation->retired_readers->readers)
                surviving += !reader.expired();
        require(surviving < scratch_limit_ / 8 / sizeof(std::weak_ptr<ReadEpoch>), FQDB_LIMIT);
        retired->readers.reserve(surviving + 1);
        if (generation->retired_readers)
            for (const auto &reader : generation->retired_readers->readers)
                if (!reader.expired())
                    retired->readers.push_back(reader);
        retired->readers.push_back(generation->reader_epoch);
        retired->charge.resize(retired->allocation());
        old_epochs_ = retired;
        pins_ = Charge(core_.pin_budget, {}, sizeof(View) + sizeof(*this) + 128);
        const auto original_tail = generation->tail;
        bool root_started = false;
        try {
            if (old->delta)
                next->checkpoint.root = fold_delta(generation, old->checkpoint.root, old->delta,
                                                   old->version, core_.options.writer_bytes);
            auto &checkpoint = next->checkpoint;
            checkpoint.version = checkpoint.floor = old->version;
            checkpoint.entries = old->entries;
            checkpoint.user_keys = old->keys;
            checkpoint.live_bytes = old->live_bytes;
            checkpoint.time = std::max(checkpoint.time, unix_ms());
            checkpoint.parent = checkpoint.parent_version = checkpoint.parent_limit =
                checkpoint.parent_legacy = checkpoint.retained_floor = 0;
            next->delta.reset();
            next->history.reset();
            const auto payload = generation->checkpoint_payload(checkpoint);
            require(payload.size() + 56 <=
                        (static_cast<uint64_t>(INT64_MAX) - generation->tail) / 2,
                    FQDB_LIMIT);
            const auto limit = generation->tail + 2 * (payload.size() + 56);
            while ((limit / quantum_ / 64 + 1) > scratch_limit_ / 8 / sizeof(uint64_t)) {
                require(quantum_ <= UINT64_MAX / 2, FQDB_LIMIT);
                quantum_ *= 2;
            }
            marked_.resize(static_cast<size_t>(limit / quantum_ / 64 + 1));
            sweep_ = quantum_;
            stack_.reserve(16);
            account();
            // The two roots are one prepaid atomic unit. No allocation or
            // cancellation point may separate them once root writes begin.
            IOWritePermit permit(
                generation->file->reservation_bytes(generation->tail, 2 * (payload.size() + 56)) +
                128);
            root_started = true;
            generation->seal_payload(checkpoint, payload, true);
            maintenance_at(74);
            generation->seal_payload(checkpoint, payload);
            maintenance_at(75);
            next->limit = generation->tail;
            next->durable = next->version;
            generation->retired_readers = std::move(retired);
            generation->reader_epoch = epoch;
            next->reader_epoch = std::move(epoch);
            core_.publish(next);
            if (old->history)
                for (const auto &segment : old->history->segments)
                    segment.generation->remove_on_close.store(true, std::memory_order_relaxed);
            source_ = std::move(next);
            started_ = true;
        } catch (...) {
            if (generation->write_failed || root_started) {
                core_.poisoned = true;
            } else if (generation->tail != original_tail) {
                try {
                    generation->file->truncate(original_tail);
                    generation->tail = original_tail;
                } catch (...) {
                    generation->write_failed = core_.poisoned = true;
                }
            }
            throw;
        }
        mark(0, data_start);
        mark(source_->checkpoint.history_frame, source_->limit - source_->checkpoint.history_frame);
        if (source_->checkpoint.root) {
            Node root;
            root.offset = source_->checkpoint.root;
            root.ceiling = source_->limit;
            push(std::move(root));
        }
    }
    void mark_references(MaintenanceJob &job) {
        while (page_ || node_pending_ || !stack_.empty()) {
            check(job);
            if (!page_) {
                if (!node_pending_) {
                    node_ = std::move(stack_.back());
                    stack_.pop_back();
                    queued_keys_ -= key_memory(node_);
                    node_pending_ = true;
                }
                require(node_.depth < 64 && node_.offset >= data_start &&
                        node_.offset < node_.ceiling);
                page_ = source_->generation->page(node_.offset, node_.ceiling);
                account();
                require(page_->frame_version <= source_->checkpoint.version &&
                        page_->maximum <= source_->checkpoint.version);
                if (node_.minimum)
                    require(page_->slots.front().key == *node_.minimum &&
                            page_->maximum == node_.maximum);
                if (node_.earliest)
                    require(page_->earliest == node_.earliest);
                if (node_.upper)
                    require(compare(page_->slots.back().key, *node_.upper) < 0);
                mark(page_->frame_offset, page_->frame_end - page_->frame_offset);
                ++visited_pages;
                slot_ = 0;
            }
            while (slot_ < page_->slots.size()) {
                check(job);
                const auto &slot = page_->slots[slot_];
                if (!page_->leaf) {
                    Node child;
                    child.offset = slot.child;
                    child.ceiling = page_->frame_offset;
                    child.depth = node_.depth + 1;
                    child.maximum = slot.maximum;
                    child.earliest = slot.earliest;
                    child.minimum = Bytes(slot.key);
                    child.upper = slot_ + 1 < page_->slots.size()
                                      ? std::optional<Bytes>(Bytes(page_->slots[slot_ + 1].key))
                                      : node_.upper;
                    push(std::move(child));
                    ++slot_;
                    continue;
                }
                if (!pending_ref_) {
                    valid_ref(slot.value, page_->frame_offset, true);
                    pending_ = slot.value;
                    pending_ref_ = true;
                }
                while (!pending_.deleted() && pending_.depth) {
                    check(job);
                    mark(pending_.offset, pending_.recipe_size);
                    auto recipe =
                        read_operation(*source_->generation->file, pending_.offset,
                                       pending_.offset + pending_.recipe_size, pending_.version);
                    require(patch_kind(recipe.kind) && recipe.result.size == pending_.size &&
                            recipe.result.depth == pending_.depth &&
                            recipe.result.crc == pending_.crc);
                    pending_ = recipe.base;
                }
                if (!pending_.deleted())
                    mark(pending_.offset, std::max<uint64_t>(1, pending_.size));
                require(entries_ != UINT64_MAX);
                ++entries_;
                if (!slot.value.deleted()) {
                    require(keys_ != UINT64_MAX && slot.value.size <= UINT64_MAX - bytes_);
                    ++keys_;
                    bytes_ += slot.value.size;
                }
                pending_ref_ = false;
                ++slot_;
            }
            page_.reset();
            node_ = {};
            node_pending_ = false;
        }
        require(entries_ == source_->checkpoint.entries && keys_ == source_->checkpoint.user_keys &&
                bytes_ == source_->checkpoint.live_bytes);
        marked_all_ = true;
    }

  public:
    uint64_t visited_pages = 0, punched = 0;
    explicit ReclaimSession(Core &core) : core_(core), scratch_limit_(core.options.writer_bytes) {
        uint8_t idle = 0;
        require(core_.reclaim_state.compare_exchange_strong(idle, 1, std::memory_order_acq_rel),
                FQDB_LOCKED);
        maintenance_at(73);
    }
    ~ReclaimSession() {
        core_.reclaim_state.store(0, std::memory_order_release);
    }
    bool started() const noexcept {
        return started_;
    }
    bool waiting_for_readers() const noexcept {
        return marked_all_ && old_epochs_ && !old_epochs_->expired();
    }
    uint64_t version() const noexcept {
        return source_ ? source_->version : 0;
    }
    uint64_t limit() const noexcept {
        return source_ ? source_->limit : 0;
    }
    bool step(const fqdb_store_maintenance_options &options) {
        require(core_.reclaim_state.load(std::memory_order_acquire) != 2, FQDB_CANCELLED);
        MaintenanceJob job(core_, options);
        if (complete_)
            return true;
        if (!started_) {
            start(job);
            return false;
        }
        {
            auto lock = writer(job);
            require(!core_.poisoned, FQDB_RECOVERY_REQUIRED);
            require(core_.current()->generation == source_->generation, FQDB_CONFLICT);
        }
        if (!marked_all_)
            mark_references(job);
        // Readers of any pre-floor view may still need old pages or values.
        // Future views share the new epoch and inherit only marked old bytes.
        if (!old_epochs_->expired())
            return false;
        maintenance_at(71);
        const auto end = source_->limit / quantum_ * quantum_;
        while (sweep_ < end) {
            check(job);
            if (sweep_ >= range_end_) {
                auto allocated = source_->generation->file->next_data(sweep_, end);
                if (!allocated) {
                    sweep_ = end;
                    break;
                }
                sweep_ = std::max(sweep_, allocated->first / quantum_ * quantum_);
                range_end_ = allocated->second;
            }
            auto block = sweep_ / quantum_;
            if (live(block)) {
                sweep_ += quantum_;
                continue;
            }
            auto blocks = std::max<uint64_t>(1, 1048576 / quantum_);
            if (auto quota = storage::io_quota; quota && quota->max_write) {
                auto remaining = quota->write_bytes <= quota->max_write
                                     ? quota->max_write - quota->write_bytes
                                     : 0;
                blocks = std::min(blocks, remaining / quantum_);
                if (!blocks) {
                    quota->rejected = true;
                    throw IOBudgetStop();
                }
            }
            auto last = std::min(end / quantum_, (range_end_ - 1) / quantum_ + 1);
            last = std::min(last, block + blocks);
            for (auto at = block + 1; at < last; ++at)
                if (live(at)) {
                    last = at;
                    break;
                }
            auto length = (last - block) * quantum_;
            source_->generation->file->punch(sweep_, length);
            punched += length;
            sweep_ += length;
            maintenance_at(72);
        }
        check(job);
        {
            auto lock = writer(job);
            require(!core_.poisoned, FQDB_RECOVERY_REQUIRED);
            require(core_.current()->generation == source_->generation, FQDB_CONFLICT);
            core_.sync_business_locked();
            source_->generation->file->sync();
        }
        complete_ = true;
        return true;
    }
};
} // namespace fengqudb::paged
