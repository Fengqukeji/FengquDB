#pragma once
#include "history.hpp"

namespace fengqudb::paged {
inline thread_local bool background_maintenance = false;
inline fqdb_store_maintenance_options maintenance_defaults() {
    return {sizeof(fqdb_store_maintenance_options), 1, 8, 0, 0, 1048576, 0, 0, nullptr};
}
class MaintenanceIO {
    storage::IOQuota quota_;
    storage::IOQuota *previous_ = storage::io_quota;
    fqdb_store_maintenance_io *output_ = nullptr;
    bool owner_ = false;

  public:
    explicit MaintenanceIO(const fqdb_store_maintenance_options &options) {
        if (!previous_ && (options.max_read_bytes || options.max_write_bytes || options.io_stats)) {
            quota_.max_read = options.max_read_bytes;
            quota_.max_write = options.max_write_bytes;
            output_ = options.io_stats;
            if (output_)
                *output_ = {};
            storage::io_quota = &quota_;
            owner_ = true;
        }
    }
    MaintenanceIO(const MaintenanceIO &) = delete;
    MaintenanceIO &operator=(const MaintenanceIO &) = delete;
    ~MaintenanceIO() {
        if (owner_) {
            if (output_)
                *output_ = {quota_.read_bytes, quota_.write_bytes, quota_.sync_calls};
            storage::io_quota = previous_;
        }
    }
    bool rejected() const noexcept {
        return storage::io_quota && storage::io_quota->rejected;
    }
};
class MaintenanceJob {
    Core &core_;
    std::unique_lock<std::mutex> lease_;
    MaintenanceIO io_;
    std::chrono::steady_clock::time_point deadline_{};
    bool timed_ = false;
    uint64_t source_bytes_ = 0;

  public:
    MaintenanceJob(Core &core, const fqdb_store_maintenance_options &options)
        : core_(core), lease_(core.maintenance, std::try_to_lock), io_(options) {
        require(lease_.owns_lock(), FQDB_LOCKED);
        if (options.timeout_ms) {
            require(options.timeout_ms <= static_cast<uint64_t>(INT64_MAX / 1000000),
                    FQDB_INVALID_ARGUMENT);
            auto now = std::chrono::steady_clock::now();
            auto delay = std::chrono::milliseconds(options.timeout_ms);
            require(delay <= std::chrono::steady_clock::time_point::max() - now,
                    FQDB_INVALID_ARGUMENT);
            deadline_ = now + delay;
            timed_ = true;
        }
        core_.maintenance_cancelled.store(false, std::memory_order_relaxed);
        core_.maintenance_active.store(true, std::memory_order_release);
    }
    ~MaintenanceJob() {
        core_.maintenance_active.store(false, std::memory_order_release);
    }
    void check() const {
        if (io_.rejected())
            throw IOBudgetStop();
        require(!background_maintenance || !core_.background_stop.load(std::memory_order_relaxed),
                FQDB_CANCELLED);
        require(!core_.maintenance_cancelled.load(std::memory_order_relaxed), FQDB_CANCELLED);
        require(!timed_ || std::chrono::steady_clock::now() < deadline_, FQDB_TIMEOUT);
    }
    std::shared_ptr<Capture> capture() {
        std::lock_guard<WriterGate> lock(core_.writer);
        require(!core_.poisoned, FQDB_RECOVERY_REQUIRED);
        check();
        auto result = std::make_shared<Capture>(core_.shared_from_this());
        source_bytes_ = result->view->generation->file->size();
        return result;
    }
    uint64_t source_bytes() const noexcept {
        return source_bytes_;
    }
};
template <class Check> inline void verify_index(const View &view, Check &&check) {
    struct Totals {
        uint64_t entries = 0, keys = 0, bytes = 0;
    } total;
    std::function<void(uint64_t, uint64_t, std::optional<std::string_view>,
                       std::optional<std::string_view>, uint64_t, uint64_t, unsigned)>
        visit;
    visit = [&](uint64_t offset, uint64_t ceiling, std::optional<std::string_view> minimum,
                std::optional<std::string_view> upper, uint64_t maximum, uint64_t earliest,
                unsigned depth) {
        check();
        require(depth < 64 && offset >= data_start && offset < ceiling);
        auto page = view.generation->page(offset, ceiling, false);
        require(page->frame_version <= view.checkpoint.version &&
                page->maximum <= view.checkpoint.version);
        if (minimum)
            require(page->slots.front().key == *minimum && page->maximum == maximum);
        if (earliest)
            require(page->earliest == earliest);
        if (upper)
            require(compare(page->slots.back().key, *upper) < 0);
        for (size_t i = 0; i < page->slots.size(); ++i) {
            const auto &slot = page->slots[i];
            if (page->leaf) {
                valid_ref(slot.value, offset, true);
                ++total.entries;
                if (!slot.value.deleted()) {
                    ++total.keys;
                    total.bytes += slot.value.size;
                }
            } else
                visit(slot.child, offset, slot.key,
                      i + 1 < page->slots.size()
                          ? std::optional<std::string_view>(page->slots[i + 1].key)
                          : upper,
                      slot.maximum, slot.earliest, depth + 1);
        }
    };
    if (view.checkpoint.root)
        visit(view.checkpoint.root, view.checkpoint.end, {}, {}, 0, 0, 0);
    require(total.entries == view.checkpoint.entries && total.keys == view.checkpoint.user_keys &&
            total.bytes == view.checkpoint.live_bytes);
}
template <class Check>
inline void verify_value(Generation &generation, const Ref &ref, uint64_t limit, Check &&check) {
    valid_ref(ref, limit);
    if (ref.depth) {
        check();
        (void)generation.value(ref, limit, false, false);
        check();
        return;
    }
    std::array<char, 262144> bytes;
    uint64_t position = 0;
    uint32_t crc = 0;
    while (position < ref.size) {
        check();
        auto size = static_cast<size_t>(std::min<uint64_t>(bytes.size(), ref.size - position));
        generation.file->read(ref.offset + position, bytes.data(), size);
        crc = storage::crc32(bytes.data(), size, crc);
        position += size;
    }
    require(crc == ref.crc);
}
class CompactTarget {
    uint64_t writer_budget_, delta_budget_, cutoff_;
    MaintenanceJob &job_;

  public:
    std::shared_ptr<Generation> generation;
    View view;
    uint64_t copied = 0;
    CompactTarget(const std::string &path, Uuid uuid, uint64_t number, Core &core,
                  MaintenanceJob &job, uint64_t cutoff)
        : writer_budget_(core.options.writer_bytes), delta_budget_(core.options.delta_bytes),
          cutoff_(cutoff), job_(job), generation(std::make_shared<Generation>(0)) {
        generation->remove_on_close.store(true, std::memory_order_relaxed);
        auto source = core.current();
        auto geometry = source ? source->generation->file->segment_bytes() : 0;
        if (core.options.flags & FQDB_STORE_SEGMENTED)
            geometry = geometry ? geometry : new_segment_geometry();
        generation->create(path, uuid, number, geometry);
        view.generation = generation;
        view.limit = generation->tail;
    }
    void flush(uint64_t version) {
        if (!view.delta)
            return;
        view.checkpoint.root =
            fold_delta(generation, view.checkpoint.root, view.delta, version, writer_budget_);
        view.delta.reset();
        view.limit = generation->tail;
    }
    void apply(const View &source, const Metadata &row, bool empty_base = false) {
        job_.check();
        auto old = empty_base ? std::optional<Ref>{} : view.find(row.key);
        Ref ref;
        if (!row.value.visible(cutoff_)) {
            if (!old)
                return;
            ref.version = row.value.version;
        } else
            ref = copy_value(*source.generation, row.value, source.limit, *generation, row.key,
                             cutoff_, [&] { job_.check(); });
        count_change(view, old, ref);
        DeltaBuilder edit(std::move(view.delta));
        edit.set(Bytes(row.key), ref);
        view.delta = edit.finish();
        view.limit = generation->tail;
        ++copied;
        if (view.delta->bytes > delta_budget_ / 2)
            flush(source.version);
    }
    void copy(const View &source, uint64_t since = 0) {
        // ViewCursor merges disk and delta into one row per key. If the target
        // starts empty, no row in this pass can replace an earlier target key;
        // avoid rereading/decoding its growing, deliberately uncached B-tree.
        // Catch-up over a populated target must still resolve prior values.
        const bool empty_base = !view.checkpoint.root && !view.delta && !view.entries;
        ViewCursor cursor(source, {}, false, since);
        while (auto row = cursor.next())
            apply(source, *row, empty_base);
        view.version = source.version;
        view.checkpoint.time = std::max(view.checkpoint.time, source.checkpoint.time);
        view.checkpoint.features = source.checkpoint.features;
        view.checkpoint.origin_hi = source.checkpoint.origin_hi;
        view.checkpoint.origin_lo = source.checkpoint.origin_lo;
    }
    void seal(uint64_t version) {
        job_.check();
        flush(version);
        auto &checkpoint = view.checkpoint;
        checkpoint.version = version;
        checkpoint.entries = view.entries;
        checkpoint.user_keys = view.keys;
        checkpoint.live_bytes = view.live_bytes;
        checkpoint.time = std::max(checkpoint.time, cutoff_);
        checkpoint.floor = version;
        generation->seal(checkpoint);
        view.version = view.durable = version;
        view.limit = generation->tail;
    }
};
class OutputDirectory {
    std::string path_, destination_;
    std::vector<std::string> files_;
    std::vector<std::pair<std::string, std::function<void()>>> generations_;
    storage::File lease_;
    bool owns_ = false;

  public:
    explicit OutputDirectory(const std::string &destination)
        : destination_(directory_path(destination)) {
        auto path = std::filesystem::u8path(destination_);
        std::error_code error;
        require(!std::filesystem::exists(path, error), FQDB_ALREADY_EXISTS);
        require(!error, FQDB_IO_ERROR);
        // A short destination can have a longer staging sibling. Validate its
        // internal-file headroom before creating anything on disk.
        path_ = directory_path(
            (path.parent_path() / std::filesystem::u8path(temporary_name('t', new_uuid())))
                .u8string());
        files_.reserve(3);
        require(std::filesystem::create_directory(std::filesystem::u8path(path_), error) && !error,
                FQDB_IO_ERROR);
        owns_ = true;
        try {
            auto lock_path = join(path_, "LOCK");
            auto status = lease_.open(lock_path.c_str(), true);
            require(status == FQDB_OK, status);
            files_.push_back(std::move(lock_path));
        } catch (...) {
            clean();
            throw;
        }
    }
    ~OutputDirectory() {
        clean();
    }
    const std::string &path() const noexcept {
        return path_;
    }
    void retain_file(std::string path) {
        files_.push_back(std::move(path));
    }
    void retain_generation(const std::shared_ptr<Generation> &generation) {
        if (auto plan = generation->file->cleanup_plan())
            generations_.emplace_back(generation->path, std::move(plan));
    }
    void clean() noexcept {
        if (!owns_)
            return;
        lease_.close();
        for (auto &generation : generations_) {
            try {
                generation.second();
            } catch (...) {
            }
        }
        for (const auto &file : files_)
            if (std::none_of(generations_.begin(), generations_.end(),
                             [&](const auto &g) { return g.first == file; }))
                remove_owned(file);
        try {
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::u8path(path_), ignored);
        } catch (...) {
        }
        owns_ = false;
    }
    void release_lease() {
        lease_.close();
    }
    fqdb_status publish() {
        lease_.close();
        auto status = publish_directory(path_, destination_);
        if (status == FQDB_OK)
            owns_ = false;
        return status;
    }
};
class CacheTransfer {
    Generation &old_, &next_;
    uint64_t bytes_;
    bool accepted_ = false;

  public:
    CacheTransfer(Generation &old, Generation &next, uint64_t bytes)
        : old_(old), next_(next), bytes_(bytes) {
        old_.configure_cache(0);
        try {
            next_.configure_cache(bytes_);
        } catch (...) {
            old_.configure_cache(bytes_);
            throw;
        }
    }
    ~CacheTransfer() {
        if (!accepted_)
            try {
                next_.configure_cache(0);
                old_.configure_cache(bytes_);
            } catch (...) {
            }
    }
    void accept() noexcept {
        accepted_ = true;
    }
};
inline fqdb_store_maintenance_info Core::compact(const fqdb_store_maintenance_options &settings,
                                                 fqdb_log_event *trace) {
    MaintenanceJob job(*this, settings);
    auto base = job.capture();
    auto source = base->view;
    fqdb_store_maintenance_info info{};
    info.snapshot_version = source->version;
    info.source_file_bytes = job.source_bytes();
    require(source->generation->number != UINT64_MAX, FQDB_LIMIT);
    auto number = source->generation->number + 1;
    // A crashed unpublished attempt can own the next number; reclaim verified
    // unused generations before creating this exclusive candidate.
    (void)reclaim_generations(directory, source->generation->uuid, source->generation->number,
                              history_keep(*source));
    job.check();
    CompactTarget target(join(directory, generation_name(source->generation->uuid, number)),
                         source->generation->uuid, number, *this, job, base->time);
    if (trace) {
        trace->database_id = id;
        trace->stage = FQDB_STAGE_INDEX;
        trace->base_version = source->version;
    }
    verify_index(*source, [&] { job.check(); });
    target.copy(*source);
    target.seal(source->version);
    maintenance_at(10);
    for (uint32_t round = 0;; ++round) {
        job.check();
        std::unique_lock<WriterGate> lock(writer);
        require(!poisoned, FQDB_RECOVERY_REQUIRED);
        auto latest = current();
        require(latest->generation == source->generation, FQDB_CONFLICT);
        DeltaRoot planned;
        bool fits = true;
        uint64_t bytes = 0;
        const auto allowance = std::min(settings.max_barrier_bytes, options.delta_bytes);
        if (latest->version != target.view.version) {
            ViewCursor cursor(*latest, {}, false, target.view.version);
            DeltaBuilder plan;
            while (auto row = cursor.next()) {
                job.check();
                auto cost =
                    row->key.size() + 128 + (row->value.visible(base->time) ? row->value.size : 0);
                if (cost > allowance - bytes) {
                    fits = false;
                    break;
                }
                bytes += cost;
                plan.set(Bytes(row->key), row->value);
            }
            if (fits)
                planned = plan.finish();
        }
        if (!fits) {
            require(round < settings.max_catchup_rounds, FQDB_LIMIT);
            lock.unlock();
            target.copy(*latest, target.view.version);
            target.seal(latest->version);
            ++info.catchup_rounds;
            maintenance_at(14);
            continue;
        }
        const bool metadata_changed =
            target.view.checkpoint.features != latest->checkpoint.features ||
            target.view.checkpoint.time < latest->checkpoint.time;
        target.view.checkpoint.features = latest->checkpoint.features;
        target.view.checkpoint.time =
            std::max(target.view.checkpoint.time, latest->checkpoint.time);
        if (planned) {
            DeltaCursor cursor(planned);
            while (auto row = cursor.next())
                target.apply(*latest, {row->key.view(), row->value});
            target.seal(latest->version);
            ++info.catchup_rounds;
        } else if (target.view.version != latest->version || metadata_changed)
            target.seal(latest->version);
        if (plan_history(*this, *latest, target.view))
            target.seal(latest->version);
        auto next = std::make_shared<View>(target.view);
        job.check();
        CacheTransfer cache(*source->generation, *target.generation, options.cache_bytes);
        job.check();
        maintenance_at(11);
        if (trace)
            trace->stage = FQDB_STAGE_PUBLISH;
        fqdb_status status = FQDB_INTERNAL_ERROR;
        status = write_current(directory, {target.generation->uuid, number});
        if (status != FQDB_OK) {
            if (status == FQDB_COMMIT_UNKNOWN) {
                poisoned = true;
                target.generation->remove_on_close.store(false, std::memory_order_relaxed);
            }
            throw Error(status);
        }
        target.generation->remove_on_close.store(false, std::memory_order_relaxed);
        maintenance_at(12);
        publish(std::move(next));
        cache.accept();
        retire_history(*source, *current());
        maintenance_at(13);
        info.final_version = latest->version;
        info.output_file_bytes = target.view.limit;
        info.stored_keys = target.view.keys;
        info.stored_value_bytes = target.view.live_bytes;
        info.copied_keys = target.copied;
        if (trace) {
            trace->version = info.final_version;
            trace->operation_count = info.copied_keys;
            trace->payload_bytes = info.output_file_bytes;
        }
        return info;
    }
}
inline fqdb_store_maintenance_info Core::backup(const std::string &destination,
                                                const fqdb_store_maintenance_options &settings,
                                                fqdb_log_event *trace) {
    flush_periodic();
    const auto saved_policy=get_write_policy();
    MaintenanceJob job(*this, settings);
    auto base = job.capture();
    auto source = base->view;
    OutputDirectory output(destination);
    auto uuid = new_uuid();
    auto filename = generation_name(uuid, 1);
    output.retain_file(join(output.path(), "CURRENT"));
    fqdb_store_maintenance_info info{};
    info.snapshot_version = info.final_version = source->version;
    info.source_file_bytes = job.source_bytes();
    {
        CompactTarget target(join(output.path(), filename), uuid, 1, *this, job, base->time);
        output.retain_file(join(output.path(), filename));
        output.retain_generation(target.generation);
        if (trace) {
            trace->database_id = id;
            trace->stage = FQDB_STAGE_INDEX;
            trace->base_version = source->version;
        }
        verify_index(*source, [&] { job.check(); });
        target.copy(*source);
        target.view.checkpoint.origin_hi = storage::get64(source->generation->uuid.data());
        target.view.checkpoint.origin_lo = storage::get64(source->generation->uuid.data() + 8);
        target.seal(source->version);
        job.check();
        auto status = write_current(output.path(), {uuid, 1});
        require(status == FQDB_OK, status);
        target.generation->remove_on_close.store(false, std::memory_order_relaxed);
        info.output_file_bytes = target.view.limit;
        info.stored_keys = target.view.keys;
        info.stored_value_bytes = target.view.live_bytes;
        info.copied_keys = target.copied;
    }
    job.check();
    output.retain_file(join(output.path(),"WRITE"));
    require(save_write_policy(output.path(),saved_policy)==FQDB_OK,FQDB_IO_ERROR);
    maintenance_at(20);
    if (trace)
        trace->stage = FQDB_STAGE_PUBLISH;
    auto status = output.publish();
    require(status == FQDB_OK, status);
    maintenance_at(21);
    if (trace) {
        trace->version = info.final_version;
        trace->operation_count = info.copied_keys;
        trace->payload_bytes = info.output_file_bytes;
    }
    return info;
}
inline fqdb_store_maintenance_info Core::verify(const fqdb_store_maintenance_options &settings,
                                                fqdb_log_event *trace) {
    MaintenanceJob job(*this, settings);
    auto base = job.capture();
    auto view = base->view;
    if (trace) {
        trace->database_id = id;
        trace->stage = FQDB_STAGE_CHECKSUM;
        trace->version = view->version;
    }
    verify_index(*view, [&] { job.check(); });
    ViewCursor cursor(*view);
    uint64_t entries = 0, keys = 0, bytes = 0;
    std::optional<Bytes> previous;
    while (auto row = cursor.next()) {
        job.check();
        require(!previous || compare(*previous, row->key) < 0);
        previous = Bytes(row->key);
        require(row->value.version <= view->version);
        valid_ref(row->value, view->limit, true);
        ++entries;
        if (!row->value.deleted()) {
            verify_value(*view->generation, row->value, view->limit, [&] { job.check(); });
            ++keys;
            bytes += row->value.size;
        }
    }
    require(entries == view->entries && keys == view->keys && bytes == view->live_bytes);
    verify_features(*view, base->time, [&] { job.check(); });
    (void)history_pins(*view);
    if (view->history) {
        HistoryReader history(view, history_floor(*view));
        while (auto record = history.next([&] { job.check(); }))
            read_wal(*record->generation, record->frame, [&](const WalOperation &operation) {
                if (!operation.result.deleted())
                    verify_value(*record->generation, operation.result, record->frame.end,
                                 [&] { job.check(); });
            });
    }
    if (trace) {
        trace->operation_count = entries;
        trace->payload_bytes = bytes;
    }
    return {view->version, view->version, job.source_bytes(), 0, keys, bytes, 0, 0};
}
} // namespace fengqudb::paged
