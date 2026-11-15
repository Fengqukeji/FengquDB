#pragma once
#include "fengqudb_store.h"
#include "fengqudb_log.h"
#include "recovery.hpp"
#include "copy.hpp"
#include "write_index.hpp"
#include "write_policy.hpp"
#include "output_jobs.hpp"
#include <condition_variable>
#include "../commit_signal.hpp"
#include "../adaptive_wait.hpp"
#include <map>
#include <optional>
#include <thread>
#include <type_traits>

namespace fengqudb::paged {
inline std::atomic<uint64_t> next_store_id{1};
#ifdef FQDB_TESTING
inline std::atomic<int> store_crash_point{0};
inline std::atomic<int> maintenance_hold_point{0};
inline std::atomic<bool> maintenance_entered{false};
inline void store_crash_at(int point) {
    if (store_crash_point.load() == point)
        std::_Exit(86);
}
inline void maintenance_at(int point) {
    if (maintenance_hold_point.load() == point) {
        maintenance_entered.store(true);
        while (maintenance_hold_point.load() == point)
            std::this_thread::yield();
    }
    store_crash_at(point);
}
#else
inline void store_crash_at(int) {}
inline void maintenance_at(int) {}
#endif
inline fqdb_store_options defaults() {
    return {sizeof(fqdb_store_options),
            1,
            FQDB_DURABILITY_SYNC,
            FQDB_STORE_CREATE,
            64 * 1024 * 1024,
            4 * 1024 * 1024,
            32 * 1024 * 1024,
            64 * 1024 * 1024,
            256 * 1024 * 1024,
            128 * 1024 * 1024,
            1024};
}
struct Transaction;
struct RulesCache;
struct QueryRuleCache;
namespace select {
struct Program;
struct PlanCache;
} // namespace select
struct Core;
void attach_history(Core &core);
void prepare_features(Transaction &transaction, const View &current);
void verify_features(const View &view, uint64_t time, const std::function<void()> &check);
struct CommitWaiter {
    Transaction *transaction = nullptr;
    SingleWrite *single = nullptr;
    std::shared_ptr<CommitSignal> signal;
    fqdb_log_event *trace = nullptr;
    void *context = nullptr;
    void (*check)(void *) = nullptr;
    void (*prepare)(CommitWaiter &, std::shared_ptr<const View>) = nullptr;
    void (*cleanup)(CommitWaiter &) noexcept = nullptr;
    void (*accept)(CommitWaiter &) noexcept = nullptr;
    CommitWaiter *next = nullptr;
    fqdb_status status = FQDB_OK;
    uint64_t version = 0, write_bytes = 0;
    uint64_t queued_bytes = 0, queue_sequence = 0;
    // Buffered delegation metadata is protected by Core::group_mutex.
    std::optional<std::condition_variable> completion;
    bool queued = false, promoted = false;
    bool done = false, appended = false;
    bool force_reliable = false, reliable = true, eager_wait = false;
    // Only writer-created commands with no escaping private writes may opt in.
    // A unique data pointer alone cannot prove that its tree node is unshared.
    bool consume_cache_values = false;
    void ready() const {
        if (check)
            check(context);
    }
    void release_private() noexcept {
        if (auto callback = std::exchange(cleanup, nullptr))
            callback(*this);
    }
    void finish() noexcept;
};
// A logical writer lease built from standard mutex/condition-variable operations.
// Avoids platform-specific timed-mutex clock APIs while supporting bounded waits
// for cancellation. Uncontended acquisition does not enter the wait queue.
class WriterGate {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> busy_{false};
    std::atomic<uint32_t> parked_{0};

    // 单次原子抢占；只有观察到空闲时才发起交换，避免无谓的缓存行独占。
    bool acquire() noexcept {
        return !busy_.load(std::memory_order_relaxed) &&
               !busy_.exchange(true, std::memory_order_acquire);
    }
    // 有界自旋：持锁区通常只有几微秒，短暂等待比停车再唤醒便宜。
    // 单 CPU 时持有者无法同时运行，自旋只会空耗，因此直接放弃。
    bool spin() noexcept {
        static const bool parallel = AdaptiveCommitWait::parallel_execution();
        if (!parallel)
            return false;
        for (unsigned i = 0; i < 1024; ++i) {
            if (acquire())
                return true;
            commit_pause();
        }
        return false;
    }

  public:
    void lock() {
        if (acquire() || spin())
            return;
        parked_.fetch_add(1, std::memory_order_seq_cst);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return acquire(); });
        }
        parked_.fetch_sub(1, std::memory_order_relaxed);
    }
    bool try_lock() {
        return acquire();
    }
    template <class R, class P> bool try_lock_for(std::chrono::duration<R, P> duration) {
        if (acquire() || spin())
            return true;
        parked_.fetch_add(1, std::memory_order_seq_cst);
        bool got;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            got = changed_.wait_for(lock, duration, [&] { return acquire(); });
        }
        parked_.fetch_sub(1, std::memory_order_relaxed);
        return got;
    }
    void unlock() {
        busy_.store(false, std::memory_order_seq_cst);
        // 先登记 parked_ 再在互斥量内复查 busy_，所以这里读到 0 时不会丢失唤醒；
        // 读到非 0 时经互斥量串行化，保证等待者要么已看到空闲要么已进入等待。
        if (parked_.load(std::memory_order_seq_cst)) {
            { std::lock_guard<std::mutex> lock(mutex_); }
            changed_.notify_one();
        }
    }
};
struct WatchLease {
    std::atomic<uint64_t> version{0};
    std::atomic<bool> cancelled{false};
};
struct WatchHub {
    Charge charge;
    std::mutex mutex;
    std::condition_variable changed;
    // Registry access is protected by Core::writer; progress is atomic.
    std::vector<std::weak_ptr<WatchLease>> readers;
    explicit WatchHub(std::shared_ptr<Budget> budget)
        : charge(std::move(budget), {}, sizeof(WatchHub) + 64 * sizeof(readers[0]) + 128) {
        readers.reserve(64);
    }
    void retain(uint64_t &minimum, uint64_t &active) {
        auto out = readers.begin();
        for (auto it = readers.begin(); it != readers.end(); ++it)
            if (auto lease = it->lock()) {
                if (lease->cancelled.load(std::memory_order_acquire))
                    continue;
                minimum = std::min(minimum, lease->version.load(std::memory_order_acquire));
                ++active;
                *out++ = *it;
            }
        readers.erase(out, readers.end());
    }
};
struct Core : std::enable_shared_from_this<Core> {
    storage::File lease;
    fqdb_store_options options;
    std::shared_ptr<const RulesCache> rules_cache;
    // Query threads publish immutable, budgeted entries through C++17 atomics.
    std::shared_ptr<const QueryRuleCache> query_rule_cache;
    std::shared_ptr<const select::Program> select_program_cache;
    std::shared_ptr<const select::PlanCache> select_plan_cache;
    std::string directory;
    std::shared_ptr<const View> state;
    // Odd while publishing. Saturation disables memoization rather than ABA.
    std::atomic<uint64_t> read_stamp{2};
    std::shared_ptr<Budget> transaction_budget, pin_budget, snapshot_budget;
    WriterGate writer;
    std::weak_ptr<WatchHub> watches; // Accessed only under writer (or before public open).
    std::mutex group_mutex;
    std::condition_variable group_changed;
    CommitWaiter *group_head = nullptr, *group_tail = nullptr;
    size_t group_count = 0;
    bool group_active = false;
    std::atomic<uint8_t> buffered_active{0};
    std::atomic<uint32_t> group_max{128}, group_delay_us{0};
    std::atomic<uint64_t> group_flushes{0}, group_transactions{0};
    // Mode transitions stop admissions, drain accepted jobs, then atomically
    // publish the saved settings. The worker never takes admission_mutex.
    std::mutex admission_mutex;
    std::condition_variable admission_changed;
    bool reconfiguring = false;
    size_t strict_active = 0;
    std::mutex periodic_mutex;
    std::condition_variable periodic_changed, periodic_drained;
    std::thread periodic_thread;
    CommitWaiter *periodic_head = nullptr, *periodic_tail = nullptr;
    uint64_t periodic_bytes = 0, periodic_pending_bytes = 0;
    uint64_t periodic_sequence = 0, periodic_completed = 0;
    size_t periodic_count = 0;
    std::atomic<bool> periodic_ready{false};
#ifdef FQDB_TESTING
    std::atomic<uint64_t> periodic_iterations{0}, periodic_idle_waits{0};
#endif
    bool periodic_stop = false;
    fqdb_store_write_options write_policy = write_defaults();
    bool write_policy_saved = false;
    std::chrono::steady_clock::time_point write_epoch = std::chrono::steady_clock::now();
    std::mutex maintenance;
    std::atomic<bool> maintenance_active{false}, maintenance_cancelled{false};
    // One atomic state keeps cancellation from being lost at session startup:
    // 0 = idle, 1 = pending/running, 2 = cancellation requested.
    std::atomic<uint8_t> reclaim_state{0};
    std::atomic<bool> poisoned{false};
    std::atomic<uint64_t> dirty_bytes{0};
    std::atomic<fqdb_status> write_failure{FQDB_OK};
    // Maintenance-lease protected, bounded to one key; never pins a generation.
    Bytes expiry_resume;
    std::weak_ptr<Generation> expiry_generation;
    bool expiry_resume_valid = false;
    const uint64_t id = (UINT64_C(1) << 63) | next_store_id.fetch_add(1, std::memory_order_relaxed);
    uint64_t recovered_transactions = 0, repaired_bytes = 0;
    std::mutex configuration_mutex, background_mutex;
    std::condition_variable background_changed;
    std::thread background_thread;
    std::atomic<bool> background_stop{false}, background_running{false};
    std::atomic<bool> automatic_maintenance{true};
    std::atomic<uint64_t> auto_next_limit{33554432};
#ifdef FQDB_TESTING
    std::atomic<bool> fail_automatic_start{false};
#endif
    std::atomic<uint64_t> background_status{0}, background_expiry{0}, background_compactions{0};
    fqdb_store_runtime_options runtime{};
    ~Core();
    void configure_write(const fqdb_store_write_options &settings);
    fqdb_store_write_options get_write_policy();
    void enqueue_periodic(CommitWaiter &request);
    void start_periodic_locked();
    void periodic_loop() noexcept;
    void stop_periodic();
    void flush_periodic();
    void cancel_commit(CommitWaiter &request);
    bool wait_commit(CommitWaiter &request, uint64_t timeout, bool poll = false);
    void admit(CommitWaiter &request, bool nonblocking = false);
    void strict_finished() noexcept;
    uint64_t submit_strict(CommitWaiter &request);
    template <class Take>
    size_t process_group(CommitWaiter **batch, size_t maximum, Take &&take, bool reliable = true);
    void stop_worker();
    void stop_worker_unlocked();
    void configure_runtime(const fqdb_store_runtime_options &settings);
    void background_loop(bool automatic = false);
    void maybe_auto_maintenance(uint64_t limit, uint64_t live) noexcept;
    void cancel_maintenance() noexcept {
        if (maintenance_active.load(std::memory_order_acquire))
            maintenance_cancelled.store(true, std::memory_order_relaxed);
        uint8_t pending = 1;
        if (reclaim_state.compare_exchange_strong(pending, 2, std::memory_order_acq_rel))
            background_changed.notify_all();
    }
    Core(std::string path, fqdb_store_options opts)
        : options(opts), directory(directory_path(path)),
          transaction_budget(std::make_shared<Budget>(options.all_transaction_bytes)),
          pin_budget(std::make_shared<Budget>(options.pinned_bytes)),
          snapshot_budget(std::make_shared<Budget>(options.max_snapshots)) {}
    std::shared_ptr<const View> current() const {
        return std::atomic_load_explicit(&state, std::memory_order_acquire);
    }
    void publish(std::shared_ptr<const View> next) {
        const auto published_limit = next->limit, published_live = next->live_bytes;
        if (next->durable == next->version)
            dirty_bytes.store(0, std::memory_order_relaxed);
        if (next->reader_epoch != next->generation->reader_epoch)
            next->reader_epoch = next->generation->reader_epoch;
        const auto stamp = read_stamp.load(std::memory_order_relaxed);
        read_stamp.store(stamp | 1, std::memory_order_release);
        maintenance_at(87);
        std::atomic_store_explicit(&state, std::move(next), std::memory_order_release);
        maintenance_at(88);
        read_stamp.store(stamp < UINT64_MAX - 2 ? stamp + 2 : UINT64_MAX,
                         std::memory_order_release);
        if (auto hub = watches.lock()) {
            std::lock_guard<std::mutex> lock(hub->mutex);
            hub->changed.notify_all();
        }
        maybe_auto_maintenance(published_limit, published_live);
    }
    void open() {
        std::error_code error;
        const auto native = std::filesystem::u8path(directory);
        if (options.flags & FQDB_STORE_SEGMENTED)
            check_path(native, 32);
        if (!std::filesystem::exists(native, error)) {
            require(!error, FQDB_IO_ERROR);
            require(options.flags & FQDB_STORE_CREATE, FQDB_NOT_FOUND);
            require(std::filesystem::create_directory(native, error) && !error, FQDB_IO_ERROR);
        }
        require(std::filesystem::is_directory(native, error) && !error, FQDB_IO_ERROR);
        if (!(options.flags & FQDB_STORE_CREATE)) {
            auto exists = std::filesystem::exists(native / "CURRENT", error);
            require(exists, error ? FQDB_IO_ERROR : FQDB_NOT_FOUND);
            require(!error, FQDB_IO_ERROR);
        }
        auto status = lease.open(join(directory, "LOCK").c_str());
        require(status == FQDB_OK, status);
        reclaim_fixed_temporaries(directory);
        write_policy = load_write_policy(directory, &write_policy_saved);
        auto generation = std::make_shared<Generation>(options.cache_bytes);
        auto manifest = std::filesystem::u8path(join(directory, "CURRENT"));
        if (!std::filesystem::exists(manifest, error)) {
            require(!error, FQDB_IO_ERROR);
            require(options.flags & FQDB_STORE_CREATE, FQDB_NOT_FOUND);
            auto uuid = new_uuid();
            generation->create(join(directory, generation_name(uuid, 1)), uuid, 1,
                               options.flags & FQDB_STORE_SEGMENTED ? new_segment_geometry() : 0,
                               true);
            generation->remove_on_close.store(true, std::memory_order_relaxed);
            Checkpoint checkpoint;
            generation->seal(checkpoint);
            status = write_current(directory, {uuid, 1});
            if (status == FQDB_OK || status == FQDB_COMMIT_UNKNOWN)
                generation->remove_on_close.store(false, std::memory_order_relaxed);
            require(status == FQDB_OK, status);
            auto initial = std::make_shared<View>();
            initial->generation = generation;
            initial->checkpoint = checkpoint;
            initial->limit = generation->tail;
            state = std::move(initial);
        } else {
            require(!error, FQDB_IO_ERROR);
            auto manifest_value = read_current(directory);
            generation->open(join(directory, manifest_value.filename()), manifest_value.uuid,
                             manifest_value.generation);
            auto checkpoint = generation->checkpoint();
            require(!(checkpoint.features & 2), FQDB_UNSUPPORTED);
            auto recovered = recover(generation, directory, options.delta_bytes,
                                     options.writer_bytes, &checkpoint);
            recovered_transactions = recovered.replayed;
            repaired_bytes = recovered.repaired;
            state = std::move(recovered.view);
        }
        attach_history(*this);
        auto active = current();
        active->reader_epoch = active->generation->reader_epoch;
        reclaim_store_temporaries(directory, active->generation->uuid, active->generation->number,
                                  history_keep(*active));
        (void)reclaim_generations(directory, active->generation->uuid, active->generation->number,
                                  history_keep(*active));
        (void)recover_output(directory, active->generation->uuid);
    }
    void sync_business_locked() {
        if (!storage::io_quota || !dirty_bytes.load(std::memory_order_relaxed))
            return;
        auto quota = storage::io_quota;
        storage::io_quota = nullptr;
        try {
            sync_locked();
        } catch (...) {
            storage::io_quota = quota;
            throw;
        }
        storage::io_quota = quota;
    }
    void checkpoint_locked() {
        require(!poisoned, FQDB_RECOVERY_REQUIRED);
        sync_business_locked();
        auto old = current();
        if (!old->delta && old->version == old->durable)
            return;
        // Reject before folding appends any pages. An exhausted root sequence
        // must not leave a tail that cannot be checkpointed during reopen.
        require(old->checkpoint.sequence != UINT64_MAX, FQDB_LIMIT);
        auto next = std::make_shared<View>(*old);
        auto generation = old->generation;
        try {
            auto &checkpoint = next->checkpoint;
            if (old->delta)
                checkpoint.root = fold_delta(generation, checkpoint.root, old->delta, old->version,
                                             options.writer_bytes);
            checkpoint.version = old->version;
            checkpoint.entries = old->entries;
            checkpoint.user_keys = old->keys;
            checkpoint.live_bytes = old->live_bytes;
            checkpoint.time = std::max(checkpoint.time, unix_ms());
            generation->seal(checkpoint);
            next->delta.reset();
            next->limit = generation->tail;
            next->durable = next->version;
            publish(std::move(next));
            dirty_bytes.store(0, std::memory_order_relaxed);
        } catch (const Error &error) {
            if (generation->write_failed || error.status == FQDB_COMMIT_UNKNOWN)
                poisoned = true;
            throw;
        } catch (...) {
            if (generation->write_failed)
                poisoned = true;
            throw;
        }
    }
    void checkpoint_locked_if_needed() {
        auto view = current();
        if (view->delta && view->delta->bytes > options.delta_bytes / 2)
            checkpoint_locked();
    }
    void checkpoint() {
        flush_periodic();
        std::lock_guard<WriterGate> lock(writer);
        checkpoint_locked();
    }
    void sync() {
        flush_periodic();
        std::lock_guard<WriterGate> lock(writer);
        sync_locked();
    }
    void sync_locked() {
        require(!poisoned, FQDB_RECOVERY_REQUIRED);
        auto old = current();
        if (old->durable == old->version) {
            dirty_bytes.store(0, std::memory_order_relaxed);
            return;
        }
        auto next = std::make_shared<View>(*old);
        try {
            maintenance_at(40);
            old->generation->file->sync();
        } catch (...) {
            poisoned = true;
            write_failure.store(FQDB_COMMIT_UNKNOWN, std::memory_order_release);
            throw Error(FQDB_COMMIT_UNKNOWN);
        }
        next->durable = next->version;
        dirty_bytes.store(0, std::memory_order_relaxed);
        group_flushes.fetch_add(1, std::memory_order_relaxed);
        publish(std::move(next));
    }
    uint64_t commit(Transaction &transaction, fqdb_log_event *trace = nullptr,
                    bool force_reliable = false);
    template <class Cursor>
    std::shared_ptr<const View> append_writes(CommitWaiter &request,
                                              std::shared_ptr<const View> old, Cursor writes,
                                              uint64_t features, uint64_t commit_time);
    uint64_t submit(CommitWaiter &request);
    uint64_t commit_direct(CommitWaiter &request);
    uint64_t commit_locked(CommitWaiter &request);
    uint64_t commit_buffered(CommitWaiter &request);
    void drain_buffered(CommitWaiter &leader);
    void drain_group();
    std::shared_ptr<const View> append_locked(CommitWaiter &request,
                                              std::shared_ptr<const View> old);
    template <class Check>
    uint64_t commit_checked(Transaction &transaction, fqdb_log_event *trace, Check &&check,
                            bool force_reliable = false);
    fqdb_store_expiry_info purge_expired(const fqdb_store_expiry_options &options,
                                         fqdb_log_event *trace = nullptr);
    fqdb_store_maintenance_info compact(const fqdb_store_maintenance_options &options,
                                        fqdb_log_event *trace = nullptr);
    fqdb_store_maintenance_info backup(const std::string &destination,
                                       const fqdb_store_maintenance_options &options,
                                       fqdb_log_event *trace = nullptr);
    fqdb_store_maintenance_info verify(const fqdb_store_maintenance_options &options,
                                       fqdb_log_event *trace = nullptr);
};
struct Capture {
    std::shared_ptr<Core> core;
    std::shared_ptr<const View> view;
    Charge count, pins;
    uint64_t time;
    explicit Capture(std::shared_ptr<Core> owner) : Capture(owner, owner->current(), unix_ms()) {}
    Capture(std::shared_ptr<Core> owner, std::shared_ptr<const View> state, uint64_t at)
        : core(std::move(owner)), view(std::move(state)), count(core->snapshot_budget, {}, 1),
          pins(core->pin_budget, {},
               sizeof(Capture) + 128 + (view->delta ? view->delta->bytes : 0)),
          time(at) {}
};
inline Value try_read(const View &view, uint64_t time, std::string_view key) {
    auto ref = view.find(key);
    if (!ref || !ref->visible(time))
        return {};
    return view.generation->value(*ref, view.limit);
}
struct Transaction {
    struct Dependency {
        Ref metadata;
        Charge charge;
        Dependency(Ref v, std::shared_ptr<Budget> local, std::shared_ptr<Budget> global,
                   uint64_t bytes)
            : metadata(v), charge(std::move(local), std::move(global), bytes) {}
    };
    struct Range {
        Bytes begin, end;
        Charge charge;
    };
    struct RetainedInput {
        std::shared_ptr<const void> owner;
        std::shared_ptr<const RetainedInput> previous;
        Charge charge;
        unsigned count;
        RetainedInput(std::shared_ptr<const void> value,
                      std::shared_ptr<const RetainedInput> before, Transaction &tx, uint64_t bytes)
            : owner(std::move(value)), previous(std::move(before)),
              charge(tx.budget, tx.base->core->transaction_budget,
                     bytes + sizeof(RetainedInput) + 64),
              count(previous ? previous->count + 1 : 1) {}
    };
    struct Savepoint {
        uint64_t id;
        WriteRoot root;
        std::shared_ptr<const RetainedInput> retained;
        Charge charge;
    };
    std::shared_ptr<Capture> base;
    std::shared_ptr<Budget> budget;
    std::map<Bytes, Dependency, detail::ByteLess> reads;
    std::vector<Range> ranges;
    std::vector<Savepoint> savepoints;
    WriteRoot writes;
    std::shared_ptr<const RetainedInput> retained;
    std::atomic<bool> cancelled{false};
    std::chrono::steady_clock::time_point deadline{};
    uint64_t next_savepoint = 0;
    bool has_deadline = false, sealed = false, broken = false;
    bool advanced = false, skip_indexes = false;
    // false only for Store-level read-only queries: the snapshot is immutable and never
    // committed, so per-key dependencies and ranges are useless. Skipping them saves a map
    // insertion and budget charge per row. Staging writes is rejected in this mode.
    bool track_reads = true;
    Ref untracked_;
    uint64_t imported_time = 0, feature_bits = 0;
    Transaction(std::shared_ptr<Core> core, uint64_t timeout)
        : Transaction(std::make_shared<Capture>(std::move(core)), timeout) {}
    Transaction(std::shared_ptr<Capture> capture, uint64_t timeout,
                std::shared_ptr<Budget> shared_budget = {})
        : base(std::move(capture)),
          budget(shared_budget ? std::move(shared_budget)
                               : std::make_shared<Budget>(base->core->options.transaction_bytes)) {
        if (timeout) {
            require(timeout <= static_cast<uint64_t>(INT64_MAX / 1000000), FQDB_INVALID_ARGUMENT);
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::milliseconds(timeout);
            require(duration <= std::chrono::steady_clock::time_point::max() - now,
                    FQDB_INVALID_ARGUMENT);
            deadline = now + duration;
            has_deadline = true;
        }
    }
    void ready(bool allow_broken = false) const {
        require(!sealed && (allow_broken || !broken), FQDB_INVALID_ARGUMENT);
        require(!cancelled.load(std::memory_order_relaxed), FQDB_CANCELLED);
        require(!has_deadline || std::chrono::steady_clock::now() < deadline, FQDB_TIMEOUT);
    }
    // Keep metadata beside each snapshot dependency. Subsequent reads and
    // edits reuse it; commit still validates against the latest writer view.
    const Ref &depend(const Bytes &key, Table::ReadPath *path = nullptr) {
        if (!track_reads) {
            // Read-only snapshot query: look up metadata without retaining a dependency.
            // The returned reference stays valid only until the next call.
            untracked_ =
                (path ? base->view->find(key, *path) : base->view->find(key)).value_or(Ref{});
            return untracked_;
        }
        auto position = reads.lower_bound(key);
        if (position == reads.end() || compare(position->first, key) != 0) {
            auto ref =
                (path ? base->view->find(key, *path) : base->view->find(key)).value_or(Ref{});
            position = reads.try_emplace(position, key, ref, budget, base->core->transaction_budget,
                                         key.size() + sizeof(Dependency) + 128);
        }
        return position->second.metadata;
    }
    std::shared_ptr<StagedData> data(const Data &value) {
        return staged_data(value, budget, base->core->transaction_budget);
    }
    std::shared_ptr<StagedData> data(std::string_view value) {
        return staged_data(value, budget, base->core->transaction_budget);
    }
    Value try_get(const Bytes &key, Ref *metadata = nullptr, Table::ReadPath *path = nullptr) {
        ready();
        // stage() already recorded the base dependency of every private write.
        if (auto node = write_find(writes, key)) {
            if (metadata) {
                *metadata = {};
                metadata->expires = node->value.expires;
            }
            if (node->value.kind == OpKind::erase ||
                (node->value.expires && node->value.expires <= base->time))
                return {};
            if (node->value.value)
                return node->value.value;
            return base->view->generation->value(node->value.original, base->view->limit);
        }
        const auto &ref = depend(key, path);
        if (metadata)
            *metadata = ref;
        return ref.visible(base->time) ? base->view->generation->value(ref, base->view->limit)
                                       : Value{};
    }
    Value get(const Bytes &key) {
        auto value = try_get(key);
        require(bool(value), FQDB_NOT_FOUND);
        return value;
    }
    void stage(const Bytes &key, Write value) {
        ready();
        require(track_reads, FQDB_INVALID_ARGUMENT);
        value.original = depend(key);
        try {
            WriteBuilder edit(std::move(writes), budget, base->core->transaction_budget);
            edit.set(key, std::move(value));
            writes = edit.finish();
        } catch (...) {
            broken = true;
            throw;
        }
    }
    void put(const Bytes &key, std::string_view value, uint64_t expires) {
        ready();
        Write write;
        write.kind = OpKind::put;
        write.expires = expires;
        try {
            write.value = data(value);
            stage(key, std::move(write));
        } catch (...) {
            broken = true;
            throw;
        }
    }
    void erase(const Bytes &key) {
        stage(key, {});
    }
    void expire(const Bytes &key, uint64_t expires) {
        (void)get(key);
        Write value;
        if (auto node = write_find(writes, key))
            value = node->value;
        else
            value.kind = OpKind::expiry;
        value.expires = expires;
        stage(key, std::move(value));
    }
    void patch(const Bytes &key, uint64_t offset, uint64_t remove, std::string_view insert) {
        ready();
        require(track_reads, FQDB_INVALID_ARGUMENT);
        auto source = get(key);
        require(offset <= source->size() && remove <= source->size() - offset,
                FQDB_INVALID_ARGUMENT);
        require(insert.size() <= FQDB_MAX_VALUE_BYTES - (source->size() - remove), FQDB_LIMIT);
        if (auto existing = write_find(writes, key); existing && existing->value.value)
            source.reset(); // Expiry-only writes still need their borrowed base value.
        try {
            WriteBuilder edit(std::move(writes), budget, base->core->transaction_budget);
            auto current = edit.editable(key);
            Write value;
            if (current)
                value = *current;
            else {
                value.original = depend(key);
                value.expires = value.original.expires;
            }
            // current's value belongs to the private path. Move it out before
            // testing uniqueness, after savepoint aliases have been isolated.
            auto old = current ? std::move(current->value) : std::shared_ptr<StagedData>{};
            value.value.reset();
            const bool recipe = !current && value.original.depth < 8;
            if (old && old.use_count() == 1)
                value.value = std::move(old);
            else if (old)
                value.value = data(*old);
            else
                value.value = data(*source);
            patch_buffer(*value.value, offset, remove, insert);
            value.edited = true;
            value.kind = recipe ? OpKind::patch : OpKind::put;
            if (recipe) {
                value.insert = data(insert);
                value.offset = offset;
                value.remove = remove;
            } else
                value.insert.reset();
            edit.set(key, std::move(value));
            writes = edit.finish();
        } catch (...) {
            broken = true;
            throw;
        }
    }
    uint64_t savepoint() {
        ready();
        require(savepoints.size() < 1024 && next_savepoint != UINT64_MAX, FQDB_LIMIT);
        auto id = next_savepoint + 1;
        savepoints.push_back(
            {id, writes, retained,
             Charge(budget, base->core->transaction_budget, sizeof(Savepoint) + 64)});
        next_savepoint = id;
        return id;
    }
    void rollback_to(uint64_t id) {
        ready(true);
        auto found = std::find_if(savepoints.begin(), savepoints.end(),
                                  [&](const Savepoint &point) { return point.id == id; });
        require(found != savepoints.end(), FQDB_INVALID_ARGUMENT);
        writes = found->root;
        retained = found->retained;
        savepoints.erase(found + 1, savepoints.end());
        broken = false;
    }
    void release(uint64_t id) {
        ready();
        require(!savepoints.empty() && savepoints.back().id == id, FQDB_INVALID_ARGUMENT);
        savepoints.pop_back();
    }
    void range(Bytes begin, Bytes end) {
        ready();
        require(end.empty() || compare(begin, end) <= 0, FQDB_INVALID_ARGUMENT);
        if (!track_reads)
            return;
        require(ranges.size() < 1024, FQDB_LIMIT);
        Charge charge(budget, base->core->transaction_budget,
                      begin.size() + end.size() + sizeof(Range) + 64);
        ranges.push_back({std::move(begin), std::move(end), std::move(charge)});
    }
    void retain(std::shared_ptr<const void> owner, uint64_t bytes) {
        for (auto item = retained; item; item = item->previous)
            if (item->owner.get() == owner.get())
                return;
        require(!retained || retained->count < 1024, FQDB_LIMIT);
        retained = std::make_shared<RetainedInput>(std::move(owner), retained, *this, bytes);
    }
    void finish() noexcept {
        sealed = true;
        writes.reset();
        savepoints.clear();
        reads.clear();
        ranges.clear();
        retained.reset();
    }
};
inline void CommitWaiter::finish() noexcept {
    if (transaction)
        transaction->finish();
    release_private();
}
inline uint64_t Core::commit(Transaction &transaction, fqdb_log_event *trace, bool force_reliable) {
    return commit_checked(transaction, trace, [] {}, force_reliable);
}
template <class Check>
inline uint64_t Core::commit_checked(Transaction &transaction, fqdb_log_event *trace, Check &&check,
                                     bool force_reliable) {
    check();
    transaction.ready();
    if (!transaction.writes && !force_reliable) {
        const auto version = transaction.base->view->version;
        transaction.finish();
        if (trace) {
            trace->database_id = id;
            trace->version = version;
            trace->stage = FQDB_STAGE_INDEX;
        }
        return version;
    }
    CommitWaiter request;
    request.transaction = &transaction;
    request.force_reliable = force_reliable;
    request.trace = trace;
    if constexpr (!std::is_empty<typename std::remove_reference<Check>::type>::value) {
        request.context = &check;
        request.check = [](void *context) {
            (*static_cast<typename std::remove_reference<Check>::type *>(context))();
        };
        // Explicit maintenance carries a caller-owned deadline/I/O context.
        // Keep its existing metered, serialized path; never borrow that TLS on
        // the periodic worker or treat administrative work as an ordinary put.
        return commit_direct(request);
    }
    if (storage::io_quota)
        return commit_direct(request);
    return submit(request);
}
inline std::shared_ptr<const View> Core::append_locked(CommitWaiter &request,
                                                       std::shared_ptr<const View> old) {
    maintenance_at(43);
    request.ready();
    require(!poisoned, FQDB_RECOVERY_REQUIRED);
    if (request.prepare)
        request.prepare(request, old);
    if (request.single) {
        auto &single = *request.single;
        if (request.trace) {
            request.trace->stage = FQDB_STAGE_INDEX;
            request.trace->durability =
                request.reliable ? FQDB_DURABILITY_SYNC : FQDB_DURABILITY_BUFFERED;
            request.trace->database_id = id;
            request.trace->operation_count = single.changed ? 1 : 0;
        }
        if (!single.changed) {
            request.version = old->version;
            return old;
        }
        return append_writes(request, old, SingleWriteCursor(single), old->checkpoint.features,
                             std::max(old->checkpoint.time, unix_ms()));
    }
    require(request.transaction != nullptr, FQDB_INTERNAL_ERROR);
    auto &transaction = *request.transaction;
    auto trace = request.trace;
    transaction.ready();
    if (trace) {
        trace->stage = FQDB_STAGE_INDEX;
        trace->durability = request.reliable ? FQDB_DURABILITY_SYNC : FQDB_DURABILITY_BUFFERED;
        trace->database_id = id;
        trace->operation_count = transaction.writes ? transaction.writes->count : 0;
    }
    if (!transaction.writes) {
        auto version = transaction.base->view->version;
        request.version = version;
        return old;
    }
    require(old->generation == transaction.base->view->generation &&
                old->checkpoint.features == transaction.base->view->checkpoint.features,
            FQDB_CONFLICT);
    if (!transaction.skip_indexes && ((old->checkpoint.features & 1) || transaction.advanced))
        try {
            prepare_features(transaction, *old);
        } catch (...) {
            transaction.broken = true;
            throw;
        }
    // The exact captured immutable view is still current under the writer
    // lease: every point/range dependency is unchanged. A different view,
    // including an unrelated publication, keeps the full conflict checks.
    if (old != transaction.base->view) {
        for (const auto &entry : transaction.reads) {
            auto value = old->find(entry.first);
            require((value ? value->version : 0) == entry.second.metadata.version, FQDB_CONFLICT);
        }
        for (const auto &range : transaction.ranges)
            require(old->range_max(range.begin, range.end) <= transaction.base->view->version,
                    FQDB_CONFLICT);
    }
    // Every accepted WAL tail must leave a sequence available for recovery.
    // compact() starts a new generation and resets this per-file sequence.
    require(old->version != UINT64_MAX && old->checkpoint.sequence != UINT64_MAX, FQDB_LIMIT);
    auto features =
        old->checkpoint.features | (transaction.advanced ? 1u : 0u) | transaction.feature_bits;
    auto commit_time = transaction.imported_time ? transaction.imported_time
                                                 : std::max(old->checkpoint.time, unix_ms());
    return append_writes(request, std::move(old), WriteCursor(transaction.writes), features,
                         commit_time);
}
template <class Cursor>
inline std::shared_ptr<const View>
Core::append_writes(CommitWaiter &request, std::shared_ptr<const View> old, Cursor writes,
                    uint64_t features, uint64_t commit_time) {
    require(old->version != UINT64_MAX && old->checkpoint.sequence != UINT64_MAX, FQDB_LIMIT);
    const auto version = old->version + 1;
    auto trace = request.trace;
    if (!request.reliable && options.durability == FQDB_DURABILITY_SYNC) {
        // Saturate externally supplied budgets at the supported file limit.
        // Wrapping a large valid budget would poison the first deferred append.
        uint64_t maximum = std::min<uint64_t>(options.all_transaction_bytes, INT64_MAX);
        maximum += std::min<uint64_t>(write_policy.threshold_bytes, INT64_MAX - maximum);
        maximum += std::min<uint64_t>(options.writer_bytes, INT64_MAX - maximum);
        old->generation->file->buffer_writes(maximum);
    }
    WalBatch wal(version, commit_time, features);
    auto cursor = writes;
    while (auto node = cursor.next()) {
        const auto &value = node->value;
        Value bytes;
        if (value.kind == OpKind::put)
            bytes = value.value;
        else if (value.kind == OpKind::patch)
            bytes = value.insert;
        auto kind = value.kind;
        if (save_candidate(kind, bytes))
            if (auto delta = save_delta(*old->generation, value.original, bytes)) {
                // An unchanged value can reuse its existing storage directly.
                if (storage::get32(reinterpret_cast<const unsigned char *>(delta->data())) == 0) {
                    bytes.reset();
                    kind = OpKind::expiry;
                } else {
                    bytes = std::move(delta);
                    kind = OpKind::multi;
                }
            }
        wal.add(kind, Bytes(node->key.view()), std::move(bytes), value.original, value.expires,
                kind == OpKind::multi ? 0 : value.offset, kind == OpKind::multi ? 0 : value.remove,
                value.value ? value_crc(value.value) : 0);
    }
    auto refs = wal.references(old->generation->tail);
    DeltaBuilder edit(old->delta);
    cursor = writes;
    auto next = std::make_shared<View>(*old);
    size_t index = 0;
    while (auto node = cursor.next()) {
        auto ref = refs[index++];
        // stage() recorded this dependency and append_locked validated its
        // version. SingleWrite obtained it under this same writer lease.
        // Tombstones have a version too; only version zero means absent.
        auto previous = node->value.original.version ? std::optional<Ref>(node->value.original)
                                                     : std::optional<Ref>{};
        count_change(*next, previous, ref);
        edit.set(Bytes(node->key.view()), ref);
    }
    next->delta = edit.finish();
    require(next->delta->bytes <= options.delta_bytes, FQDB_LIMIT);
    next->checkpoint.features = features;
    next->checkpoint.time = std::max(next->checkpoint.time, commit_time);
    next->version = version;
    next->limit = old->generation->tail + wal.size() + 56;
    request.ready();
    if (request.transaction)
        request.transaction->ready();
    if (trace) {
        trace->stage = FQDB_STAGE_WRITE;
        trace->file_offset = old->generation->tail;
        trace->payload_bytes = wal.size();
    }
    try {
        store_crash_at(1);
        refs = wal.append(*old->generation, &refs);
        store_crash_at(2);

    } catch (const IOBudgetStop &) {
        // WAL reservations reject before their first byte, so this request
        // has not appended anything and the live writer remains usable.
        throw;
    } catch (...) {
        poisoned = true;
        write_failure.store(FQDB_COMMIT_UNKNOWN, std::memory_order_release);
        throw Error(FQDB_COMMIT_UNKNOWN);
    }
    const bool consume_values = request.single || request.consume_cache_values;
    if (consume_values)
        wal.release_small_values();
    // Only cache-owned Data escapes here: never retain transaction budget
    // charges or mutate aliases held by savepoints, batches or private cursors.
    if (options.cache_bytes) {
        cursor = writes;
        size_t i = 0;
        while (auto node = cursor.next()) {
            const auto &ref = refs[i++];
            const auto &value = node->value.value;
            // Preserve edits and repeated bounded overwrites as delta candidates.
            // First inserts do not populate the read cache just for writing.
            const bool repeated = save_candidate(node->value.kind, value) &&
                                  node->value.original.version && !node->value.original.deleted();
            if (!node->value.edited && !repeated &&
                (!node->value.original.valid() ||
                 !old->generation->values.recently_read(node->value.original.offset)))
                continue;
            if (ref.offset == node->value.original.offset && ref.crc == node->value.original.crc &&
                ref.size == node->value.original.size && ref.depth == node->value.original.depth &&
                ref.recipe_size == node->value.original.recipe_size)
                continue;
            if (!value || ref.deleted() ||
                !old->generation->values.admits(sizeof(Data) + value->capacity()))
                continue;
            try {
                auto cached = cache_data(value, ref.crc, consume_values);
                const bool admitted = old->generation->values.put(
                    ref.offset, cached, sizeof(Data) + cached->capacity(),
                    (uint64_t(ref.depth) << 32) | ref.recipe_size, false);
                if (admitted && node->value.original.valid() &&
                    node->value.original.offset != ref.offset)
                    old->generation->values.retire(node->value.original.offset);
            } catch (...) { /* Optional caching cannot fail an appended transaction. */
            }
        }
    }
    request.write_bytes = wal.size() + 56;
    dirty_bytes.fetch_add(request.write_bytes, std::memory_order_relaxed);
    request.appended = true;
    request.version = version;
    return next;
}
} // namespace fengqudb::paged
