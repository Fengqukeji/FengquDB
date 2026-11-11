#ifndef FENGQUDB_STORE_HPP
#define FENGQUDB_STORE_HPP
#include "fengqudb.hpp"
#include "fengqudb_store.h"
#include "fengqudb_wire.hpp"

namespace fengqudb {
struct StoreOptions {
    Durability durability = Durability::sync;
    bool create = true;
    uint64_t cache_bytes = 64 * 1024 * 1024, delta_bytes = 4 * 1024 * 1024,
             writer_bytes = 32 * 1024 * 1024;
    uint64_t transaction_bytes = 64 * 1024 * 1024, all_transaction_bytes = 256 * 1024 * 1024;
    uint64_t pinned_bytes = 128 * 1024 * 1024, max_snapshots = 1024;
    bool segmented = false;
    fqdb_store_options native() const noexcept {
        return {sizeof(fqdb_store_options),
                1,
                static_cast<uint32_t>(durability),
                (create ? static_cast<uint32_t>(FQDB_STORE_CREATE) : 0u) |
                    (segmented ? static_cast<uint32_t>(FQDB_STORE_SEGMENTED) : 0u),
                cache_bytes,
                delta_bytes,
                writer_bytes,
                transaction_bytes,
                all_transaction_bytes,
                pinned_bytes,
                max_snapshots};
    }
};
using StoreInfo = fqdb_store_info;
using StoreMaintenanceInfo = fqdb_store_maintenance_info;
using StoreMaintenanceIO = fqdb_store_maintenance_io;
struct StoreMaintenanceOptions {
    uint32_t max_catchup_rounds = 8;
    uint64_t timeout_ms = 0, max_barrier_bytes = 1048576;
    uint64_t max_read_bytes = 0, max_write_bytes = 0;
    StoreMaintenanceIO *io_stats = nullptr;
    fqdb_store_maintenance_options native() const noexcept {
        return {sizeof(fqdb_store_maintenance_options),
                (max_read_bytes || max_write_bytes || io_stats) ? 2u : 1u,
                max_catchup_rounds,
                0,
                timeout_ms,
                max_barrier_bytes,
                max_read_bytes,
                max_write_bytes,
                io_stats};
    }
};
using StoreExpiryInfo = fqdb_store_expiry_info;
struct StoreExpiryOptions {
    uint32_t max_keys = 1024;
    uint64_t timeout_ms = 0, max_batch_bytes = 1048576;
    fqdb_store_expiry_options native() const noexcept {
        return {sizeof(fqdb_store_expiry_options), 1, max_keys, 0, timeout_ms, max_batch_bytes};
    }
};
struct StoreRuntimeOptions {
    bool background = false;
    uint32_t interval_ms = 200, expiry_keys = 256, group_max = 128, group_delay_us = 0,
             garbage_percent = 30;
    uint64_t compact_min_bytes = 33554432, compact_growth_bytes = 33554432, job_timeout_ms = 1000;
    uint64_t job_max_read_bytes = 0, job_max_write_bytes = 0;
    fqdb_store_runtime_options native() const noexcept {
        return {sizeof(fqdb_store_runtime_options),
                (job_max_read_bytes || job_max_write_bytes) ? 2u : 1u,
                background ? 1u : 0u,
                interval_ms,
                expiry_keys,
                group_max,
                group_delay_us,
                garbage_percent,
                compact_min_bytes,
                compact_growth_bytes,
                job_timeout_ms,
                job_max_read_bytes,
                job_max_write_bytes};
    }
};
using StoreRuntimeInfo = fqdb_store_runtime_info;
struct StoreCommand {
    uint32_t operation;
    std::string_view scope, key = {}, value = {};
    int64_t argument = 0;
    double score = 0;
    uint64_t limit = 256;
    uint32_t flags = 0;
    std::string encode() const {
        wire::Writer out;
        out.reserve(wire::bytes_size(2, scope.size()) + wire::bytes_size(3, key.size()) +
                    wire::bytes_size(4, value.size()) + 29 + wire::varint_size(operation) +
                    wire::varint_size(flags));
        out.uint_field(1, operation);
        out.bytes_field(2, scope);
        out.bytes_field(3, key);
        out.bytes_field(4, value);
        out.fixed64_field(5, static_cast<uint64_t>(argument));
        out.double_field(6, score);
        out.fixed64_field(7, limit);
        out.uint_field(8, flags);
        return out.take();
    }
};
using StoreIndexField = fqdb_store_index_field;
struct StoreIndexQuery {
    std::string equal;
    std::optional<std::string> lower, upper;
    bool lower_exclusive = false, upper_inclusive = false, reverse = false, include_values = false;
    std::vector<uint32_t> fields;
    std::string encode() const {
        wire::Writer out;
        out.bytes_field(1, equal);
        if (lower)
            out.bytes_field(2, *lower);
        if (upper)
            out.bytes_field(3, *upper);
        out.uint_field(4, (lower_exclusive ? 1u : 0u) | (upper_inclusive ? 2u : 0u) |
                              (reverse ? 4u : 0u) | (include_values ? 8u : 0u));
        detail::check(fields.size() <= 256 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
        for (auto field : fields)
            out.uint_field(5, field);
        return out.take();
    }
};
struct StoreFeatureRow {
    std::string key, value;
    double score = 0;
};
struct StoreCommandResult {
    uint64_t version = 0, count = 0;
    bool changed = false;
    std::optional<std::string> value;
    std::string cursor;
    std::vector<StoreFeatureRow> rows;
    StoreCommandResult() = default;
    explicit StoreCommandResult(std::string_view bytes) {
        wire::Reader reader(bytes);
        wire::Field field;
        while (reader.next(field)) {
            if (field.number == 1) {
                std::string_view v;
                detail::check(reader.bytes(v) ? FQDB_OK : FQDB_CORRUPT);
                value = std::string(v);
            } else if (field.number == 2)
                detail::check(reader.fixed(count, 8) ? FQDB_OK : FQDB_CORRUPT);
            else if (field.number == 3) {
                uint64_t v = 0;
                detail::check(reader.varint(v) ? FQDB_OK : FQDB_CORRUPT);
                changed = v != 0;
            } else if (field.number == 4) {
                std::string_view v;
                detail::check(reader.bytes(v) ? FQDB_OK : FQDB_CORRUPT);
                wire::Reader item(v);
                wire::Field part;
                StoreFeatureRow row;
                while (item.next(part)) {
                    if (part.number == 1 || part.number == 2) {
                        std::string_view text;
                        detail::check(item.bytes(text) ? FQDB_OK : FQDB_CORRUPT);
                        (part.number == 1 ? row.key : row.value).assign(text);
                    } else if (part.number == 3) {
                        uint64_t bits = 0;
                        detail::check(item.fixed(bits, 8) ? FQDB_OK : FQDB_CORRUPT);
                        std::memcpy(&row.score, &bits, 8);
                    } else
                        detail::check(item.skip(part) ? FQDB_OK : FQDB_CORRUPT);
                }
                detail::check(item.done() ? FQDB_OK : FQDB_CORRUPT);
                rows.push_back(std::move(row));
            } else if (field.number == 5) {
                std::string_view v;
                detail::check(reader.bytes(v) ? FQDB_OK : FQDB_CORRUPT);
                cursor.assign(v);
            } else
                detail::check(reader.skip(field) ? FQDB_OK : FQDB_CORRUPT);
        }
        detail::check(reader.done() ? FQDB_OK : FQDB_CORRUPT);
    }
};
class Store;
class StoreSnapshot;
class StoreTransaction;
class StorePreparedKey;
class StoreBatch;
struct StoreWatchOptions {
    uint32_t flags = 0, max_scan_commits = 256;
    uint64_t max_batch_bytes = FQDB_MAX_TRANSACTION_BYTES;
    fqdb_store_watch_options native() const noexcept {
        return {sizeof(fqdb_store_watch_options), 1, flags, max_scan_commits, max_batch_bytes};
    }
};
class StoreChanges {
    std::unique_ptr<fqdb_store_change_batch,
                    detail::Deleter<fqdb_store_change_batch, fqdb_store_change_batch_close>>
        handle_;
    explicit StoreChanges(fqdb_store_change_batch *batch) : handle_(batch) {}
    friend class StoreWatch;

  public:
    StoreChanges(StoreChanges &&) noexcept = default;
    StoreChanges &operator=(StoreChanges &&) noexcept = default;
    const fqdb_store_change_batch_info &info() const {
        detail::require(handle_.get());
        return *fqdb_store_change_batch_data(handle_.get());
    }
    const fqdb_store_change_row *begin() const {
        return info().rows;
    }
    const fqdb_store_change_row *end() const {
        const auto &data = info();
        return data.count ? data.rows + data.count : data.rows;
    }
};
class StoreWatch {
    std::unique_ptr<fqdb_store_watch, detail::Deleter<fqdb_store_watch, fqdb_store_watch_close>>
        handle_;
    explicit StoreWatch(fqdb_store_watch *watch) : handle_(watch) {}
    friend class Store;

  public:
    StoreWatch(StoreWatch &&) noexcept = default;
    StoreWatch &operator=(StoreWatch &&) noexcept = default;
    std::optional<StoreChanges> next(uint64_t timeout_ms = 0) {
        detail::require(handle_.get());
        fqdb_store_change_batch *batch = nullptr;
        auto status = fqdb_store_watch_next(handle_.get(), timeout_ms, &batch);
        if (status == FQDB_NOT_MODIFIED || status == FQDB_TIMEOUT)
            return {};
        detail::check(status);
        return StoreChanges(batch);
    }
    void ack(uint64_t version) {
        detail::require(handle_.get());
        detail::check(fqdb_store_watch_ack(handle_.get(), version));
    }
    void cancel() {
        detail::require(handle_.get());
        fqdb_store_watch_cancel(handle_.get());
    }
};
using StoreAtomicInfo = fqdb_store_atomic_info;
struct StoreI64FieldUpdate {
    std::string_view key;
    uint64_t offset;
    int64_t delta, minimum = INT64_MIN, maximum = INT64_MAX;
};
using StoreHistoryInfo = fqdb_store_history_info;
struct StorePut {
    std::string_view key, value;
    uint64_t expires_at_ms = 0;
};
struct StoreResult {
    std::optional<std::string_view> value;
    uint64_t version = 0, expires_at_ms = 0;
};
namespace detail {
inline std::vector<fqdb_slice> store_keys(const std::vector<std::string_view> &keys) {
    check(keys.size() <= 4096 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
    std::vector<fqdb_slice> out;
    out.reserve(keys.size());
    for (auto key : keys)
        out.push_back(slice(key));
    return out;
}
inline std::vector<fqdb_store_put_item> store_items(const std::vector<StorePut> &items) {
    check(items.size() <= 4096 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
    std::vector<fqdb_store_put_item> out;
    out.reserve(items.size());
    for (const auto &item : items)
        out.push_back({slice(item.key), slice(item.value), item.expires_at_ms});
    return out;
}
} // namespace detail
class StoreValues {
    std::unique_ptr<fqdb_store_results,
                    detail::Deleter<fqdb_store_results, fqdb_store_results_close>>
        handle_;
    explicit StoreValues(fqdb_store_results *handle) : handle_(handle) {}
    friend class Store;
    friend class StoreTransaction;
    friend class StoreSnapshot;
    friend class StoreReader;

  public:
    StoreValues(StoreValues &&) noexcept = default;
    StoreValues &operator=(StoreValues &&) noexcept = default;
    uint32_t size() const {
        detail::require(handle_.get());
        return fqdb_store_results_count(handle_.get());
    }
    StoreResult operator[](uint32_t index) const {
        detail::require(handle_.get());
        fqdb_store_result_row row{};
        detail::check(fqdb_store_results_get(handle_.get(), index, &row));
        return {row.found ? std::optional<std::string_view>(detail::view(row.value)) : std::nullopt,
                row.version, row.expires_at_ms};
    }
};
// Borrowed data: invalidated by the reader's next read/acquire/release/destruction.
class StoreReadView {
    const fqdb_store_result_row *rows_;
    uint32_t count_;

  public:
    StoreReadView(const fqdb_store_result_row *rows, uint32_t count) : rows_(rows), count_(count) {}
    uint32_t size() const noexcept {
        return count_;
    }
    StoreResult operator[](uint32_t index) const {
        detail::check(index < count_ ? FQDB_OK : FQDB_INVALID_ARGUMENT);
        const auto &row = rows_[index];
        return {row.found ? std::optional<std::string_view>(detail::view(row.value)) : std::nullopt,
                row.version, row.expires_at_ms};
    }
};
class StoreReader {
    std::unique_ptr<fqdb_store_reader, detail::Deleter<fqdb_store_reader, fqdb_store_reader_close>>
        handle_;
    explicit StoreReader(fqdb_store_reader *handle) : handle_(handle) {}
    friend class Store;

  public:
    StoreReader(StoreReader &&) noexcept = default;
    StoreReader &operator=(StoreReader &&) noexcept = default;
    StoreReadView read() {
        detail::require(handle_.get());
        const fqdb_store_result_row *rows = nullptr;
        uint32_t count = 0;
        detail::check(fqdb_store_reader_read(handle_.get(), &rows, &count));
        return {rows, count};
    }
    StoreValues acquire() {
        detail::require(handle_.get());
        fqdb_store_results *out = nullptr;
        detail::check(fqdb_store_reader_acquire(handle_.get(), &out));
        return StoreValues(out);
    }
    void release() {
        detail::require(handle_.get());
        fqdb_store_reader_release(handle_.get());
    }
};
class StoreBatch {
    std::unique_ptr<fqdb_store_batch, detail::Deleter<fqdb_store_batch, fqdb_store_batch_close>>
        handle_;
    explicit StoreBatch(fqdb_store_batch *handle) : handle_(handle) {}
    friend class Store;
    friend class StoreTransaction;

  public:
    StoreBatch(StoreBatch &&) noexcept = default;
    StoreBatch &operator=(StoreBatch &&) noexcept = default;
};
class StoreValue {
    std::unique_ptr<fqdb_store_value, detail::Deleter<fqdb_store_value, fqdb_store_value_close>>
        handle_;
    explicit StoreValue(fqdb_store_value *handle) : handle_(handle) {}
    friend class Store;
    friend class StoreSnapshot;
    friend class StorePreparedKey;

  public:
    StoreValue(StoreValue &&) noexcept = default;
    StoreValue &operator=(StoreValue &&) noexcept = default;
    std::string_view view() const {
        detail::require(handle_.get());
        return detail::view(fqdb_store_value_data(handle_.get()));
    }
    std::string string() const {
        return std::string(view());
    }
    uint64_t version() const {
        detail::require(handle_.get());
        return fqdb_store_value_version(handle_.get());
    }
    uint64_t expires_at_ms() const {
        detail::require(handle_.get());
        return fqdb_store_value_expires_at_ms(handle_.get());
    }
};
class StorePreparedKey {
    std::unique_ptr<fqdb_store_key, detail::Deleter<fqdb_store_key, fqdb_store_key_close>> handle_;
    explicit StorePreparedKey(fqdb_store_key *handle) : handle_(handle) {}
    friend class Store;
    friend class StoreSnapshot;

  public:
    StorePreparedKey(StorePreparedKey &&) noexcept = default;
    StorePreparedKey &operator=(StorePreparedKey &&) noexcept = default;
    std::optional<StoreValue> get() const {
        detail::require(handle_.get());
        fqdb_store_value *out = nullptr;
        auto status = fqdb_store_key_get_view(handle_.get(), &out);
        if (status == FQDB_NOT_FOUND)
            return {};
        detail::check(status);
        return StoreValue(out);
    }
    bool get_into(void *destination, uint64_t capacity, uint64_t &required) const {
        detail::require(handle_.get());
        auto status = fqdb_store_key_get_into(handle_.get(), destination, capacity, &required);
        if (status == FQDB_NOT_FOUND)
            return false;
        detail::check(status);
        return true;
    }
};
struct StoreRow {
    std::string_view key, value;
    uint64_t version, expires_at_ms;
};
class StoreCursor {
    std::unique_ptr<fqdb_store_cursor, detail::Deleter<fqdb_store_cursor, fqdb_store_cursor_close>>
        handle_;
    explicit StoreCursor(fqdb_store_cursor *handle) : handle_(handle) {}
    friend class StoreSnapshot;
    friend class StoreTransaction;

  public:
    StoreCursor(StoreCursor &&) noexcept = default;
    StoreCursor &operator=(StoreCursor &&) noexcept = default;
    // Borrowed row slices last until the next call or cursor destruction.
    std::optional<StoreRow> next() {
        detail::require(handle_.get());
        fqdb_store_row row{};
        auto status = fqdb_store_cursor_next(handle_.get(), &row);
        if (status == FQDB_END)
            return {};
        detail::check(status);
        return StoreRow{detail::view(row.key), detail::view(row.value), row.version,
                        row.expires_at_ms};
    }
};
class StoreSnapshot {
    std::unique_ptr<fqdb_store_snapshot,
                    detail::Deleter<fqdb_store_snapshot, fqdb_store_snapshot_close>>
        handle_;
    explicit StoreSnapshot(fqdb_store_snapshot *handle) : handle_(handle) {}
    friend class Store;

  public:
    StoreValues get_many(const std::vector<std::string_view> &keys) const {
        detail::require(handle_.get());
        auto input = detail::store_keys(keys);
        fqdb_store_results *out = nullptr;
        detail::check(fqdb_store_snapshot_get_many(handle_.get(), input.data(),
                                                   static_cast<uint32_t>(input.size()), &out));
        return StoreValues(out);
    }
    std::optional<StoreValue> get(const StorePreparedKey &key) const {
        detail::require(handle_.get());
        detail::require(key.handle_.get());
        fqdb_store_value *out = nullptr;
        auto status = fqdb_store_snapshot_get_prepared_view(handle_.get(), key.handle_.get(), &out);
        if (status == FQDB_NOT_FOUND)
            return {};
        detail::check(status);
        return StoreValue(out);
    }
    StoreSnapshot(StoreSnapshot &&) noexcept = default;
    StoreSnapshot &operator=(StoreSnapshot &&) noexcept = default;
    std::optional<StoreValue> get(std::string_view key) const {
        detail::require(handle_.get());
        fqdb_store_value *value = nullptr;
        auto status = fqdb_store_snapshot_get_view(handle_.get(), detail::slice(key), &value);
        if (status == FQDB_NOT_FOUND)
            return {};
        detail::check(status);
        return StoreValue(value);
    }
    StoreCursor scan(std::string_view begin = {}, std::string_view end = {},
                     bool reverse = false) const {
        detail::require(handle_.get());
        fqdb_store_cursor *cursor = nullptr;
        detail::check(fqdb_store_cursor_open(handle_.get(), detail::slice(begin),
                                             detail::slice(end), reverse ? 1u : 0u, &cursor));
        return StoreCursor(cursor);
    }
};
enum class StoreWriteMode : uint32_t { Reliable = 0, Performance = 1 };
using StoreWriteState = fqdb_store_write_state;
struct StoreWriteOptions {
    StoreWriteMode mode = StoreWriteMode::Performance;
    uint32_t period_ms = 50;
    uint64_t threshold_bytes = 1048576;
    fqdb_store_write_options native() const {
        return {sizeof(fqdb_store_write_options), 1, static_cast<uint32_t>(mode), period_ms,
                threshold_bytes};
    }
};
class StoreCommit {
    std::unique_ptr<fqdb_store_commit, detail::Deleter<fqdb_store_commit, fqdb_store_commit_close>>
        handle_;

  public:
    explicit StoreCommit(fqdb_store_commit *handle) : handle_(handle) {}
    StoreCommit(StoreCommit &&) noexcept = default;
    StoreCommit &operator=(StoreCommit &&) noexcept = default;
    std::optional<CommitResult> poll() const {
        detail::require(handle_.get());
        fqdb_status result;
        uint64_t version;
        auto status = fqdb_store_commit_poll(handle_.get(), &result, &version);
        if (status == FQDB_NOT_MODIFIED)
            return {};
        detail::check(status);
        return CommitResult{static_cast<Status>(result), version};
    }
    CommitResult wait(uint64_t timeout_ms = 0) const {
        detail::require(handle_.get());
        fqdb_status result;
        uint64_t version;
        detail::check(fqdb_store_commit_wait(handle_.get(), timeout_ms, &result, &version));
        return {static_cast<Status>(result), version};
    }
    void cancel() const {
        detail::require(handle_.get());
        fqdb_store_commit_cancel(handle_.get());
    }
};
class StoreTransaction {
    std::unique_ptr<fqdb_store_txn, detail::Deleter<fqdb_store_txn, fqdb_store_txn_close>> handle_;
    explicit StoreTransaction(fqdb_store_txn *handle) : handle_(handle) {}
    friend class Store;

  public:
    StoreValues get_many(const std::vector<std::string_view> &keys) {
        detail::require(handle_.get());
        auto input = detail::store_keys(keys);
        fqdb_store_results *out = nullptr;
        detail::check(fqdb_store_txn_get_many(handle_.get(), input.data(),
                                              static_cast<uint32_t>(input.size()), &out));
        return StoreValues(out);
    }
    void put_many(const std::vector<StorePut> &items) {
        detail::require(handle_.get());
        auto input = detail::store_items(items);
        detail::check(fqdb_store_txn_put_many(handle_.get(), input.data(),
                                              static_cast<uint32_t>(input.size())));
    }
    void delete_many(const std::vector<std::string_view> &keys) {
        detail::require(handle_.get());
        auto input = detail::store_keys(keys);
        detail::check(fqdb_store_txn_delete_many(handle_.get(), input.data(),
                                                 static_cast<uint32_t>(input.size())));
    }
    void put_batch(const StoreBatch &batch) {
        detail::require(handle_.get());
        detail::require(batch.handle_.get());
        detail::check(fqdb_store_txn_put_batch(handle_.get(), batch.handle_.get()));
    }
    StoreTransaction(StoreTransaction &&) noexcept = default;
    StoreTransaction &operator=(StoreTransaction &&) noexcept = default;
    std::optional<std::string> get(std::string_view key) {
        detail::require(handle_.get());
        fqdb_buffer value{};
        auto status = fqdb_store_txn_get(handle_.get(), detail::slice(key), &value);
        if (status == FQDB_NOT_FOUND)
            return {};
        detail::check(status);
        struct Free {
            fqdb_buffer *value;
            ~Free() {
                fqdb_buffer_free(value);
            }
        } free{&value};
        return std::string(detail::view({value.data, value.size}));
    }
    void put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0) {
        detail::require(handle_.get());
        detail::check(fqdb_store_txn_put(handle_.get(), detail::slice(key), detail::slice(value),
                                         expires_at_ms));
    }
    void erase(std::string_view key) {
        detail::require(handle_.get());
        detail::check(fqdb_store_txn_delete(handle_.get(), detail::slice(key)));
    }
    void expire(std::string_view key, uint64_t expires_at_ms) {
        detail::require(handle_.get());
        detail::check(fqdb_store_txn_expire(handle_.get(), detail::slice(key), expires_at_ms));
    }
    void patch(std::string_view key, uint64_t offset, uint64_t remove, std::string_view insert) {
        detail::require(handle_.get());
        detail::check(fqdb_store_txn_patch(handle_.get(), detail::slice(key), offset, remove,
                                           detail::slice(insert)));
    }
    uint64_t savepoint() {
        detail::require(handle_.get());
        uint64_t id = 0;
        detail::check(fqdb_store_txn_savepoint(handle_.get(), &id));
        return id;
    }
    void rollback_to(uint64_t id) {
        detail::require(handle_.get());
        detail::check(fqdb_store_txn_rollback_to(handle_.get(), id));
    }
    void release(uint64_t id) {
        detail::require(handle_.get());
        detail::check(fqdb_store_txn_release(handle_.get(), id));
    }
    void cancel() {
        detail::require(handle_.get());
        fqdb_store_txn_cancel(handle_.get());
    }
    StoreCommandResult query_index(std::string_view name, const StoreIndexQuery &query = {},
                                   std::string_view cursor = {}, uint64_t limit = 256) {
        auto bytes = query.encode();
        return execute({FQDB_INDEX_QUERY, name, cursor, bytes, 0, 0, limit});
    }
    std::string project(std::string_view key, const std::vector<uint32_t> &fields) {
        StoreIndexQuery query;
        query.fields = fields;
        auto bytes = query.encode();
        return *execute({FQDB_RECORD_PROJECT, {}, key, bytes}).value;
    }
    StoreCommandResult execute(const StoreCommand &command) {
        detail::require(handle_.get());
        auto request = command.encode();
        fqdb_buffer bytes{};
        detail::check(fqdb_store_txn_execute(handle_.get(), detail::slice(request), &bytes));
        struct Free {
            fqdb_buffer *value;
            ~Free() {
                fqdb_buffer_free(value);
            }
        } free{&bytes};
        return StoreCommandResult(detail::view({bytes.data, bytes.size}));
    }
    CommitResult commit(bool force_reliable = false) {
        detail::require(handle_.get());
        uint64_t version = 0;
        auto status = fqdb_store_txn_commit(handle_.get(), &version, force_reliable);
        return {static_cast<Status>(status), version};
    }
    StoreCommit submit(bool force_reliable = false) {
        detail::require(handle_.get());
        fqdb_store_commit *out = nullptr;
        detail::check(fqdb_store_txn_submit(handle_.get(), &out, force_reliable));
        return StoreCommit(out);
    }
    StoreCursor scan(std::string_view begin = {}, std::string_view end = {}, bool reverse = false) {
        detail::require(handle_.get());
        fqdb_store_cursor *cursor = nullptr;
        detail::check(fqdb_store_txn_cursor_open(handle_.get(), detail::slice(begin),
                                                 detail::slice(end), reverse ? 1u : 0u, &cursor));
        return StoreCursor(cursor);
    }
};
class Store {
    std::unique_ptr<fqdb_store, detail::Deleter<fqdb_store, fqdb_store_close>> handle_;

    explicit Store(fqdb_store *handle) : handle_(handle) {}

  public:
    StoreWriteOptions write_options() const {
        fqdb_store_write_options out{};
        detail::check(fqdb_store_write_options_get(handle_.get(), &out));
        return {static_cast<StoreWriteMode>(out.mode), out.period_ms, out.threshold_bytes};
    }
    void configure_write(const StoreWriteOptions &options) {
        auto native = options.native();
        detail::check(fqdb_store_write_configure(handle_.get(), &native));
    }
    void reset_write_options() {
        detail::check(fqdb_store_write_configure(handle_.get(), nullptr));
    }
    StoreCommit submit_put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0,
                           bool force_reliable = false) {
        fqdb_store_commit *out = nullptr;
        detail::check(fqdb_store_put_submit(handle_.get(), detail::slice(key), detail::slice(value),
                                            expires_at_ms, &out, force_reliable));
        return StoreCommit(out);
    }
    StoreWriteState write_state() const {
        StoreWriteState out{};
        detail::check(fqdb_store_write_state_get(handle_.get(), &out));
        return out;
    }
    StoreAtomicInfo put(std::string_view key, std::string_view value, uint64_t expires_at_ms = 0,
                        bool force_reliable = false) {
        StoreAtomicInfo out{};
        detail::check(fqdb_store_put(handle_.get(), detail::slice(key), detail::slice(value),
                                     expires_at_ms, &out, force_reliable));
        return out;
    }
    StoreAtomicInfo erase(std::string_view key, bool force_reliable = false) {
        StoreAtomicInfo out{};
        detail::check(fqdb_store_delete(handle_.get(), detail::slice(key), &out, force_reliable));
        return out;
    }
    StoreAtomicInfo patch(std::string_view key, uint64_t offset, uint64_t remove,
                          std::string_view insert, bool force_reliable = false) {
        StoreAtomicInfo out{};
        detail::check(fqdb_store_patch(handle_.get(), detail::slice(key), offset, remove,
                                       detail::slice(insert), &out, force_reliable));
        return out;
    }
    static std::string path_at(const std::string &root, const std::string &name) {
        detail::check(root.find('\0') == std::string::npos && name.find('\0') == std::string::npos
                          ? FQDB_OK
                          : FQDB_INVALID_ARGUMENT);
        fqdb_buffer out{};
        detail::check(fqdb_store_path_at(root.c_str(), name.c_str(), &out));
        struct Free {
            fqdb_buffer *out;
            ~Free() {
                fqdb_buffer_free(out);
            }
        } free{&out};
        return std::string(detail::view({out.data, out.size}));
    }
    static Store open_at(const std::string &root, const std::string &name,
                         const StoreOptions &options = {}) {
        detail::check(root.find('\0') == std::string::npos && name.find('\0') == std::string::npos
                          ? FQDB_OK
                          : FQDB_INVALID_ARGUMENT);
        detail::check(fqdb_abi_version() == FQDB_ABI_VERSION ? FQDB_OK : FQDB_UNSUPPORTED);
        auto native = options.native();
        fqdb_store *out = nullptr;
        detail::check(fqdb_store_open_at(root.c_str(), name.c_str(), &native, &out));
        return Store(out);
    }
    StoreValues get_many(const std::vector<std::string_view> &keys) const {
        detail::require(handle_.get());
        auto input = detail::store_keys(keys);
        fqdb_store_results *out = nullptr;
        detail::check(fqdb_store_get_many(handle_.get(), input.data(),
                                          static_cast<uint32_t>(input.size()), &out));
        return StoreValues(out);
    }
    StoreReader prepare_reader(const std::vector<std::string_view> &keys) const {
        detail::require(handle_.get());
        auto input = detail::store_keys(keys);
        fqdb_store_reader *out = nullptr;
        detail::check(fqdb_store_reader_create(handle_.get(), input.data(),
                                               static_cast<uint32_t>(input.size()), &out));
        return StoreReader(out);
    }
    StoreReader prepare_reader(std::string_view key) const {
        return prepare_reader(std::vector<std::string_view>{key});
    }
    StoreReader prepare_project_reader(const std::vector<std::string_view> &keys,
                                       const std::vector<uint32_t> &fields) const {
        detail::require(handle_.get());
        detail::check(fields.size() <= 256 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
        auto input = detail::store_keys(keys);
        fqdb_store_reader *out = nullptr;
        detail::check(fqdb_store_project_reader_create(
            handle_.get(), input.data(), static_cast<uint32_t>(input.size()), fields.data(),
            static_cast<uint32_t>(fields.size()), &out));
        return StoreReader(out);
    }
    StorePreparedKey prepare_key(std::string_view key) const {
        detail::require(handle_.get());
        fqdb_store_key *out = nullptr;
        detail::check(fqdb_store_key_prepare(handle_.get(), detail::slice(key), &out));
        return StorePreparedKey(out);
    }
    StoreBatch prepare_batch(const std::vector<StorePut> &items) const {
        detail::require(handle_.get());
        auto input = detail::store_items(items);
        fqdb_store_batch *out = nullptr;
        detail::check(fqdb_store_batch_create(handle_.get(), input.data(),
                                              static_cast<uint32_t>(input.size()), &out));
        return StoreBatch(out);
    }
    StoreAtomicInfo compare_exchange(std::string_view key, uint64_t expected,
                                     std::string_view value, uint64_t expires_at_ms = 0,
                                     uint64_t timeout_ms = 0, bool force_reliable = false) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(fqdb_store_compare_exchange(handle_.get(), detail::slice(key), expected,
                                                  detail::slice(value), expires_at_ms, timeout_ms,
                                                  &out, force_reliable));
        return out;
    }
    StoreAtomicInfo put_if_absent(std::string_view key, std::string_view value,
                                  uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0,
                                  bool force_reliable = false) {
        return compare_exchange(key, 0, value, expires_at_ms, timeout_ms, force_reliable);
    }
    StoreAtomicInfo delete_if_version(std::string_view key, uint64_t expected,
                                      uint64_t timeout_ms = 0, bool force_reliable = false) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(fqdb_store_delete_if_version(handle_.get(), detail::slice(key), expected,
                                                   timeout_ms, &out, force_reliable));
        return out;
    }
    StoreAtomicInfo put_if_changed(std::string_view key, std::string_view value,
                                   uint64_t expires_at_ms = 0, uint64_t timeout_ms = 0,
                                   bool force_reliable = false) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(fqdb_store_put_if_changed(handle_.get(), detail::slice(key),
                                                detail::slice(value), expires_at_ms, timeout_ms,
                                                &out, force_reliable));
        return out;
    }
    StoreAtomicInfo add_i64(std::string_view key, int64_t delta, int64_t minimum = INT64_MIN,
                            int64_t maximum = INT64_MAX, uint64_t timeout_ms = 0,
                            bool force_reliable = false) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(fqdb_store_add_i64(handle_.get(), detail::slice(key), delta, minimum, maximum,
                                         timeout_ms, &out, force_reliable));
        return out;
    }
    StoreAtomicInfo add_i64_fields(const fqdb_store_i64_field_update *items, uint32_t count,
                                   uint64_t timeout_ms = 0, bool force_reliable = false) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(fqdb_store_add_i64_fields(handle_.get(), items, count, timeout_ms, &out,
                                                force_reliable));
        return out;
    }
    StoreAtomicInfo add_i64_fields(const std::vector<StoreI64FieldUpdate> &items,
                                   uint64_t timeout_ms = 0, bool force_reliable = false) {
        detail::check(!items.empty() && items.size() <= 256 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
        std::vector<fqdb_store_i64_field_update> native;
        native.reserve(items.size());
        for (const auto &item : items)
            native.push_back(
                {detail::slice(item.key), item.offset, item.delta, item.minimum, item.maximum});
        return add_i64_fields(native.data(), static_cast<uint32_t>(native.size()), timeout_ms,
                              force_reliable);
    }
    uint64_t put_many(const std::vector<StorePut> &items, bool force_reliable = false) {
        auto tx = transaction();
        tx.put_many(items);
        auto result = tx.commit(force_reliable);
        detail::check(static_cast<fqdb_status>(result.status));
        return result.version;
    }
    uint64_t delete_many(const std::vector<std::string_view> &keys, bool force_reliable = false) {
        auto tx = transaction();
        tx.delete_many(keys);
        auto result = tx.commit(force_reliable);
        detail::check(static_cast<fqdb_status>(result.status));
        return result.version;
    }
    uint64_t put_batch(const StoreBatch &batch, bool force_reliable = false) {
        auto tx = transaction();
        tx.put_batch(batch);
        auto result = tx.commit(force_reliable);
        detail::check(static_cast<fqdb_status>(result.status));
        return result.version;
    }
    explicit Store(std::string_view directory, const StoreOptions &options = {}) {
        detail::check(fqdb_abi_version() == FQDB_ABI_VERSION ? FQDB_OK : FQDB_UNSUPPORTED);
        auto path = detail::path(directory);
        auto native = options.native();
        fqdb_store *handle = nullptr;
        detail::check(fqdb_store_open(path.c_str(), &native, &handle));
        handle_.reset(handle);
    }
    Store(Store &&) noexcept = default;
    Store &operator=(Store &&) noexcept = default;
    std::optional<StoreValue> hash_get(std::string_view scope, std::string_view key) const {
        detail::require(handle_.get());
        fqdb_store_value *value = nullptr;
        auto status = fqdb_store_hash_get_view(handle_.get(), detail::slice(scope),
                                               detail::slice(key), &value);
        if (status == FQDB_NOT_FOUND)
            return {};
        detail::check(status);
        return StoreValue(value);
    }
    StoreCommandResult query_index(std::string_view name, const StoreIndexQuery &query = {},
                                   std::string_view cursor = {}, uint64_t limit = 256) {
        auto bytes = query.encode();
        return execute({FQDB_INDEX_QUERY, name, cursor, bytes, 0, 0, limit});
    }
    std::string project(std::string_view key, const std::vector<uint32_t> &fields) {
        detail::require(handle_.get());
        detail::check(fields.size() <= 256 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
        fqdb_buffer bytes{};
        detail::check(fqdb_store_project(handle_.get(), detail::slice(key), fields.data(),
                                         static_cast<uint32_t>(fields.size()), &bytes));
        struct Free {
            fqdb_buffer &bytes;
            ~Free() {
                fqdb_buffer_free(&bytes);
            }
        } free{bytes};
        return std::string(detail::view({bytes.data, bytes.size}));
    }
    // Read-only query on a fresh snapshot (FQDB_SELECT / FQDB_SELECT_EXPLAIN only);
    // creates no committable transaction and records no per-row dependencies.
    StoreCommandResult select_command(const StoreCommand &command) const {
        detail::require(handle_.get());
        auto request = command.encode();
        fqdb_buffer bytes{};
        detail::check(fqdb_store_select(handle_.get(), detail::slice(request), &bytes));
        struct Free {
            fqdb_buffer &bytes;
            ~Free() {
                fqdb_buffer_free(&bytes);
            }
        } free{bytes};
        return StoreCommandResult(detail::view({bytes.data, bytes.size}));
    }
    StoreCommandResult execute(const StoreCommand &command, bool force_reliable = false) {
        auto tx = transaction();
        auto result = tx.execute(command);
        auto committed = tx.commit(force_reliable);
        detail::check(static_cast<fqdb_status>(committed.status));
        result.version = committed.version;
        return result;
    }
    void configure(const StoreRuntimeOptions &options = {}) {
        detail::require(handle_.get());
        auto native = options.native();
        detail::check(fqdb_store_configure(handle_.get(), &native));
    }
    StoreRuntimeInfo runtime_info() const {
        detail::require(handle_.get());
        StoreRuntimeInfo info{};
        detail::check(fqdb_store_runtime_info_get(handle_.get(), &info));
        return info;
    }
    StoreMaintenanceInfo define_index(std::string_view name, std::string_view prefix,
                                      uint32_t field, uint32_t type, uint32_t flags = 0,
                                      const StoreMaintenanceOptions &options = {}) {
        detail::require(handle_.get());
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_index_define(handle_.get(), detail::slice(name),
                                              detail::slice(prefix), field, type, flags, &native,
                                              &info));
        return info;
    }
    StoreAtomicInfo hold_history(std::string_view name,
                                 uint64_t from_version = FQDB_HISTORY_CURRENT,
                                 uint64_t timeout_ms = 0) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(fqdb_store_history_hold(handle_.get(), detail::slice(name), from_version,
                                              timeout_ms, &out));
        return out;
    }
    StoreAtomicInfo release_history(std::string_view name, uint64_t timeout_ms = 0) {
        detail::require(handle_.get());
        StoreAtomicInfo out{};
        detail::check(
            fqdb_store_history_release(handle_.get(), detail::slice(name), timeout_ms, &out));
        return out;
    }
    StoreHistoryInfo history_info() const {
        detail::require(handle_.get());
        StoreHistoryInfo out{};
        detail::check(fqdb_store_history_info_get(handle_.get(), &out));
        return out;
    }
    StoreWatch watch(uint64_t from_version = FQDB_HISTORY_CURRENT, std::string_view prefix = {},
                     const StoreWatchOptions &options = {}) {
        detail::require(handle_.get());
        auto settings = options.native();
        fqdb_store_watch *out = nullptr;
        detail::check(fqdb_store_watch_open(handle_.get(), from_version, detail::slice(prefix),
                                            &settings, &out));
        return StoreWatch(out);
    }
    StoreMaintenanceInfo define_compound_index(std::string_view name, std::string_view prefix,
                                               const std::vector<StoreIndexField> &fields,
                                               uint32_t flags = 0,
                                               const StoreMaintenanceOptions &options = {}) {
        detail::require(handle_.get());
        detail::check(!fields.empty() && fields.size() <= 8 ? FQDB_OK : FQDB_INVALID_ARGUMENT);
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_index_define_compound(
            handle_.get(), detail::slice(name), detail::slice(prefix), fields.data(),
            static_cast<uint32_t>(fields.size()), flags, &native, &info));
        return info;
    }
    StoreMaintenanceInfo drop_index(std::string_view name,
                                    const StoreMaintenanceOptions &options = {}) {
        detail::require(handle_.get());
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_index_drop(handle_.get(), detail::slice(name), &native, &info));
        return info;
    }
    StoreMaintenanceInfo backup_incremental(std::string_view destination, uint64_t from,
                                            const StoreMaintenanceOptions &options = {}) {
        detail::require(handle_.get());
        auto path = detail::path(destination);
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(
            fqdb_store_backup_incremental(handle_.get(), path.c_str(), from, &native, &info));
        return info;
    }
    static StoreMaintenanceInfo
    recover_chain(std::string_view base, const std::vector<std::string> &archives,
                  std::string_view destination, uint64_t target_version = 0,
                  uint64_t target_unix_ms = 0, const StoreMaintenanceOptions &options = {}) {
        auto from = detail::path(base), to = detail::path(destination);
        auto native = options.native();
        std::vector<const char *> paths;
        paths.reserve(archives.size());
        for (const auto &path : archives) {
            detail::path(path);
            paths.push_back(path.c_str());
        }
        if (paths.size() > 1024)
            throw Error(FQDB_INVALID_ARGUMENT);
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_recover_chain(from.c_str(), paths.data(),
                                               static_cast<uint32_t>(paths.size()), to.c_str(),
                                               target_version, target_unix_ms, &native, &info));
        return info;
    }

    StoreInfo info() const {
        detail::require(handle_.get());
        StoreInfo info{};
        detail::check(fqdb_store_get_info(handle_.get(), &info));
        return info;
    }
    void sync() {
        detail::require(handle_.get());
        detail::check(fqdb_store_sync(handle_.get()));
    }
    void checkpoint() {
        detail::require(handle_.get());
        detail::check(fqdb_store_checkpoint(handle_.get()));
    }
    StoreMaintenanceInfo compact(const StoreMaintenanceOptions &options = {}) {
        detail::require(handle_.get());
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_compact(handle_.get(), &native, &info));
        return info;
    }
    StoreMaintenanceInfo backup(std::string_view destination,
                                const StoreMaintenanceOptions &options = {}) {
        detail::require(handle_.get());
        auto path = detail::path(destination);
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_backup(handle_.get(), path.c_str(), &native, &info));
        return info;
    }
    StoreMaintenanceInfo verify(const StoreMaintenanceOptions &options = {}) const {
        detail::require(handle_.get());
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_verify(handle_.get(), &native, &info));
        return info;
    }
    static StoreMaintenanceInfo restore(std::string_view source, std::string_view destination,
                                        const StoreMaintenanceOptions &options = {}) {
        auto from = detail::path(source), to = detail::path(destination);
        auto native = options.native();
        StoreMaintenanceInfo info{};
        detail::check(fqdb_store_restore(from.c_str(), to.c_str(), &native, &info));
        return info;
    }
    StoreExpiryInfo purge_expired(const StoreExpiryOptions &options = {}) {
        detail::require(handle_.get());
        auto native = options.native();
        StoreExpiryInfo info{};
        detail::check(fqdb_store_purge_expired(handle_.get(), &native, &info));
        return info;
    }
    void cancel_maintenance() {
        detail::require(handle_.get());
        fqdb_store_cancel_maintenance(handle_.get());
    }
    std::optional<StoreValue> get(std::string_view key) const {
        detail::require(handle_.get());
        fqdb_store_value *value = nullptr;
        auto status = fqdb_store_get_view(handle_.get(), detail::slice(key), &value);
        if (status == FQDB_NOT_FOUND)
            return {};
        detail::check(status);
        return StoreValue(value);
    }
    StoreSnapshot snapshot() const {
        detail::require(handle_.get());
        fqdb_store_snapshot *snapshot = nullptr;
        detail::check(fqdb_store_snapshot_create(handle_.get(), &snapshot));
        return StoreSnapshot(snapshot);
    }
    StoreTransaction transaction(uint64_t timeout_ms = 0) {
        detail::require(handle_.get());
        fqdb_store_txn *txn = nullptr;
        detail::check(fqdb_store_txn_begin(handle_.get(), timeout_ms, &txn));
        return StoreTransaction(txn);
    }
};
} // namespace fengqudb
#endif
