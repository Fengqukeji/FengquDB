#pragma once
#include "features.hpp"
#include "history.hpp"
#include <optional>

namespace fengqudb::paged {
inline int64_t checked_add_i64(int64_t previous, int64_t delta, int64_t minimum, int64_t maximum) {
    require(!(delta > 0 && previous > INT64_MAX - delta) &&
                !(delta < 0 && previous < INT64_MIN - delta),
            FQDB_CONSTRAINT);
    const auto next = previous + delta;
    require(next >= minimum && next <= maximum, FQDB_CONSTRAINT);
    return next;
}
enum class AtomicKind {
    compare_exchange,
    erase,
    put_changed,
    add,
    history_hold,
    history_release,
    put,
    erase_any,
    patch
};
struct AtomicUpdate {
    std::shared_ptr<Core> core;
    Bytes key;
    std::string_view value;
    AtomicKind kind;
    uint64_t expected = 0, expires = 0;
    uint64_t offset = 0, remove = 0;
    int64_t delta = 0, minimum = INT64_MIN, maximum = INT64_MAX;
    std::chrono::steady_clock::time_point deadline{};
    bool timed = false;
    std::optional<Transaction> transaction;
    std::optional<SingleWrite> single;
    fqdb_store_atomic_info info{};
    AtomicUpdate(std::shared_ptr<Core> owner, Bytes name, AtomicKind operation, uint64_t timeout)
        : core(std::move(owner)), key(std::move(name)), kind(operation) {
        if (timeout) {
            require(timeout <= static_cast<uint64_t>(INT64_MAX / 1000000), FQDB_INVALID_ARGUMENT);
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::milliseconds(timeout);
            require(duration <= std::chrono::steady_clock::time_point::max() - now,
                    FQDB_INVALID_ARGUMENT);
            deadline = now + duration;
            timed = true;
        }
    }
    void ready() const {
        require(!timed || std::chrono::steady_clock::now() < deadline, FQDB_TIMEOUT);
    }
    void prepare(CommitWaiter &waiter, std::shared_ptr<const View> view) {
        ready();
        if (!view->checkpoint.features &&
            (kind == AtomicKind::put || kind == AtomicKind::erase_any ||
             kind == AtomicKind::patch)) {
            single.emplace(key, core->options.transaction_bytes, core->transaction_budget);
            waiter.single = &*single;
            auto ref = view->find(key);
            const bool present = ref && ref->visible(unix_ms());
            info.observed_version = ref ? ref->version : 0;
            info.matched = 1;
            auto &write = single->value;
            write.original = ref.value_or(Ref{});
            if (kind == AtomicKind::erase_any) {
                if (!present)
                    return;
                write.kind = OpKind::erase;
            } else if (kind == AtomicKind::put) {
                write.kind = OpKind::put;
                write.expires = expires;
                write.value = single->data(value);
            } else {
                require(present, FQDB_NOT_FOUND);
                require(offset <= ref->size && remove <= ref->size - offset, FQDB_INVALID_ARGUMENT);
                require(value.size() <= FQDB_MAX_VALUE_BYTES - (ref->size - remove), FQDB_LIMIT);
                write.value = single->data(*view->generation->value(*ref, view->limit));
                patch_buffer(*write.value, offset, remove, value);
                write.edited = true;
                write.expires = ref->expires;
                write.kind = ref->depth < 8 ? OpKind::patch : OpKind::put;
                if (write.kind == OpKind::patch) {
                    write.insert = single->data(value);
                    write.offset = offset;
                    write.remove = remove;
                }
            }
            single->changed = true;
            info.changed = 1;
            return;
        }
        auto capture = std::make_shared<Capture>(core, std::move(view), unix_ms());
        transaction.emplace(capture, 0);
        waiter.transaction = &*transaction;
        auto ref = capture->view->find(key);
        const bool present = ref && ref->visible(capture->time);
        info.observed_version = ref ? ref->version : 0;
        info.matched = 1;
        if (kind == AtomicKind::history_hold || kind == AtomicKind::history_release) {
            enable_features(*transaction);
            if (kind == AtomicKind::history_release) {
                if (present) {
                    transaction->erase(key);
                    info.changed = 1;
                }
            } else {
                auto pins = history_pins(*capture->view);
                if (expected == UINT64_MAX)
                    expected = capture->view->version;
                require(expected >= history_floor(*capture->view) &&
                            expected <= capture->view->version,
                        FQDB_HISTORY_LOST);
                require(present || pins.total < max_history_pins, FQDB_LIMIT);
                auto bytes = history_pin_bytes(capture->view->generation->uuid, expected);
                if (present &&
                    *capture->view->generation->value(*ref, capture->view->limit) == bytes)
                    return;
                transaction->feature_bits |= 4;
                transaction->put(key, bytes, 0);
                info.changed = 1;
            }
            return;
        }
        if (kind == AtomicKind::compare_exchange || kind == AtomicKind::erase) {
            info.matched = expected ? present && ref->version == expected : !present;
            if (!info.matched)
                return;
        }
        if (kind == AtomicKind::erase || kind == AtomicKind::erase_any) {
            if (present) {
                transaction->erase(key);
                info.changed = 1;
            }
        } else if (kind == AtomicKind::patch) {
            transaction->patch(key, offset, remove, value);
            info.changed = 1;
        } else if (kind == AtomicKind::add) {
            int64_t previous = 0;
            if (present) {
                require(ref->size == 8, FQDB_INVALID_ARGUMENT);
                auto bytes = capture->view->generation->value(*ref, capture->view->limit);
                auto bits = storage::get64(reinterpret_cast<const unsigned char *>(bytes->data()));
                std::memcpy(&previous, &bits, 8);
            }
            auto next = checked_add_i64(previous, delta, minimum, maximum);
            info.value = next;
            if (!present || next != previous) {
                Bytes bytes(8, '\0');
                storage::put64(reinterpret_cast<unsigned char *>(bytes.data()),
                               static_cast<uint64_t>(next));
                transaction->put(key, bytes, present ? ref->expires : 0);
                info.changed = 1;
            }
        } else {
            if (kind == AtomicKind::put_changed && present && ref->expires == expires &&
                ref->size == value.size()) {
                auto bytes = capture->view->generation->value(*ref, capture->view->limit);
                if (detail::binary_equal(*bytes, value))
                    return;
            }
            transaction->put(key, value, expires);
            info.changed = 1;
        }
    }
    fqdb_store_atomic_info commit(fqdb_log_event *trace, bool force_reliable = false) {
        CommitWaiter request;
        request.queued_bytes = key.size() + value.size() + 64;
        request.context = this;
        request.trace = trace;
        request.force_reliable = force_reliable || kind == AtomicKind::history_hold ||
                                 kind == AtomicKind::history_release;
        request.consume_cache_values =
            kind != AtomicKind::history_hold && kind != AtomicKind::history_release;
        request.check = [](void *context) { static_cast<AtomicUpdate *>(context)->ready(); };
        request.prepare = [](CommitWaiter &request, std::shared_ptr<const View> view) {
            static_cast<AtomicUpdate *>(request.context)->prepare(request, std::move(view));
        };
        request.cleanup = [](CommitWaiter &request) noexcept {
            // Only AtomicUpdate owns this private transaction. Its caller and
            // scalar result remain alive until the commit completes.
            request.transaction = nullptr;
            request.single = nullptr;
            auto &update = *static_cast<AtomicUpdate *>(request.context);
            update.transaction.reset();
            update.single.reset();
        };
        ready();
        info.version = storage::io_quota ? core->commit_direct(request) : core->submit(request);
        return info;
    }
};
struct FieldUpdates {
    struct Item {
        Bytes key;
        uint64_t offset;
        int64_t delta, minimum, maximum;
    };
    std::shared_ptr<Core> core;
    std::shared_ptr<Budget> budget;
    Charge inputs;
    std::vector<Item> items;
    std::optional<Transaction> transaction;
    std::chrono::steady_clock::time_point deadline{};
    uint64_t queued_bytes;
    bool timed = false;
    fqdb_store_atomic_info info{};
    FieldUpdates(std::shared_ptr<Core> owner, uint64_t bytes, uint64_t timeout)
        : core(std::move(owner)), budget(std::make_shared<Budget>(core->options.transaction_bytes)),
          inputs(budget, core->transaction_budget, bytes), queued_bytes(bytes) {
        if (timeout) {
            require(timeout <= static_cast<uint64_t>(INT64_MAX / 1000000), FQDB_INVALID_ARGUMENT);
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::milliseconds(timeout);
            require(duration <= std::chrono::steady_clock::time_point::max() - now,
                    FQDB_INVALID_ARGUMENT);
            deadline = now + duration;
            timed = true;
        }
    }
    void ready() const {
        require(!timed || std::chrono::steady_clock::now() < deadline, FQDB_TIMEOUT);
    }
    void prepare(CommitWaiter &request, std::shared_ptr<const View> view) {
        ready();
        info.observed_version = view->version;
        // Inputs and staged data share one per-command limit.
        transaction.emplace(std::make_shared<Capture>(core, std::move(view), unix_ms()), 0, budget);
        request.transaction = &*transaction;
        for (const auto &item : items) {
            ready();
            const auto previous = [&] {
                auto bytes = transaction->get(item.key);
                require(item.offset <= bytes->size() && bytes->size() - item.offset >= 8,
                        FQDB_INVALID_ARGUMENT);
                const auto bits = storage::get64(
                    reinterpret_cast<const unsigned char *>(bytes->data() + item.offset));
                int64_t value;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            }(); // Release the read alias before editing an already-private buffer.
            const auto next = checked_add_i64(previous, item.delta, item.minimum, item.maximum);
            info.value = next;
            if (next != previous) {
                unsigned char bytes[8];
                storage::put64(bytes, static_cast<uint64_t>(next));
                transaction->patch(item.key, item.offset, 8,
                                   {reinterpret_cast<const char *>(bytes), 8});
                info.changed = 1;
            }
        }
        ready();
        info.matched = 1;
    }
    fqdb_store_atomic_info commit(fqdb_log_event *trace, bool reliable) {
        CommitWaiter request;
        request.queued_bytes = queued_bytes;
        request.context = this;
        request.trace = trace;
        request.force_reliable = reliable;
        request.consume_cache_values = true; // Only scalar results escape this command.
        request.check = [](void *context) { static_cast<FieldUpdates *>(context)->ready(); };
        request.prepare = [](CommitWaiter &r, std::shared_ptr<const View> view) {
            static_cast<FieldUpdates *>(r.context)->prepare(r, std::move(view));
        };
        request.cleanup = [](CommitWaiter &r) noexcept {
            r.transaction = nullptr;
            auto &job = *static_cast<FieldUpdates *>(r.context);
            job.transaction.reset();
            std::vector<Item>().swap(job.items);
            job.inputs.reset();
        };
        ready();
        info.version = storage::io_quota ? core->commit_direct(request) : core->submit(request);
        return info;
    }
};
struct PreparedBatch {
    struct Item {
        Bytes key;
        std::shared_ptr<StagedData> value;
        uint64_t expires;
    };
    Charge pins;
    std::vector<Item> items;
    uint64_t bytes = 0;
    explicit PreparedBatch(Core &core) : pins(core.pin_budget, {}, sizeof(PreparedBatch) + 128) {}
    void apply(Transaction &tx, std::shared_ptr<const PreparedBatch> self) const {
        tx.ready();
        if (items.empty())
            return;
        tx.retain(std::move(self), bytes);
        for (const auto &item : items) {
            Write value;
            value.kind = OpKind::put;
            value.value = item.value;
            value.expires = item.expires;
            tx.stage(item.key, std::move(value));
        }
    }
};
} // namespace fengqudb::paged
