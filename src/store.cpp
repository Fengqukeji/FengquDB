#include "paged/cursor.hpp"
#include "paged/expiry.hpp"
#include "paged/features.hpp"
#include "paged/query.hpp"
#include "paged/select.hpp"
#define FQDB_CORE_IMPLEMENTATION
#include "paged/group.hpp"
#undef FQDB_CORE_IMPLEMENTATION
#include "paged/index_build.hpp"
#include "paged/archive.hpp"
#include "paged/operations.hpp"
#include "paged/watch.hpp"
#include "paged/reclaim.hpp"
#include "logging.hpp"

namespace fengqudb::paged {
void attach_history(Core &core) {
    load_history(core);
}
void prepare_features(Transaction &transaction, const View &current) {
    features_prepare(transaction, current);
}
void verify_features(const View &view, uint64_t time, const std::function<void()> &check) {
    try {
        features_verify(view, time, check);
    } catch (const Error &error) {
        if (error.status == FQDB_CONSTRAINT)
            throw Error(FQDB_CORRUPT);
        throw;
    }
}
} // namespace fengqudb::paged
namespace p = fengqudb::paged;
struct fqdb_store {
    std::shared_ptr<p::Core> core;
};
struct fqdb_store_snapshot {
    std::shared_ptr<p::Capture> capture;
};
struct fqdb_store_watch {
    p::Watch value;
    template <class... Args>
    explicit fqdb_store_watch(Args &&...args) : value(std::forward<Args>(args)...) {}
};
struct fqdb_store_change_batch {
    std::shared_ptr<p::ChangeBatch> value;
};
struct fqdb_store_txn {
    p::Transaction value;
    fqdb_store_txn(std::shared_ptr<p::Core> core, uint64_t timeout)
        : value(std::move(core), timeout) {}
};
struct fqdb_store_commit {
    // Native close waits before releasing this external Core owner. Worker
    // cleanup releases every private transaction BEFORE publishing completion,
    // so Core can never be destroyed by its own write thread.
    std::shared_ptr<p::Core> core;
    p::CommitWaiter request;
    std::unique_ptr<p::Transaction> transaction;
    p::Transaction *source = nullptr;
    std::string key, value;
    uint64_t expires = 0;
    std::atomic<bool> cancelled{false};
    explicit fqdb_store_commit(std::shared_ptr<p::Core> owner) : core(std::move(owner)) {
        request.context = this;
        request.check = [](void *context) {
            auto &job = *static_cast<fqdb_store_commit *>(context);
            p::require(!job.cancelled.load(std::memory_order_relaxed), FQDB_CANCELLED);
            if (job.source)
                job.source->ready();
        };
        request.cleanup = [](p::CommitWaiter &r) noexcept {
            r.transaction = nullptr;
            auto &job = *static_cast<fqdb_store_commit *>(r.context);
            job.transaction.reset();
            std::string{}.swap(job.key);
            std::string{}.swap(job.value);
        };
    }
};
struct fqdb_store_cursor {
    p::ReadCursor value;
    template <class... Args>
    explicit fqdb_store_cursor(Args &&...args) : value(std::forward<Args>(args)...) {}
};
struct fqdb_store_value {
    std::shared_ptr<p::Core> core;
    p::Value value;
    uint64_t version, expires, pinned;
    fqdb_store_value(std::shared_ptr<p::Core> owner, p::Value bytes, const p::Ref &ref)
        : core(std::move(owner)), value(std::move(bytes)), version(ref.version),
          expires(ref.expires),
          pinned(sizeof(fqdb_store_value) + sizeof(p::Data) + value->capacity() + 64) {
        // Core already owns this budget and outlives the lease. Avoid a second
        // shared ownership counter on every ordinary shared read.
        core->pin_budget->take(pinned);
    }
    ~fqdb_store_value() {
        core->pin_budget->release(pinned);
    }
    fqdb_store_value(const fqdb_store_value &) = delete;
    fqdb_store_value &operator=(const fqdb_store_value &) = delete;
};
struct fqdb_store_results {
    struct Item {
        p::Value value;
        p::Ref metadata;
    };
    std::shared_ptr<p::Core> core;
    p::Charge pins;
    std::vector<Item> items;
    std::vector<fqdb_store_result_row> rows;
    uint64_t retained = 0, payload = 0;
    fqdb_store_results(std::shared_ptr<p::Core> owner, uint32_t count)
        : core(std::move(owner)),
          pins(core->pin_budget, {},
               sizeof(fqdb_store_results) + count * (sizeof(Item) + sizeof(fqdb_store_result_row)) +
                   128) {
        items.reserve(count);
        rows.reserve(count);
        retained = sizeof(fqdb_store_results) + items.capacity() * sizeof(Item) +
                   rows.capacity() * sizeof(fqdb_store_result_row) + 128;
        if (retained != pins.bytes())
            pins.resize(retained);
    }
};
struct fqdb_store_reader {
    std::shared_ptr<p::Core> core;
    p::Charge pins;
    std::vector<p::Bytes> keys;
    std::vector<uint32_t> projection;
    bool projected = false;
    std::weak_ptr<const p::View> cached;
    std::unique_ptr<fqdb_store_results> result;
    uint64_t last_time = 0, expires = 0, stamp = 0, generation = 0;
    fqdb_store_reader(std::shared_ptr<p::Core> owner, const fqdb_slice *input, uint32_t count)
        : core(std::move(owner)), pins(core->pin_budget, {}, sizeof(fqdb_store_reader) + 128) {
        keys.reserve(count);
        uint64_t retained = sizeof(fqdb_store_reader) + 128 + keys.capacity() * sizeof(p::Bytes);
        pins.resize(retained);
        for (uint32_t i = 0; i < count; ++i) {
            keys.emplace_back(input[i].size ? static_cast<const char *>(input[i].data) : "",
                              static_cast<size_t>(input[i].size));
            retained += keys.back().capacity() + 32;
            pins.resize(retained);
        }
    }
    void release() noexcept {
        result.reset();
        cached.reset();
        expires = last_time = stamp = generation = 0;
    }
};
struct fqdb_store_batch {
    std::shared_ptr<p::Core> core;
    std::shared_ptr<const p::PreparedBatch> data;
};
struct fqdb_store_key {
    std::shared_ptr<p::Core> core;
    p::Bytes name;
    p::Charge pins;
    std::mutex mutex;
    std::weak_ptr<const p::View> cached;
    std::optional<p::Ref> reference;
    std::weak_ptr<const p::Data> memo_value;
    p::Ref memo_reference;
    uint64_t memo_stamp = 0, memo_time = 0, memo_generation = 0;
    fqdb_store_key(std::shared_ptr<p::Core> owner, p::Bytes key)
        : core(std::move(owner)), name(std::move(key)),
          pins(core->pin_budget, {}, sizeof(fqdb_store_key) + name.capacity() + 128) {}
    std::optional<p::Ref> resolve(const std::shared_ptr<const p::View> &view) {
        std::lock_guard<std::mutex> lock(mutex);
        if (cached.lock() != view) {
            reference = (view->checkpoint.features & 1) && p::system_key(name)
                            ? std::optional<p::Ref>{}
                            : view->find(name);
            cached = view;
        }
        return reference;
    }
};
namespace {
template <class F> fqdb_status boundary(F &&function) noexcept {
    try {
        if constexpr (std::is_void<decltype(function())>::value) {
            function();
            return FQDB_OK;
        } else
            return function();
    } catch (const p::Error &error) {
        return error.status;
    } catch (const std::bad_alloc &) {
        return FQDB_NO_MEMORY;
    } catch (const std::length_error &) {
        return FQDB_LIMIT;
    } catch (const std::invalid_argument &) {
        return FQDB_INVALID_ARGUMENT;
    } catch (const std::filesystem::filesystem_error &) {
        return FQDB_IO_ERROR;
    } catch (...) {
        return FQDB_INTERNAL_ERROR;
    }
}
bool valid(fqdb_slice value, uint64_t maximum) {
    return value.size <= maximum && (!value.size || value.data);
}
std::string_view view(fqdb_slice value) {
    return value.size ? std::string_view(static_cast<const char *>(value.data),
                                         static_cast<size_t>(value.size))
                      : std::string_view{};
}
p::Bytes key(fqdb_slice value) {
    return p::Bytes(view(value));
}
void raw_range(p::Transaction &tx, const p::Bytes &begin, const p::Bytes &end) {
    p::require(end.empty() || p::compare(begin, end) <= 0, FQDB_INVALID_ARGUMENT);
    if (!tx.advanced && !(tx.base->view->checkpoint.features & 1)) {
        tx.range(begin, end);
        return;
    }
    const auto &prefix = p::system_prefix();
    auto after = p::prefix_end(prefix);
    if (p::compare(begin, prefix) < 0) {
        auto high = end.empty() || p::compare(end, prefix) > 0 ? prefix : end;
        if (p::compare(begin, high) < 0)
            tx.range(begin, std::move(high));
    }
    if (end.empty() || p::compare(end, after) > 0) {
        auto low = p::compare(begin, after) > 0 ? begin : after;
        if (end.empty() || p::compare(low, end) < 0)
            tx.range(std::move(low), end);
    }
}
void copy(const p::Value &value, fqdb_buffer *out) {
    auto status = fqdb_buffer_copy({value->data(), value->size()}, out);
    p::require(status == FQDB_OK, status);
}
template <class F>
fqdb_status read_value(const p::View &state, uint64_t time, std::string_view key, F &&consume) {
    auto ref = state.find(key);
    if (!ref || !ref->visible(time))
        return FQDB_NOT_FOUND;
    consume(state.generation->value(*ref, state.limit), *ref);
    return FQDB_OK;
}
bool same_ref(const p::Ref &a, const p::Ref &b) noexcept {
    return a.version == b.version && a.offset == b.offset && a.size == b.size &&
           a.expires == b.expires && a.crc == b.crc && a.depth == b.depth &&
           a.recipe_size == b.recipe_size;
}
struct LatestReadMemo {
    std::array<char, 256> key{};
    size_t length = SIZE_MAX;
    uint64_t store = 0, stamp = 0, time = 0, generation = 0;
    p::Ref metadata;
    std::weak_ptr<const p::Data> value;
};
LatestReadMemo &latest_read_memo() {
    // Fixed metadata only: no Core, View, generation or strong value retention.
    static thread_local LatestReadMemo memo;
    return memo;
}
template <class F> fqdb_status read_latest(p::Core &core, std::string_view key, F &&consume) {
    const auto stamp = core.read_stamp.load(std::memory_order_acquire);
    auto &memo = latest_read_memo();
    const bool eligible = core.options.cache_bytes && key.size() <= memo.key.size();
    const bool matching = eligible && memo.store == core.id && memo.length == key.size() &&
                          (key.empty() || !std::memcmp(memo.key.data(), key.data(), key.size()));
    if (matching && !(stamp & 1) && memo.stamp == stamp) {
        const auto time = p::unix_ms();
        if (time >= memo.time && memo.metadata.visible(time)) {
            if (auto bytes = memo.value.lock()) {
                memo.time = time;
                consume(std::move(bytes), memo.metadata);
                return FQDB_OK;
            }
        }
    }
    auto state = core.current();
    const auto after = core.read_stamp.load(std::memory_order_acquire);
    const auto time = p::unix_ms();
    if ((state->checkpoint.features & 1) && p::system_key(key))
        return FQDB_NOT_FOUND;
    auto ref = state->find(key);
    if (!ref || !ref->visible(time))
        return FQDB_NOT_FOUND;
    p::Value bytes;
    if (matching && memo.generation == state->generation->number && same_ref(memo.metadata, *ref))
        bytes = memo.value.lock();
    if (!bytes)
        bytes = state->generation->value(*ref, state->limit);
    if (eligible && stamp == after && !(after & 1)) {
        if (!key.empty())
            std::memcpy(memo.key.data(), key.data(), key.size());
        memo.length = key.size();
        memo.store = core.id;
        memo.stamp = after;
        memo.time = time;
        memo.generation = state->generation->number;
        memo.metadata = *ref;
        memo.value = bytes;
    }
    consume(std::move(bytes), *ref);
    return FQDB_OK;
}
// The prepared key owns its name and budget; weak bytes never retain a
// payload, generation or Core beyond the existing handle's lifetime.
template <class Prepare, class Consume>
fqdb_status read_prepared(fqdb_store_key &key, Prepare &&prepare, Consume &&consume) {
    auto &core = *key.core;
    const auto stamp = core.read_stamp.load(std::memory_order_acquire);
    const auto time = p::unix_ms();
    const bool eligible = core.options.cache_bytes != 0;
    p::Ref metadata;
    p::Value bytes;
    if (eligible && !(stamp & 1)) {
        std::lock_guard<std::mutex> lock(key.mutex);
        if (key.memo_stamp == stamp && time >= key.memo_time && key.memo_reference.visible(time)) {
            metadata = key.memo_reference;
            bytes = key.memo_value.lock();
            if (bytes) {
                auto status = prepare(metadata);
                if (status != FQDB_OK)
                    return status;
                key.memo_time = time;
            }
        }
    }
    if (bytes) {
        consume(std::move(bytes), metadata);
        return FQDB_OK;
    }
    p::maintenance_at(89); // Test-only: expired weak bytes before resolving a fresh view.
    auto state = core.current();
    const auto after = core.read_stamp.load(std::memory_order_acquire);
    const auto at = p::unix_ms();
    auto ref = key.resolve(state);
    if (!ref || !ref->visible(at))
        return FQDB_NOT_FOUND;
    auto status = prepare(*ref);
    if (status != FQDB_OK)
        return status; // Buffer-too-small must not load or checksum the value.
    if (eligible) {
        std::lock_guard<std::mutex> lock(key.mutex);
        if (key.memo_generation == state->generation->number && same_ref(key.memo_reference, *ref))
            bytes = key.memo_value.lock();
    }
    if (!bytes)
        bytes = state->generation->value(*ref, state->limit);
    if (eligible && stamp == after && !(after & 1)) {
        std::lock_guard<std::mutex> lock(key.mutex);
        key.memo_stamp = after;
        key.memo_time = at;
        key.memo_generation = state->generation->number;
        key.memo_reference = *ref;
        key.memo_value = bytes;
    }
    consume(std::move(bytes), *ref);
    return FQDB_OK;
}

template <class F> fqdb_status traced(uint32_t operation, F &&function) {
    fengqudb::detail::LogSpan span(operation);
    auto result = boundary([&] { function(span.event()); });
    return span.finish(result);
}
template <class F> fqdb_status staging(fqdb_store_txn *txn, F &&function) {
    return boundary([&] {
        try {
            function();
        } catch (const p::Error &error) {
            if (error.status == FQDB_LIMIT || error.status == FQDB_NO_MEMORY ||
                error.status == FQDB_IO_ERROR || error.status == FQDB_CORRUPT)
                txn->value.broken = true;
            throw;
        } catch (...) {
            txn->value.broken = true;
            throw;
        }
    });
}
void validate_keys(const fqdb_slice *keys, uint32_t count, bool write = false) {
    p::require(count <= 4096 && (!count || keys), FQDB_INVALID_ARGUMENT);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        p::require(valid(keys[i], FQDB_MAX_KEY_BYTES) && (!write || !p::system_key(view(keys[i]))),
                   FQDB_INVALID_ARGUMENT);
        bytes += keys[i].size + 64;
    }
    p::require(bytes <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
}
void validate_items(const fqdb_store_put_item *items, uint32_t count) {
    p::require(count <= 4096 && (!count || items), FQDB_INVALID_ARGUMENT);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        p::require(valid(items[i].key, FQDB_MAX_KEY_BYTES) && !p::system_key(view(items[i].key)) &&
                       valid(items[i].value, FQDB_MAX_VALUE_BYTES),
                   FQDB_INVALID_ARGUMENT);
        bytes += items[i].key.size + items[i].value.size + 64;
    }
    p::require(bytes <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
}
uint64_t item_retained(const p::Value &value) noexcept {
    return value ? sizeof(p::Data) + value->capacity() + 64 : 0;
}
void result_item(fqdb_store_results &result, uint32_t index, fqdb_store_results::Item item,
                 uint32_t following = 0) {
    const auto &previous = result.items[index];
    const auto payload = result.payload - (previous.value ? previous.value->size() : 0) +
                         (item.value ? item.value->size() : 0);
    p::require(payload <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
    const auto retained =
        result.retained - item_retained(previous.value) + item_retained(item.value);
    // Fresh batches reserve a small, already-charged allowance to amortize
    // shared budget atomics. A tight budget retries the exact requirement.
    // The last row returns unused allowance before publishing the result.
    if (following && retained > result.pins.bytes()) {
        const auto slack = item_retained(item.value) * std::min<uint32_t>(following, 7);
        try {
            result.pins.resize(retained + std::min<uint64_t>(slack, 65536));
        } catch (const p::Error &e) {
            if (e.status != FQDB_LIMIT)
                throw;
            result.pins.resize(retained);
        }
    } else if (!following && retained != result.pins.bytes())
        result.pins.resize(retained);
    result.rows[index] = {item.value ? fqdb_slice{item.value->data(), item.value->size()}
                                     : fqdb_slice{},
                          item.metadata.version, item.metadata.expires, item.value ? 1u : 0u, 0};
    result.items[index] = std::move(item);
    result.payload = payload;
    result.retained = retained;
}
template <class Fetch>
void read_many(std::shared_ptr<p::Core> core, uint32_t count, fqdb_store_results **out,
               Fetch &&fetch) {
    auto result = std::make_unique<fqdb_store_results>(std::move(core), count);
    result->items.resize(count);
    result->rows.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        result_item(*result, i, fetch(i), count - i - 1);
    *out = result.release();
}
p::Ref read_ref(const p::View &state, fqdb_slice key, p::Table::ReadPath *path = nullptr) {
    if ((state.checkpoint.features & 1) && p::system_key(view(key)))
        return {};
    return (path ? state.find(view(key), *path) : state.find(view(key))).value_or(p::Ref{});
}
fqdb_store_results::Item read_item(const p::View &state, uint64_t time, fqdb_slice key,
                                   p::Table::ReadPath *path = nullptr) {
    auto ref = read_ref(state, key, path);
    return {ref.visible(time) ? state.generation->value(ref, state.limit) : p::Value{}, ref};
}
void reader_refresh(fqdb_store_reader &reader) {
    const auto stamp = reader.core->read_stamp.load(std::memory_order_acquire);
    if (reader.result && stamp == reader.stamp && !(stamp & 1)) {
        const auto time = p::unix_ms();
        if (time >= reader.last_time && (!reader.expires || time < reader.expires)) {
            reader.last_time = time;
            return;
        }
    }
    auto state = reader.core->current();
    const auto after = reader.core->read_stamp.load(std::memory_order_acquire);
    const auto time = p::unix_ms();
    const auto observed = stamp == after && !(after & 1) ? after : 0;
    if (reader.result && reader.cached.lock() == state && time >= reader.last_time &&
        (!reader.expires || time < reader.expires)) {
        reader.stamp = observed;
        reader.last_time = time;
        return;
    }
    try {
        std::optional<p::Table::ReadPath> path;
        if (reader.keys.size() >= 8 && reader.core->options.cache_bytes)
            path.emplace(std::min<uint64_t>(262144, reader.core->options.writer_bytes / 8));
        const bool reusable = reader.generation == state->generation->number;
        auto materialize = [&](const p::Ref &ref) {
            fqdb_store_results::Item item{
                ref.visible(time) ? state->generation->value(ref, state->limit) : p::Value{}, ref};
            if (item.value && reader.projected) {
                p::Charge temporary(reader.core->pin_budget, {}, 0);
                uint64_t retained = 0;
                item.value = std::make_shared<p::Data>(
                    p::project_fields(*item.value, reader.projection, temporary, retained));
            }
            return item;
        };
        auto fetch = [&](uint32_t i) {
            const auto &name = reader.keys[i];
            return materialize(
                read_ref(*state, {name.data(), name.size()}, path ? &*path : nullptr));
        };
        auto rebuild = [&] {
            fqdb_store_results *result = nullptr;
            read_many(reader.core, static_cast<uint32_t>(reader.keys.size()), &result, fetch);
            reader.result.reset(result);
        };
        if (!reader.result) {
            rebuild();
        } else {
            try {
                // This descriptor array belongs only to this reader. Borrowed reads
                // expire on the next call; acquired batches own separate descriptors.
                // Resolve all keys against ONE captured view/time and replace only
                // changed items, preserving other immutable payloads and pin charges.
                for (uint32_t i = 0; i < reader.keys.size(); ++i) {
                    auto &name = reader.keys[i];
                    const auto ref =
                        read_ref(*state, {name.data(), name.size()}, path ? &*path : nullptr);
                    const auto &old = reader.result->items[i];
                    if (reusable && same_ref(old.metadata, ref) &&
                        bool(old.value) == ref.visible(time))
                        continue;
                    result_item(*reader.result, i, materialize(ref));
                }
            } catch (const p::Error &error) {
                if (error.status != FQDB_LIMIT)
                    throw;
                // Mixed old/new payloads may temporarily exceed a tight pin or
                // batch limit. Release the invalidated batch and retry once against
                // the same captured view, matching fresh-read budget semantics.
                reader.release();
                rebuild();
            }
        }
    } catch (...) {
        // Never return a partly refreshed batch after an allocation, wire or I/O failure.
        reader.release();
        throw;
    }
    reader.expires = 0;
    for (const auto &row : reader.result->rows)
        if (row.found && row.expires_at_ms &&
            (!reader.expires || row.expires_at_ms < reader.expires))
            reader.expires = row.expires_at_ms;
    reader.cached = state;
    reader.generation = state->generation->number;
    reader.last_time = time;
    reader.stamp = observed;
}

template <class Read> fqdb_status shared_read(fqdb_store_shared_value *out, Read &&read) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    auto status = read(&out->owner);
    if (status == FQDB_OK) {
        auto &v = *out->owner;
        out->row = {{v.value->data(), v.value->size()}, v.version, v.expires, 1, 0};
    }
    return status;
}

template <class Configure>
fqdb_status atomic_update(fqdb_store *store, fqdb_slice k, p::AtomicKind kind, uint64_t timeout,
                          fqdb_store_atomic_info *out, Configure &&configure,
                          uint32_t force_reliable = 0) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store || force_reliable > 1 || !valid(k, FQDB_MAX_KEY_BYTES) || p::system_key(view(k)))
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_COMMIT, [&](fqdb_log_event *event) {
        p::AtomicUpdate update(store->core, key(k), kind, timeout);
        configure(update);
        if (event) {
            event->database_id = store->core->id;
            event->durability = store->core->options.durability;
        }
        *out = update.commit(event, force_reliable != 0);
    });
}
fqdb_store_maintenance_options maintenance_options(const fqdb_store_maintenance_options *options) {
    auto result = p::maintenance_defaults();
    if (options) {
        constexpr size_t prefix_size = offsetof(fqdb_store_maintenance_options, max_read_bytes);
        p::require(options->struct_size >= prefix_size &&
                       (options->abi_version == 1 || options->abi_version == 2),
                   FQDB_INVALID_ARGUMENT);
        std::memcpy(&result, options, prefix_size);
        if (options->abi_version == 2) {
            p::require(options->struct_size >= sizeof(result), FQDB_INVALID_ARGUMENT);
            std::memcpy(&result, options, sizeof(result));
        }
    }
    p::require(result.reserved == 0 && result.max_catchup_rounds <= 64 &&
                   result.max_barrier_bytes <= FQDB_MAX_TRANSACTION_BYTES,
               FQDB_INVALID_ARGUMENT);
    return result;
}
template <class F>
auto metered_maintenance(const fqdb_store_maintenance_options &settings, F &&function) {
    p::MaintenanceIO io(settings);
    try {
        return function();
    } catch (const p::Error &error) {
        if (io.rejected() && (error.status == FQDB_IO_ERROR || error.status == FQDB_CORRUPT))
            throw p::IOBudgetStop();
        throw;
    }
}
void maintenance_event(fqdb_log_event *event, const p::Core &core) noexcept {
    if (event) {
        event->database_id = core.id;
        event->durability = core.options.durability;
        event->stage = FQDB_STAGE_FILE_HEADER;
    }
}
void maintenance_result(fqdb_log_event *event, const fqdb_store_maintenance_info &info) noexcept {
    if (event) {
        event->version = info.final_version;
        event->base_version = info.snapshot_version;
        event->operation_count = info.copied_keys;
        event->payload_bytes = info.output_file_bytes;
        event->stage = FQDB_STAGE_COMPLETE;
    }
}
} // namespace
extern "C" {
#ifdef FQDB_TESTING
void fqdb_store_test_read_stamp(fqdb_store *store, uint64_t value) {
    std::lock_guard<p::WriterGate> lock(store->core->writer);
    store->core->read_stamp.store(value, std::memory_order_release);
}
void fqdb_store_test_crash(int point) {
    p::store_crash_point.store(point);
}
void fqdb_store_test_maintenance_hold(int point) {
    if (point)
        p::maintenance_entered.store(false);
    p::maintenance_hold_point.store(point);
}
bool fqdb_store_test_maintenance_entered() {
    return p::maintenance_entered.load();
}
void fqdb_store_test_publication_fault(bool manifest, bool directory) {
    p::manifest_uncertain.store(manifest);
    p::directory_uncertain.store(directory);
}
void fqdb_store_test_open_fault(bool fail) {
    p::paged_fail_created_open.store(fail);
}
void fqdb_store_test_io_fault(int write, int sync) {
    p::paged_fail_write.store(write);
    p::paged_fail_sync.store(sync);
}
uintptr_t fqdb_store_test_staged_pointer(fqdb_store_txn *txn, fqdb_slice k) {
    auto node = p::write_find(txn->value.writes, view(k));
    return node && node->value.value ? reinterpret_cast<uintptr_t>(node->value.value->data()) : 0;
}
#endif
fqdb_status FQDB_CALL fqdb_store_open(const char *directory, const fqdb_store_options *options,
                                      fqdb_store **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!directory || !*directory)
        return FQDB_INVALID_ARGUMENT;
    auto opts = p::defaults();
    if (options) {
        if (options->struct_size < sizeof(opts) || options->abi_version != 1)
            return FQDB_INVALID_ARGUMENT;
        opts = *options;
    }
    if (opts.durability > FQDB_DURABILITY_BUFFERED ||
        (opts.flags & ~(FQDB_STORE_CREATE | FQDB_STORE_SEGMENTED)) || opts.delta_bytes < 65536 ||
        opts.writer_bytes < 1048576 || opts.transaction_bytes < 1048576 ||
        opts.all_transaction_bytes < opts.transaction_bytes || opts.pinned_bytes < 65536 ||
        !opts.max_snapshots)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_OPEN, [&](fqdb_log_event *event) {
        auto result = std::make_unique<fqdb_store>();
        result->core = std::make_shared<p::Core>(directory, opts);
        if (event) {
            event->database_id = result->core->id;
            event->durability = opts.durability;
            event->stage = FQDB_STAGE_OPEN;
        }
        result->core->open();
        if (event) {
            event->version = result->core->current()->version;
            event->repaired_bytes = result->core->repaired_bytes;
        }
        *out = result.release();
    });
}
void FQDB_CALL fqdb_store_close(fqdb_store *store) {
    if (store) {
        store->core->stop_worker();
        store->core->stop_periodic();
    }
    delete store;
}
fqdb_status FQDB_CALL fqdb_store_write_configure(fqdb_store *store,
                                                 const fqdb_store_write_options *options) {
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    if (options && options->struct_size < sizeof(*options))
        return FQDB_INVALID_ARGUMENT;
    auto settings = options ? *options : p::write_defaults();
    if (!p::valid_write_policy(settings))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { store->core->configure_write(settings); });
}
fqdb_status FQDB_CALL fqdb_store_write_options_get(fqdb_store *store,
                                                   fqdb_store_write_options *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { *out = store->core->get_write_policy(); });
}
fqdb_status FQDB_CALL fqdb_store_put_submit(fqdb_store *store, fqdb_slice k, fqdb_slice value,
                                            uint64_t expires, fqdb_store_commit **out,
                                            uint32_t force_reliable) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store || !valid(k, FQDB_MAX_KEY_BYTES) || p::system_key(view(k)) ||
        !valid(value, FQDB_MAX_VALUE_BYTES) ||
        store->core->options.durability != FQDB_DURABILITY_SYNC)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto job = std::make_unique<fqdb_store_commit>(store->core);
        job->key = key(k);
        job->value.assign(view(value));
        job->expires = expires;
        job->request.queued_bytes = k.size + value.size + 64;
        job->request.prepare = [](p::CommitWaiter &r, std::shared_ptr<const p::View> state) {
            auto &job = *static_cast<fqdb_store_commit *>(r.context);
            job.transaction = std::make_unique<p::Transaction>(
                std::make_shared<p::Capture>(job.core, std::move(state), p::unix_ms()), 0);
            r.transaction = job.transaction.get();
            job.transaction->put(job.key, job.value, job.expires);
            r.consume_cache_values = true; // Newly created put, not a submitted user write tree.
        };
        job->request.force_reliable = force_reliable != 0;
        p::require(force_reliable <= 1, FQDB_INVALID_ARGUMENT);
        job->core->admit(job->request, true);
        *out = job.release();
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_submit(fqdb_store_txn *txn, fqdb_store_commit **out,
                                            uint32_t force_reliable) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!txn || txn->value.base->core->options.durability != FQDB_DURABILITY_SYNC)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto &source = txn->value;
        source.ready();
        auto job = std::make_unique<fqdb_store_commit>(source.base->core);
        job->source = &source;
        job->transaction = std::make_unique<p::Transaction>(source.base, 0);
        job->request.transaction = job->transaction.get();
        // Estimate before admission. All state transfer below is nonthrowing.
        p::CommitWaiter estimate;
        estimate.transaction = &source;
        job->request.queued_bytes = p::commit_input_bytes(estimate);
        job->request.accept = [](p::CommitWaiter &r) noexcept {
            auto &job = *static_cast<fqdb_store_commit *>(r.context);
            auto &from = *job.source;
            auto &to = *job.transaction;
            to.budget.swap(from.budget);
            to.reads.swap(from.reads);
            to.ranges.swap(from.ranges);
            to.savepoints.swap(from.savepoints);
            to.writes.swap(from.writes);
            to.retained.swap(from.retained);
            to.cancelled.store(from.cancelled.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            to.deadline = from.deadline;
            to.next_savepoint = from.next_savepoint;
            to.has_deadline = from.has_deadline;
            to.advanced = from.advanced;
            to.skip_indexes = from.skip_indexes;
            to.imported_time = from.imported_time;
            to.feature_bits = from.feature_bits;
            from.sealed = true;
            job.source = nullptr;
        };
        job->request.force_reliable = force_reliable != 0;
        p::require(force_reliable <= 1, FQDB_INVALID_ARGUMENT);
        job->core->admit(job->request, true);
        *out = job.release();
    });
}
fqdb_status FQDB_CALL fqdb_store_commit_wait(fqdb_store_commit *job, uint64_t timeout,
                                             fqdb_status *completion, uint64_t *version) {
    if (!completion || !version)
        return FQDB_INVALID_ARGUMENT;
    *completion = FQDB_NOT_MODIFIED;
    *version = 0;
    if (!job)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        if (!job->core->wait_commit(job->request, timeout))
            return FQDB_TIMEOUT;
        *completion = job->request.status;
        *version = job->request.version;
        return FQDB_OK;
    });
}
fqdb_status FQDB_CALL fqdb_store_commit_poll(fqdb_store_commit *job, fqdb_status *completion,
                                             uint64_t *version) {
    if (!completion || !version)
        return FQDB_INVALID_ARGUMENT;
    *completion = FQDB_NOT_MODIFIED;
    *version = 0;
    if (!job)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        if (!job->core->wait_commit(job->request, 0, true))
            return FQDB_NOT_MODIFIED;
        *completion = job->request.status;
        *version = job->request.version;
        return FQDB_OK;
    });
}
void FQDB_CALL fqdb_store_commit_cancel(fqdb_store_commit *job) {
    if (job) {
        job->cancelled.store(true, std::memory_order_relaxed);
        job->core->cancel_commit(job->request);
    }
}
void FQDB_CALL fqdb_store_commit_close(fqdb_store_commit *job) {
    if (job) {
        (void)job->core->wait_commit(job->request, 0);
        delete job;
    }
}
fqdb_status FQDB_CALL fqdb_store_configure(fqdb_store *store,
                                           const fqdb_store_runtime_options *options) {
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    auto settings = fqdb_store_runtime_options{sizeof(fqdb_store_runtime_options),
                                               1,
                                               0,
                                               200,
                                               256,
                                               128,
                                               0,
                                               30,
                                               33554432,
                                               33554432,
                                               1000,
                                               0,
                                               0};
    if (options) {
        constexpr size_t prefix = offsetof(fqdb_store_runtime_options, job_max_read_bytes);
        if (options->struct_size < prefix ||
            (options->abi_version != 1 && options->abi_version != 2))
            return FQDB_INVALID_ARGUMENT;
        std::memcpy(&settings, options, prefix);
        if (options->abi_version == 2) {
            if (options->struct_size < sizeof(settings))
                return FQDB_INVALID_ARGUMENT;
            std::memcpy(&settings, options, sizeof(settings));
        }
    }
    if (settings.background > 1 || settings.interval_ms < 10 || !settings.expiry_keys ||
        settings.expiry_keys > 65536 || !settings.group_max || settings.group_max > 256 ||
        settings.group_delay_us > 10000 || !settings.garbage_percent ||
        settings.garbage_percent > 100 || !settings.compact_growth_bytes)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_CONFIGURE, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        store->core->configure_runtime(settings);
    });
}
fqdb_status FQDB_CALL fqdb_store_runtime_info_get(fqdb_store *store, fqdb_store_runtime_info *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    auto &c = *store->core;
    *out = {c.background_running.load(), c.background_status.load(),
            c.background_expiry.load(),  c.background_compactions.load(),
            c.group_flushes.load(),      c.group_transactions.load()};
    return FQDB_OK;
}
fqdb_status FQDB_CALL fqdb_store_get_info(fqdb_store *store, fqdb_store_info *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto &core = *store->core;
        std::lock_guard<p::WriterGate> lock(core.writer);
        auto state = core.current();
        *out = {state->version,
                state->durable,
                state->generation->file->size(),
                state->entries,
                state->keys,
                state->live_bytes,
                state->delta ? state->delta->bytes : 0,
                state->generation->pages.info().bytes + state->generation->values.info().bytes,
                core.pin_budget->used(),
                core.transaction_budget->used(),
                core.recovered_transactions,
                core.repaired_bytes};
    });
}
fqdb_status FQDB_CALL fqdb_store_sync(fqdb_store *store) {
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_SYNC, [&](fqdb_log_event *event) {
        if (event) {
            event->database_id = store->core->id;
            event->stage = FQDB_STAGE_FLUSH;
        }
        store->core->sync();
    });
}
fqdb_status FQDB_CALL fqdb_store_checkpoint(fqdb_store *store) {
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_CHECKPOINT, [&](fqdb_log_event *event) {
        if (event) {
            event->database_id = store->core->id;
            event->stage = FQDB_STAGE_INDEX;
        }
        store->core->checkpoint();
    });
}
fqdb_status FQDB_CALL fqdb_store_compact(fqdb_store *store,
                                         const fqdb_store_maintenance_options *options,
                                         fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_COMPACT, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        auto info = store->core->compact(maintenance_options(options), event);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_backup(fqdb_store *store, const char *destination,
                                        const fqdb_store_maintenance_options *options,
                                        fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store || !destination || !*destination)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_BACKUP, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        auto info = store->core->backup(destination, maintenance_options(options), event);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_verify(fqdb_store *store,
                                        const fqdb_store_maintenance_options *options,
                                        fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_VERIFY, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        auto info = store->core->verify(maintenance_options(options), event);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_restore(const char *source, const char *destination,
                                         const fqdb_store_maintenance_options *options,
                                         fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!source || !*source || !destination || !*destination)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_RESTORE, [&](fqdb_log_event *event) {
        auto settings = maintenance_options(options);
        auto info = metered_maintenance(settings, [&] {
            auto open_options = p::defaults();
            open_options.flags = 0;
            auto core = std::make_shared<p::Core>(source, open_options);
            core->open();
            maintenance_event(event, *core);
            return core->backup(destination, settings, event);
        });
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_backup_incremental(fqdb_store *store, const char *destination,
                                                    uint64_t from,
                                                    const fqdb_store_maintenance_options *options,
                                                    fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store || !destination || !*destination)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_BACKUP_INCREMENTAL, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        auto info =
            p::archive_increment(*store->core, destination, from, maintenance_options(options));
        maintenance_result(event, info);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_recover_chain(const char *base, const char *const *archives,
                                               uint32_t count, const char *destination,
                                               uint64_t version, uint64_t time,
                                               const fqdb_store_maintenance_options *options,
                                               fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!base || !*base || !destination || !*destination || count > 1024 || (count && !archives))
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_RECOVER_CHAIN, [&](fqdb_log_event *event) {
        if (event)
            event->stage = FQDB_STAGE_INDEX;
        std::vector<std::string> paths;
        paths.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            p::require(archives[i] && *archives[i], FQDB_INVALID_ARGUMENT);
            paths.emplace_back(archives[i]);
        }
        auto settings = maintenance_options(options);
        auto info = metered_maintenance(settings, [&] {
            return p::recover_chain(base, paths, destination, version, time, settings);
        });
        maintenance_result(event, info);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_index_define(fqdb_store *store, fqdb_slice name, fqdb_slice prefix,
                                              uint32_t field, uint32_t type, uint32_t flags,
                                              const fqdb_store_maintenance_options *options,
                                              fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store || !valid(name, 128) || !name.size || !valid(prefix, 4096) || !field ||
        field > 0x1fffffff || type < 1 || type > 11 || flags > 3)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_INDEX_DEFINE, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        p::IndexRule rule;
        rule.name = key(name);
        rule.prefix = key(prefix);
        rule.field = field;
        rule.type = type;
        rule.flags = flags;
        auto info = p::build_index(*store->core, rule, false, maintenance_options(options));
        maintenance_result(event, info);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_index_define_compound(
    fqdb_store *store, fqdb_slice name, fqdb_slice prefix, const fqdb_store_index_field *fields,
    uint32_t count, uint32_t flags, const fqdb_store_maintenance_options *options,
    fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store || !valid(name, 128) || !name.size || !valid(prefix, 4096) || !fields || !count ||
        count > 8 || flags > 3)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_INDEX_DEFINE, [&](fqdb_log_event *event) {
        p::IndexRule rule;
        rule.name = key(name);
        rule.prefix = key(prefix);
        rule.flags = flags;
        for (uint32_t i = 0; i < count; ++i)
            rule.parts.push_back({fields[i].field, fields[i].type});
        p::require(rule.valid(), FQDB_INVALID_ARGUMENT);
        maintenance_event(event, *store->core);
        auto info = p::build_index(*store->core, rule, false, maintenance_options(options));
        maintenance_result(event, info);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_index_drop(fqdb_store *store, fqdb_slice name,
                                            const fqdb_store_maintenance_options *options,
                                            fqdb_store_maintenance_info *out) {
    if (out)
        *out = {};
    if (!store || !valid(name, 128) || !name.size)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_INDEX_DROP, [&](fqdb_log_event *event) {
        maintenance_event(event, *store->core);
        p::IndexRule rule;
        rule.name = key(name);
        auto info = p::build_index(*store->core, rule, true, maintenance_options(options));
        maintenance_result(event, info);
        if (out)
            *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_purge_expired(fqdb_store *store,
                                               const fqdb_store_expiry_options *options,
                                               fqdb_store_expiry_info *out) {
    if (out)
        *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_PURGE_EXPIRED, [&](fqdb_log_event *event) {
        auto settings = p::expiry_defaults();
        if (options) {
            p::require(options->struct_size >= sizeof(settings) && options->abi_version == 1,
                       FQDB_INVALID_ARGUMENT);
            settings = *options;
        }
        p::require(settings.reserved == 0 && settings.max_keys && settings.max_keys <= 65536 &&
                       settings.max_batch_bytes &&
                       settings.max_batch_bytes <= FQDB_MAX_TRANSACTION_BYTES,
                   FQDB_INVALID_ARGUMENT);
        maintenance_event(event, *store->core);
        auto info = store->core->purge_expired(settings, event);
        if (out)
            *out = info;
    });
}
void FQDB_CALL fqdb_store_cancel_maintenance(fqdb_store *store) {
    if (store)
        store->core->cancel_maintenance();
}
fqdb_status FQDB_CALL fqdb_store_get(fqdb_store *store, fqdb_slice k, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        return read_latest(*store->core, view(k),
                           [&](const p::Value &bytes, const p::Ref &) { copy(bytes, out); });
    });
}
fqdb_status FQDB_CALL fqdb_store_get_view(fqdb_store *store, fqdb_slice k, fqdb_store_value **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        return read_latest(*store->core, view(k), [&](p::Value bytes, const p::Ref &ref) {
            *out = new fqdb_store_value(store->core, std::move(bytes), ref);
        });
    });
}
fqdb_status FQDB_CALL fqdb_store_hash_get_view(fqdb_store *store, fqdb_slice scope, fqdb_slice k,
                                               fqdb_store_value **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store || !valid(scope, 256) || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        auto state = store->core->current();
        if (!(state->checkpoint.features & 1))
            return FQDB_NOT_FOUND;
        auto time = p::unix_ms();
        auto prefix = p::system_prefix() + "C" + p::component(view(scope));
        auto meta = p::try_read(*state, time, prefix);
        if (!meta)
            return FQDB_NOT_FOUND;
        p::require(p::CollectionMeta(*meta).kind == 1, FQDB_CONSTRAINT);
        return read_value(*state, time, prefix + "M" + p::component(view(k)),
                          [&](p::Value bytes, const p::Ref &ref) {
                              *out = new fqdb_store_value(store->core, std::move(bytes), ref);
                          });
    });
}
fqdb_slice FQDB_CALL fqdb_store_value_data(const fqdb_store_value *value) {
    return value ? fqdb_slice{value->value->data(), value->value->size()} : fqdb_slice{};
}
void FQDB_CALL fqdb_store_value_close(fqdb_store_value *value) {
    delete value;
}
fqdb_status FQDB_CALL fqdb_store_snapshot_create(fqdb_store *store, fqdb_store_snapshot **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto result = std::make_unique<fqdb_store_snapshot>();
        result->capture = std::make_shared<p::Capture>(store->core);
        *out = result.release();
    });
}
void FQDB_CALL fqdb_store_snapshot_close(fqdb_store_snapshot *snapshot) {
    delete snapshot;
}
fqdb_status FQDB_CALL fqdb_store_snapshot_get(const fqdb_store_snapshot *snapshot, fqdb_slice k,
                                              fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!snapshot || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    if ((snapshot->capture->view->checkpoint.features & 1) && p::system_key(view(k)))
        return FQDB_NOT_FOUND;
    return boundary([&] {
        auto &capture = *snapshot->capture;
        return read_value(*capture.view, capture.time, view(k),
                          [&](const p::Value &bytes, const p::Ref &) { copy(bytes, out); });
    });
}
fqdb_status FQDB_CALL fqdb_store_snapshot_get_view(const fqdb_store_snapshot *snapshot,
                                                   fqdb_slice k, fqdb_store_value **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!snapshot || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    if ((snapshot->capture->view->checkpoint.features & 1) && p::system_key(view(k)))
        return FQDB_NOT_FOUND;
    return boundary([&] {
        auto &capture = *snapshot->capture;
        return read_value(*capture.view, capture.time, view(k),
                          [&](p::Value bytes, const p::Ref &ref) {
                              *out = new fqdb_store_value(capture.core, std::move(bytes), ref);
                          });
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_begin(fqdb_store *store, uint64_t timeout,
                                           fqdb_store_txn **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { *out = new fqdb_store_txn(store->core, timeout); });
}
void FQDB_CALL fqdb_store_txn_close(fqdb_store_txn *txn) {
    delete txn;
}
void FQDB_CALL fqdb_store_txn_cancel(fqdb_store_txn *txn) {
    if (txn)
        txn->value.cancelled.store(true, std::memory_order_relaxed);
}
fqdb_status FQDB_CALL fqdb_store_txn_execute(fqdb_store_txn *txn, fqdb_slice request,
                                             fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!txn || !valid(request, FQDB_MAX_TRANSACTION_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] {
        p::FeatureRequest parsed(view(request));
        if (parsed.operation == FQDB_SELECT || parsed.operation == FQDB_SELECT_EXPLAIN) {
            p::Charge retained;
            auto result = p::select_query(txn->value, parsed, retained);
            *out = result.encode();
        } else if (parsed.operation == FQDB_INDEX_LOOKUP || parsed.operation == FQDB_INDEX_QUERY ||
                   parsed.operation == FQDB_RECORD_PROJECT) {
            p::Charge retained;
            auto result = p::query_index(txn->value, parsed, retained);
            *out = result.encode();
        } else
            *out = p::execute_feature(txn->value, parsed).encode();
    });
}
// Store-level read-only query on a fresh immutable snapshot: no committable transaction
// and no per-row dependencies. Same encoding as fqdb_store_txn_execute; SELECT/EXPLAIN only.
fqdb_status FQDB_CALL fqdb_store_select(fqdb_store *store, fqdb_slice request, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store || !valid(request, FQDB_MAX_TRANSACTION_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        p::FeatureRequest parsed(view(request));
        p::require(parsed.operation == FQDB_SELECT || parsed.operation == FQDB_SELECT_EXPLAIN,
                   FQDB_INVALID_ARGUMENT);
        p::Transaction tx(store->core, 0);
        tx.track_reads = false;
        p::Charge retained;
        auto result = p::select_query(tx, parsed, retained);
        *out = result.encode();
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_get(fqdb_store_txn *txn, fqdb_slice k, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!txn || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        txn->value.ready();
        if ((txn->value.advanced || (txn->value.base->view->checkpoint.features & 1)) &&
            p::system_key(view(k)))
            return FQDB_NOT_FOUND;
        copy(txn->value.get(key(k)), out);
        return FQDB_OK;
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_put(fqdb_store_txn *txn, fqdb_slice k, fqdb_slice value,
                                         uint64_t expires) {
    if (!txn || !valid(k, FQDB_MAX_KEY_BYTES) || p::system_key(view(k)) ||
        !valid(value, FQDB_MAX_VALUE_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] { txn->value.put(key(k), view(value), expires); });
}
fqdb_status FQDB_CALL fqdb_store_txn_delete(fqdb_store_txn *txn, fqdb_slice k) {
    if (!txn || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] {
        txn->value.ready();
        p::require(!p::system_key(view(k)) ||
                       !(txn->value.advanced || (txn->value.base->view->checkpoint.features & 1)),
                   FQDB_INVALID_ARGUMENT);
        txn->value.erase(key(k));
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_expire(fqdb_store_txn *txn, fqdb_slice k, uint64_t expires) {
    if (!txn || !valid(k, FQDB_MAX_KEY_BYTES) || p::system_key(view(k)))
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] { txn->value.expire(key(k), expires); });
}
fqdb_status FQDB_CALL fqdb_store_txn_patch(fqdb_store_txn *txn, fqdb_slice k, uint64_t offset,
                                           uint64_t remove, fqdb_slice insert) {
    if (!txn || !valid(k, FQDB_MAX_KEY_BYTES) || p::system_key(view(k)) ||
        !valid(insert, FQDB_MAX_VALUE_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] { txn->value.patch(key(k), offset, remove, view(insert)); });
}
fqdb_status FQDB_CALL fqdb_store_txn_savepoint(fqdb_store_txn *txn, uint64_t *id) {
    if (!id)
        return FQDB_INVALID_ARGUMENT;
    *id = 0;
    if (!txn)
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] { *id = txn->value.savepoint(); });
}
fqdb_status FQDB_CALL fqdb_store_txn_rollback_to(fqdb_store_txn *txn, uint64_t id) {
    if (!txn)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { txn->value.rollback_to(id); });
}
fqdb_status FQDB_CALL fqdb_store_txn_release(fqdb_store_txn *txn, uint64_t id) {
    if (!txn)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { txn->value.release(id); });
}
fqdb_status FQDB_CALL fqdb_store_txn_commit(fqdb_store_txn *txn, uint64_t *version,
                                            uint32_t force_reliable) {
    if (!version)
        return FQDB_INVALID_ARGUMENT;
    *version = 0;
    if (!txn || force_reliable > 1)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_COMMIT, [&](fqdb_log_event *event) {
        auto &value = txn->value;
        if (event) {
            event->base_version = value.base->view->version;
            event->durability = value.base->core->options.durability;
            event->stage = FQDB_STAGE_WRITE;
        }
        try {
            *version = value.base->core->commit(value, event, force_reliable != 0);
        } catch (const p::Error &error) {
            if (error.status == FQDB_CONFLICT || error.status == FQDB_COMMIT_UNKNOWN ||
                error.status == FQDB_RECOVERY_REQUIRED || error.status == FQDB_TIMEOUT ||
                error.status == FQDB_CANCELLED)
                value.finish();
            else if (error.status == FQDB_LIMIT)
                value.broken = true;
            throw;
        } catch (...) {
            value.broken = true;
            throw;
        }
        if (event)
            event->version = *version;
    });
}
fqdb_status FQDB_CALL fqdb_store_cursor_open(const fqdb_store_snapshot *snapshot, fqdb_slice begin,
                                             fqdb_slice end, uint32_t reverse,
                                             fqdb_store_cursor **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!snapshot || !valid(begin, FQDB_MAX_KEY_BYTES) || !valid(end, FQDB_MAX_KEY_BYTES) ||
        reverse > 1)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        *out = new fqdb_store_cursor(snapshot->capture, key(begin), key(end), reverse != 0);
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_cursor_open(fqdb_store_txn *txn, fqdb_slice begin,
                                                 fqdb_slice end, uint32_t reverse,
                                                 fqdb_store_cursor **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!txn || !valid(begin, FQDB_MAX_KEY_BYTES) || !valid(end, FQDB_MAX_KEY_BYTES) || reverse > 1)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto low = key(begin), high = key(end);
        raw_range(txn->value, low, high);
        *out = new fqdb_store_cursor(txn->value.base, std::move(low), std::move(high), reverse != 0,
                                     txn->value.writes, false, txn->value.advanced);
    });
}
fqdb_status FQDB_CALL fqdb_store_cursor_next(fqdb_store_cursor *cursor, fqdb_store_row *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!cursor)
        return FQDB_INVALID_ARGUMENT;
    bool ended = false;
    auto status = boundary([&] {
        auto row = cursor->value.next();
        if (row)
            *out = *row;
        else
            ended = true;
    });
    return status == FQDB_OK && ended ? FQDB_END : status;
}
void FQDB_CALL fqdb_store_cursor_close(fqdb_store_cursor *cursor) {
    delete cursor;
}
fqdb_status FQDB_CALL fqdb_store_path_at(const char *root, const char *name, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!root || !name)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto path = p::database_path(root, name);
        p::require(fqdb_buffer_copy({path.data(), path.size()}, out) == FQDB_OK, FQDB_NO_MEMORY);
    });
}
fqdb_status FQDB_CALL fqdb_store_open_at(const char *root, const char *name,
                                         const fqdb_store_options *options, fqdb_store **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!root || !name)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        auto path = p::database_path(root, name);
        if (options && (options->struct_size < sizeof(*options) || options->abi_version != 1 ||
                        options->durability > FQDB_DURABILITY_BUFFERED ||
                        (options->flags & ~(FQDB_STORE_CREATE | FQDB_STORE_SEGMENTED)) ||
                        options->delta_bytes < 65536 || options->writer_bytes < 1048576 ||
                        options->transaction_bytes < 1048576 ||
                        options->all_transaction_bytes < options->transaction_bytes ||
                        options->pinned_bytes < 65536 || !options->max_snapshots))
            return FQDB_INVALID_ARGUMENT;
        auto child = std::filesystem::u8path(path);
        std::error_code error;
        auto state = std::filesystem::symlink_status(child, error);
        p::require(!error || error == std::errc::no_such_file_or_directory, FQDB_IO_ERROR);
        p::require(!std::filesystem::is_symlink(state), FQDB_INVALID_ARGUMENT);
#ifdef _WIN32
        auto attributes = GetFileAttributesW(child.c_str());
        p::require(attributes == INVALID_FILE_ATTRIBUTES ||
                       !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                   FQDB_INVALID_ARGUMENT);
#endif
        if (!options || (options->flags & FQDB_STORE_CREATE)) {
            error.clear();
            std::filesystem::create_directories(child.parent_path(), error);
            p::require(!error, FQDB_IO_ERROR);
        }
        return fqdb_store_open(path.c_str(), options, out);
    });
}
uint64_t FQDB_CALL fqdb_store_value_version(const fqdb_store_value *value) {
    return value ? value->version : 0;
}
uint64_t FQDB_CALL fqdb_store_value_expires_at_ms(const fqdb_store_value *value) {
    return value ? value->expires : 0;
}
fqdb_status FQDB_CALL fqdb_store_get_version(fqdb_store *store, fqdb_slice k, uint64_t *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = 0;
    if (!store || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        auto state = store->core->current();
        if ((state->checkpoint.features & 1) && p::system_key(view(k)))
            return FQDB_NOT_FOUND;
        auto ref = state->find(view(k));
        if (ref)
            *out = ref->version;
        return ref && ref->visible(p::unix_ms()) ? FQDB_OK : FQDB_NOT_FOUND;
    });
}
fqdb_status FQDB_CALL fqdb_store_put(fqdb_store *store, fqdb_slice k, fqdb_slice value,
                                     uint64_t expires, fqdb_store_atomic_info *out,
                                     uint32_t force_reliable) {
    if (!valid(value, FQDB_MAX_VALUE_BYTES)) {
        if (out)
            *out = {};
        return FQDB_INVALID_ARGUMENT;
    }
    return atomic_update(
        store, k, p::AtomicKind::put, 0, out,
        [&](p::AtomicUpdate &u) {
            u.value = view(value);
            u.expires = expires;
        },
        force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_delete(fqdb_store *store, fqdb_slice k,
                                        fqdb_store_atomic_info *out, uint32_t force_reliable) {
    return atomic_update(
        store, k, p::AtomicKind::erase_any, 0, out, [](p::AtomicUpdate &) {}, force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_patch(fqdb_store *store, fqdb_slice k, uint64_t offset,
                                       uint64_t remove, fqdb_slice insert,
                                       fqdb_store_atomic_info *out, uint32_t force_reliable) {
    if (!valid(insert, FQDB_MAX_VALUE_BYTES)) {
        if (out)
            *out = {};
        return FQDB_INVALID_ARGUMENT;
    }
    return atomic_update(
        store, k, p::AtomicKind::patch, 0, out,
        [&](p::AtomicUpdate &u) {
            u.value = view(insert);
            u.offset = offset;
            u.remove = remove;
        },
        force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_write_state_get(fqdb_store *store, fqdb_store_write_state *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto &c = *store->core;
        std::lock_guard<p::WriterGate> lock(c.writer);
        auto v = c.current();
        *out = {v->version, v->durable, c.dirty_bytes.load(std::memory_order_relaxed),
                uint64_t(c.write_failure.load(std::memory_order_acquire))};
    });
}
fqdb_status FQDB_CALL fqdb_store_compare_exchange(fqdb_store *store, fqdb_slice k,
                                                  uint64_t expected, fqdb_slice value,
                                                  uint64_t expires, uint64_t timeout,
                                                  fqdb_store_atomic_info *out,
                                                  uint32_t force_reliable) {
    if (!valid(value, FQDB_MAX_VALUE_BYTES)) {
        if (out)
            *out = {};
        return FQDB_INVALID_ARGUMENT;
    }
    return atomic_update(
        store, k, p::AtomicKind::compare_exchange, timeout, out,
        [&](p::AtomicUpdate &update) {
            update.expected = expected;
            update.value = view(value);
            update.expires = expires;
        },
        force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_delete_if_version(fqdb_store *store, fqdb_slice k,
                                                   uint64_t expected, uint64_t timeout,
                                                   fqdb_store_atomic_info *out,
                                                   uint32_t force_reliable) {
    return atomic_update(
        store, k, p::AtomicKind::erase, timeout, out,
        [&](p::AtomicUpdate &update) { update.expected = expected; }, force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_put_if_changed(fqdb_store *store, fqdb_slice k, fqdb_slice value,
                                                uint64_t expires, uint64_t timeout,
                                                fqdb_store_atomic_info *out,
                                                uint32_t force_reliable) {
    if (!valid(value, FQDB_MAX_VALUE_BYTES)) {
        if (out)
            *out = {};
        return FQDB_INVALID_ARGUMENT;
    }
    return atomic_update(
        store, k, p::AtomicKind::put_changed, timeout, out,
        [&](p::AtomicUpdate &update) {
            update.value = view(value);
            update.expires = expires;
        },
        force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_add_i64(fqdb_store *store, fqdb_slice k, int64_t delta,
                                         int64_t minimum, int64_t maximum, uint64_t timeout,
                                         fqdb_store_atomic_info *out, uint32_t force_reliable) {
    if (minimum > maximum) {
        if (out)
            *out = {};
        return FQDB_INVALID_ARGUMENT;
    }
    return atomic_update(
        store, k, p::AtomicKind::add, timeout, out,
        [&](p::AtomicUpdate &update) {
            update.delta = delta;
            update.minimum = minimum;
            update.maximum = maximum;
        },
        force_reliable);
}
fqdb_status FQDB_CALL fqdb_store_add_i64_fields(fqdb_store *store,
                                                const fqdb_store_i64_field_update *items,
                                                uint32_t count, uint64_t timeout,
                                                fqdb_store_atomic_info *out,
                                                uint32_t force_reliable) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store || !items || !count || count > 256 || force_reliable > 1)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_COMMIT, [&](fqdb_log_event *event) {
        uint64_t bytes = sizeof(p::FieldUpdates) + count * (sizeof(p::FieldUpdates::Item) + 128);
        for (uint32_t i = 0; i < count; ++i) {
            const auto &item = items[i];
            p::require(valid(item.key, FQDB_MAX_KEY_BYTES) && !p::system_key(view(item.key)) &&
                           item.minimum <= item.maximum,
                       FQDB_INVALID_ARGUMENT);
            bytes += item.key.size;
        }
        p::FieldUpdates job(store->core, bytes, timeout);
        job.items.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto &item = items[i];
            job.items.push_back(
                {key(item.key), item.offset, item.delta, item.minimum, item.maximum});
        }
        *out = job.commit(event, force_reliable != 0);
    });
}
fqdb_status FQDB_CALL fqdb_store_history_hold(fqdb_store *store, fqdb_slice name, uint64_t from,
                                              uint64_t timeout, fqdb_store_atomic_info *out) {
    if (out)
        *out = {};
    if (!store || !out || !valid(name, 128) || !name.size)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_CONFIGURE, [&](fqdb_log_event *event) {
        p::AtomicUpdate update(store->core, p::history_prefix() + p::component(view(name)),
                               p::AtomicKind::history_hold, timeout);
        update.expected = from;
        *out = update.commit(event);
    });
}
fqdb_status FQDB_CALL fqdb_store_watch_open(fqdb_store *store, uint64_t from, fqdb_slice prefix,
                                            const fqdb_store_watch_options *options,
                                            fqdb_store_watch **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store || !valid(prefix, FQDB_MAX_KEY_BYTES) ||
        (options && (options->struct_size < sizeof(*options) || options->abi_version != 1)))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto settings = options ? *options : p::watch_defaults();
        p::require(settings.struct_size >= sizeof(settings) && settings.abi_version == 1 &&
                       !(settings.flags & ~3u) && settings.max_scan_commits >= 1 &&
                       settings.max_scan_commits <= 4096 && settings.max_batch_bytes >= 1024 &&
                       settings.max_batch_bytes <= UINT64_C(268435456),
                   FQDB_INVALID_ARGUMENT);
        *out = new fqdb_store_watch(store->core, from, key(prefix), settings);
    });
}
fqdb_status FQDB_CALL fqdb_store_watch_next(fqdb_store_watch *watch, uint64_t timeout,
                                            fqdb_store_change_batch **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!watch)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        auto batch = watch->value.next(timeout);
        if (!batch)
            return FQDB_NOT_MODIFIED;
        *out = new fqdb_store_change_batch{std::move(batch)};
        return FQDB_OK;
    });
}
fqdb_status FQDB_CALL fqdb_store_watch_ack(fqdb_store_watch *watch, uint64_t version) {
    if (!watch)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { watch->value.ack(version); });
}
void FQDB_CALL fqdb_store_watch_cancel(fqdb_store_watch *watch) {
    if (watch)
        watch->value.cancel();
}
void FQDB_CALL fqdb_store_watch_close(fqdb_store_watch *watch) {
    delete watch;
}
const fqdb_store_change_batch_info *FQDB_CALL
fqdb_store_change_batch_data(const fqdb_store_change_batch *batch) {
    return batch ? &batch->value->info : nullptr;
}
void FQDB_CALL fqdb_store_change_batch_close(fqdb_store_change_batch *batch) {
    delete batch;
}
fqdb_status FQDB_CALL fqdb_store_history_release(fqdb_store *store, fqdb_slice name,
                                                 uint64_t timeout, fqdb_store_atomic_info *out) {
    if (out)
        *out = {};
    if (!store || !out || !valid(name, 128) || !name.size)
        return FQDB_INVALID_ARGUMENT;
    return traced(FQDB_LOG_CONFIGURE, [&](fqdb_log_event *event) {
        p::AtomicUpdate update(store->core, p::history_prefix() + p::component(view(name)),
                               p::AtomicKind::history_release, timeout);
        *out = update.commit(event);
    });
}
fqdb_status FQDB_CALL fqdb_store_history_info_get(fqdb_store *store, fqdb_store_history_info *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        auto view = store->core->current();
        auto pins = p::history_pins(*view);
        fqdb_store_history_info info{
            view->version, p::history_floor(*view), pins.minimum, pins.active, pins.total, 0, 0};
        if (view->history)
            for (const auto &segment : view->history->segments) {
                ++info.retained_generations;
                auto bytes = segment.generation->file->size();
                p::require(bytes <= UINT64_MAX - info.retained_file_bytes, FQDB_LIMIT);
                info.retained_file_bytes += bytes;
            }
        *out = info;
    });
}
fqdb_status FQDB_CALL fqdb_store_get_many(fqdb_store *store, const fqdb_slice *keys, uint32_t count,
                                          fqdb_store_results **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        validate_keys(keys, count);
        auto state = store->core->current();
        auto time = p::unix_ms();
        std::optional<p::Table::ReadPath> path;
        if (count >= 8 && store->core->options.cache_bytes)
            path.emplace(std::min<uint64_t>(262144, store->core->options.writer_bytes / 8));
        read_many(store->core, count, out, [&](uint32_t i) {
            return read_item(*state, time, keys[i], path ? &*path : nullptr);
        });
    });
}
fqdb_status FQDB_CALL fqdb_store_get_shared(fqdb_store *store, fqdb_slice key,
                                            fqdb_store_shared_value *out) {
    return shared_read(out,
                       [&](fqdb_store_value **v) { return fqdb_store_get_view(store, key, v); });
}
fqdb_status FQDB_CALL fqdb_store_snapshot_get_shared(const fqdb_store_snapshot *snapshot,
                                                     fqdb_slice key, fqdb_store_shared_value *out) {
    return shared_read(
        out, [&](fqdb_store_value **v) { return fqdb_store_snapshot_get_view(snapshot, key, v); });
}
fqdb_status FQDB_CALL fqdb_store_key_get_shared(fqdb_store_key *key, fqdb_store_shared_value *out) {
    return shared_read(out, [&](fqdb_store_value **v) { return fqdb_store_key_get_view(key, v); });
}
fqdb_status FQDB_CALL fqdb_store_reader_create(fqdb_store *store, const fqdb_slice *keys,
                                               uint32_t count, fqdb_store_reader **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        validate_keys(keys, count);
        *out = new fqdb_store_reader(store->core, keys, count);
    });
}
fqdb_status FQDB_CALL fqdb_store_reader_read(fqdb_store_reader *reader,
                                             const fqdb_store_result_row **rows, uint32_t *count) {
    if (rows)
        *rows = nullptr;
    if (count)
        *count = 0;
    if (!reader || !rows || !count)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        reader_refresh(*reader);
        *rows = reader->result->rows.data();
        *count = static_cast<uint32_t>(reader->result->rows.size());
    });
}
fqdb_status FQDB_CALL fqdb_store_reader_acquire(fqdb_store_reader *reader,
                                                fqdb_store_results **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!reader)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        reader_refresh(*reader);
        read_many(reader->core, static_cast<uint32_t>(reader->keys.size()), out,
                  [&](uint32_t i) { return reader->result->items[i]; });
    });
}
fqdb_status FQDB_CALL fqdb_store_project(fqdb_store *store, fqdb_slice k, const uint32_t *fields,
                                         uint32_t count, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!store || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        p::validate_projection(fields, count);
        auto budget = std::make_shared<p::Budget>(store->core->options.transaction_bytes);
        uint64_t retained = sizeof(uint32_t) * std::min<uint32_t>(count, 256) + 128;
        p::Charge charge(budget, store->core->transaction_budget, retained);
        auto selection = p::projection_fields(fields, count);
        return read_latest(*store->core, view(k), [&](const p::Value &value, const p::Ref &) {
            auto bytes = p::project_fields(*value, selection, charge, retained);
            auto status = fqdb_buffer_copy({bytes.data(), bytes.size()}, out);
            p::require(status == FQDB_OK, status);
        });
    });
}
fqdb_status FQDB_CALL fqdb_store_project_reader_create(fqdb_store *store, const fqdb_slice *keys,
                                                       uint32_t count, const uint32_t *fields,
                                                       uint32_t field_count,
                                                       fqdb_store_reader **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        validate_keys(keys, count);
        p::validate_projection(fields, field_count);
        auto reader = std::make_unique<fqdb_store_reader>(store->core, keys, count);
        uint64_t retained = sizeof(fqdb_store_reader) + 128 +
                            reader->keys.capacity() * sizeof(p::Bytes) +
                            std::min<uint32_t>(field_count, 256) * sizeof(uint32_t);
        for (const auto &key : reader->keys)
            retained += key.capacity() + 32;
        reader->pins.resize(retained);
        reader->projection = p::projection_fields(fields, field_count);
        reader->projected = true;
        *out = reader.release();
    });
}
void FQDB_CALL fqdb_store_reader_release(fqdb_store_reader *reader) {
    if (reader)
        reader->release();
}
void FQDB_CALL fqdb_store_reader_close(fqdb_store_reader *reader) {
    delete reader;
}

fqdb_status FQDB_CALL fqdb_store_snapshot_get_many(const fqdb_store_snapshot *snapshot,
                                                   const fqdb_slice *keys, uint32_t count,
                                                   fqdb_store_results **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!snapshot)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        validate_keys(keys, count);
        const auto &capture = *snapshot->capture;
        std::optional<p::Table::ReadPath> path;
        if (count >= 8 && capture.core->options.cache_bytes)
            path.emplace(std::min<uint64_t>(262144, capture.core->options.writer_bytes / 8));
        read_many(capture.core, count, out, [&](uint32_t i) {
            return read_item(*capture.view, capture.time, keys[i], path ? &*path : nullptr);
        });
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_get_many(fqdb_store_txn *txn, const fqdb_slice *keys,
                                              uint32_t count, fqdb_store_results **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!txn)
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] {
        validate_keys(keys, count);
        txn->value.ready();
        read_many(txn->value.base->core, count, out, [&](uint32_t i) {
            fqdb_store_results::Item item;
            if ((!txn->value.advanced && !(txn->value.base->view->checkpoint.features & 1)) ||
                !p::system_key(view(keys[i])))
                item.value = txn->value.try_get(key(keys[i]), &item.metadata);
            return item;
        });
    });
}
uint32_t FQDB_CALL fqdb_store_results_count(const fqdb_store_results *results) {
    return results ? static_cast<uint32_t>(results->items.size()) : 0;
}
const fqdb_store_result_row *FQDB_CALL fqdb_store_results_data(const fqdb_store_results *results) {
    return results ? results->rows.data() : nullptr;
}
fqdb_status FQDB_CALL fqdb_store_results_get(const fqdb_store_results *results, uint32_t index,
                                             fqdb_store_result_row *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {};
    if (!results || index >= results->items.size())
        return FQDB_INVALID_ARGUMENT;
    const auto &item = results->items[index];
    *out = {item.value ? fqdb_slice{item.value->data(), item.value->size()} : fqdb_slice{},
            item.metadata.version, item.metadata.expires, item.value ? 1u : 0u, 0};
    return FQDB_OK;
}
void FQDB_CALL fqdb_store_results_close(fqdb_store_results *results) {
    delete results;
}
fqdb_status FQDB_CALL fqdb_store_txn_put_many(fqdb_store_txn *txn, const fqdb_store_put_item *items,
                                              uint32_t count) {
    if (!txn)
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] {
        validate_items(items, count);
        txn->value.ready();
        for (uint32_t i = 0; i < count; ++i)
            txn->value.put(key(items[i].key), view(items[i].value), items[i].expires_at_ms);
    });
}
fqdb_status FQDB_CALL fqdb_store_txn_delete_many(fqdb_store_txn *txn, const fqdb_slice *keys,
                                                 uint32_t count) {
    if (!txn)
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] {
        validate_keys(keys, count, true);
        txn->value.ready();
        for (uint32_t i = 0; i < count; ++i)
            txn->value.erase(key(keys[i]));
    });
}
fqdb_status FQDB_CALL fqdb_store_batch_create(fqdb_store *store, const fqdb_store_put_item *items,
                                              uint32_t count, fqdb_store_batch **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] {
        validate_items(items, count);
        std::vector<uint32_t> order(count);
        for (uint32_t i = 0; i < count; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            auto compare = p::compare(view(items[a].key), view(items[b].key));
            return compare ? compare < 0 : a < b;
        });
        auto data = std::make_shared<p::PreparedBatch>(*store->core);
        data->items.reserve(count);
        uint64_t metadata = sizeof(p::PreparedBatch) +
                            data->items.capacity() * sizeof(p::PreparedBatch::Item) + 128;
        uint64_t values = 0;
        data->pins.resize(metadata);
        for (uint32_t i = 0; i < count; ++i) {
            auto index = order[i];
            if (i + 1 < count && view(items[index].key) == view(items[order[i + 1]].key))
                continue;
            auto name = key(items[index].key);
            metadata += name.capacity() + 64;
            data->pins.resize(metadata);
            auto value = std::make_shared<p::StagedData>(store->core->pin_budget,
                                                         std::shared_ptr<p::Budget>{});
            value->capacity_for(static_cast<size_t>(items[index].value.size));
            value->assign(view(items[index].value));
            (void)p::value_crc(value);
            values += sizeof(p::StagedData) + value->capacity() + 64;
            data->items.push_back({std::move(name), std::move(value), items[index].expires_at_ms});
        }
        data->bytes = metadata + values;
        *out = new fqdb_store_batch{store->core, std::move(data)};
    });
}
void FQDB_CALL fqdb_store_batch_close(fqdb_store_batch *batch) {
    delete batch;
}
fqdb_status FQDB_CALL fqdb_store_txn_put_batch(fqdb_store_txn *txn, const fqdb_store_batch *batch) {
    if (!txn || !batch)
        return FQDB_INVALID_ARGUMENT;
    return staging(txn, [&] {
        txn->value.ready();
        p::require(txn->value.base->core == batch->core, FQDB_INVALID_ARGUMENT);
        batch->data->apply(txn->value, batch->data);
    });
}
fqdb_status FQDB_CALL fqdb_store_key_prepare(fqdb_store *store, fqdb_slice k,
                                             fqdb_store_key **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!store || !valid(k, FQDB_MAX_KEY_BYTES))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { *out = new fqdb_store_key(store->core, key(k)); });
}
void FQDB_CALL fqdb_store_key_close(fqdb_store_key *key) {
    delete key;
}
fqdb_status FQDB_CALL fqdb_store_key_get_view(fqdb_store_key *key, fqdb_store_value **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!key)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        return read_prepared(
            *key, [](const p::Ref &) { return FQDB_OK; },
            [&](p::Value bytes, const p::Ref &ref) {
                *out = new fqdb_store_value(key->core, std::move(bytes), ref);
            });
    });
}
fqdb_status FQDB_CALL fqdb_store_key_get_into(fqdb_store_key *key, void *destination,
                                              uint64_t capacity, uint64_t *required) {
    if (!required)
        return FQDB_INVALID_ARGUMENT;
    *required = 0;
    if (!key || (capacity && !destination) || capacity > SIZE_MAX)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        return read_prepared(
            *key,
            [&](const p::Ref &ref) {
                *required = ref.size;
                return capacity < ref.size ? FQDB_BUFFER_TOO_SMALL : FQDB_OK;
            },
            [&](const p::Value &bytes, const p::Ref &) {
                if (!bytes->empty())
                    std::memcpy(destination, bytes->data(), bytes->size());
            });
    });
}
fqdb_status FQDB_CALL fqdb_store_snapshot_get_prepared_view(const fqdb_store_snapshot *snapshot,
                                                            fqdb_store_key *key,
                                                            fqdb_store_value **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!snapshot || !key || snapshot->capture->core != key->core)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        const auto &capture = *snapshot->capture;
        auto ref = key->resolve(capture.view);
        if (!ref || !ref->visible(capture.time))
            return FQDB_NOT_FOUND;
        *out = new fqdb_store_value(
            capture.core, capture.view->generation->value(*ref, capture.view->limit), *ref);
        return FQDB_OK;
    });
}
}

namespace fengqudb::paged {
Core::~Core() {
    stop_worker();
    stop_periodic();
}
void Core::stop_worker_unlocked() {
    background_stop.store(true, std::memory_order_release);
    background_changed.notify_all();
    if (background_thread.joinable())
        background_thread.join();
}
void Core::stop_worker() {
    std::lock_guard<std::mutex> lock(configuration_mutex);
    stop_worker_unlocked();
}
void Core::configure_runtime(const fqdb_store_runtime_options &settings) {
    std::lock_guard<std::mutex> lock(configuration_mutex);
    stop_worker_unlocked();
    runtime = settings;
    group_max.store(settings.group_max);
    group_delay_us.store(settings.group_delay_us);
    background_stop.store(false, std::memory_order_release);
    if (settings.background)
        background_thread = std::thread([this] { background_loop(); });
}
void Core::background_loop() {
    background_maintenance = true;
    background_running.store(true, std::memory_order_release);
    uint64_t compacted_version = 0, compacted_size = 0;
    uint64_t compacted_generation = 0;
    bool sparse_supported = true;
    std::unique_ptr<ReclaimSession> reclaim;
    std::chrono::steady_clock::time_point reader_wait{};
    Bytes last_rule;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(background_mutex);
            background_changed.wait_for(lock, std::chrono::milliseconds(runtime.interval_ms), [&] {
                return background_stop.load(std::memory_order_acquire) ||
                       (reclaim && reclaim_state.load(std::memory_order_acquire) == 2);
            });
            if (background_stop.load(std::memory_order_acquire))
                break;
        }
        if (reclaim && reclaim_state.load(std::memory_order_acquire) == 2) {
            reclaim.reset();
            reader_wait = {};
            background_status.store(FQDB_CANCELLED);
            continue;
        }
        auto cycle_policy = maintenance_defaults();
        cycle_policy.max_read_bytes = runtime.job_max_read_bytes;
        cycle_policy.max_write_bytes = runtime.job_max_write_bytes;
        MaintenanceIO cycle_io(cycle_policy);
        try {
            auto expiry = expiry_defaults();
            expiry.max_keys = runtime.expiry_keys;
            expiry.timeout_ms = runtime.job_timeout_ms;
            fqdb_store_expiry_info purged{};
            fqdb_status status = FQDB_OK;
            try {
                purged = purge_expired(expiry);
            } catch (...) {
                status = current_error();
            }
            if (status != FQDB_LOCKED)
                background_status.store(status);
            if (!status)
                background_expiry.fetch_add(1);
            if (background_stop.load())
                break;
            auto view = current();
            if (view->checkpoint.features & 1) {
                auto prefix = catalog_prefix();
                ViewCursor schemas(*view, prefix);
                std::optional<IndexRule> selected, fallback;
                while (auto row = schemas.next()) {
                    if (row->key.substr(0, prefix.size()) != prefix)
                        break;
                    if (row->value.deleted())
                        continue;
                    auto bytes = view->generation->value(row->value, view->limit, false, false);
                    IndexRule rule(*bytes);
                    if (rule.ready)
                        continue;
                    if (!fallback)
                        fallback = rule;
                    if (compare(rule.name, last_rule) > 0) {
                        selected = std::move(rule);
                        break;
                    }
                }
                if (!selected)
                    selected = std::move(fallback);
                if (selected) {
                    last_rule = selected->name;
                    auto policy = maintenance_defaults();
                    policy.timeout_ms = runtime.job_timeout_ms;
                    try {
                        (void)build_index(*this, *selected, false, policy);
                        status = FQDB_OK;
                    } catch (...) {
                        status = current_error();
                    }
                    if (status != FQDB_LOCKED)
                        background_status.store(status);
                }
            }
            if (background_stop.load())
                break;
            view = current();
            auto size = view->generation->file->size();
            if (compacted_generation != view->generation->number) {
                compacted_generation = view->generation->number;
                compacted_version = compacted_size = 0;
            }
            auto allocated = sparse_supported ? view->generation->file->allocated() : size;
            // Sparse allocation metadata can lag cached appends on Windows.
            // Bytes beyond the last durable checkpoint are another known
            // lower bound on pending allocation; do not flush merely to poll it.
            const auto unsealed = size >= view->checkpoint.end ? size - view->checkpoint.end : 0;
            allocated = std::max(allocated, unsealed);
            const bool needed =
                view->version > compacted_version && allocated >= runtime.compact_min_bytes &&
                size >= compacted_size && size - compacted_size >= runtime.compact_growth_bytes &&
                view->live_bytes <= allocated - (allocated / 100) * runtime.garbage_percent;
            view.reset(); // A trigger snapshot must not pin the pre-floor epoch.
            if (reclaim || needed) {
                auto policy = maintenance_defaults();
                policy.timeout_ms = runtime.job_timeout_ms;
                bool fallback = !sparse_supported;
                if (sparse_supported) {
                    try {
                        if (!reclaim)
                            reclaim = std::make_unique<ReclaimSession>(*this);
                        if (reclaim->step(policy)) {
                            compacted_version = reclaim->version();
                            compacted_size = reclaim->limit();
                            reclaim.reset();
                            reader_wait = {};
                            background_compactions.fetch_add(1);
                        } else if (reclaim->waiting_for_readers()) {
                            const auto now = std::chrono::steady_clock::now();
                            if (reader_wait == std::chrono::steady_clock::time_point{})
                                reader_wait = now;
                            const auto wait = std::chrono::milliseconds(
                                std::max<uint64_t>(1000, uint64_t{runtime.interval_ms} * 2));
                            if (now - reader_wait >= wait) {
                                reclaim.reset();
                                reader_wait = {};
                                fallback = true;
                            }
                        } else
                            reader_wait = {};
                        status = FQDB_OK;
                    } catch (const IOBudgetStop &) {
                        status =
                            FQDB_LIMIT; // Keep the exact mark/sweep position for the next slice.
                    } catch (...) {
                        status = current_error();
                        if (status != FQDB_TIMEOUT) {
                            reclaim.reset();
                            reader_wait = {};
                            if (status == FQDB_UNSUPPORTED)
                                sparse_supported = false;
                            fallback = status == FQDB_UNSUPPORTED || status == FQDB_LOCKED;
                        }
                    }
                }
                if (fallback) {
                    try {
                        const auto info = compact(policy);
                        compacted_generation = current()->generation->number;
                        compacted_version = info.final_version;
                        compacted_size = info.output_file_bytes;
                        background_compactions.fetch_add(1);
                        status = FQDB_OK;
                    } catch (...) {
                        status = current_error();
                    }
                }
                if (status != FQDB_LOCKED)
                    background_status.store(status);
            }
        } catch (...) {
            background_status.store(current_error());
        }
    }
    reclaim.reset();
    background_running.store(false, std::memory_order_release);
    background_maintenance = false;
}
} // namespace fengqudb::paged
