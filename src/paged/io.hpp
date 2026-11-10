#pragma once
#include "native_io.hpp"
#include "segments.hpp"
#include "cell_file.hpp"
#include "hybrid_file.hpp"
#include "deferred_io.hpp"

namespace fengqudb::paged {
// Keep one root handle alive independently of the read cache: its lease must
// protect this generation even while data-segment handles are being evicted.
class SharedFile {
    std::shared_ptr<NativeFile> root_ = std::make_shared<NativeFile>();
    std::unique_ptr<SegmentedFile> segmented_;
    std::unique_ptr<HybridFile> arena_;
    std::string path_;
    DeferredIO deferred_;
    void read_raw(uint64_t offset, void *data, size_t size) {
        if (arena_)
            arena_->read(offset, data, size);
        else if (segmented_)
            segmented_->read(offset, data, size);
        else
            root_->read(offset, data, size);
    }
    void write_raw(uint64_t offset, const void *data, size_t size) {
        if (arena_)
            arena_->write(offset, data, size);
        else if (segmented_)
            segmented_->write(offset, data, size);
        else
            root_->write(offset, data, size);
    }
    void sync_raw() {
        if (arena_)
            arena_->sync();
        else if (segmented_)
            segmented_->sync();
        else
            root_->sync();
    }

  public:
    void buffer_writes(uint64_t maximum) {
        if (!accelerated())
            deferred_.enable(maximum, [&] {
                return arena_ ? arena_->size() : (segmented_ ? segmented_->size() : root_->size());
            });
    }
    std::atomic<uint64_t> reads{0}, writes{0}, syncs{0}, read_bytes{0}, written_bytes{0};
    std::atomic<uint64_t> punches{0}, punched_bytes{0};
    bool exclusively_created() const noexcept {
        return root_->exclusively_created();
    }
    uint64_t segment_bytes() const noexcept {
        return segmented_ ? segmented_->geometry() : 0;
    }
    uint64_t reservation_bytes(uint64_t offset, uint64_t size) const {
        if (arena_) {
            if (storage::io_quota)
                arena_->materialize();
            arena_->prepare(offset, size);
        }
        return segmented_ ? segmented_->reservation_bytes(offset, size) : size;
    }
    void initialize_arena(std::array<unsigned char, 16> uuid, uint64_t generation) {
        require(!segmented_ && !arena_, FQDB_INVALID_ARGUMENT);
        arena_ = std::make_unique<HybridFile>(root_, path_);
        try {
            arena_->create(uuid, generation);
        } catch (...) {
            // Both targets belong to this failed creation; an existing sidecar
            // that prevented CREATE_NEW is never removed.
            arena_->remove_companion();
            if (root_->exclusively_created())
                remove_owned(path_);
            throw;
        }
    }
    void arena_identity(const std::array<unsigned char, 16> &uuid, uint64_t generation) const {
        if (arena_)
            arena_->identity(uuid, generation);
    }
    void create_segmented(const std::string &path, std::array<unsigned char, 16> uuid,
                          uint64_t generation, uint64_t geometry) {
        segmented_ = std::make_unique<SegmentedFile>(
            std::filesystem::u8path(path).parent_path().u8string(), uuid, generation, geometry);
        require(segmented_->path(0) == path, FQDB_INVALID_ARGUMENT);
        try {
            segmented_->create();
        } catch (...) {
            if (auto primary = segmented_->primary())
                root_ = std::move(primary);
            throw;
        }
        root_ = segmented_->primary();
    }
    void open(const std::string &path, bool create_only = false, bool existing_only = false,
              bool read_only = false) {
        path_ = path;
        root_->open(path, create_only, existing_only, read_only);
        if (create_only || root_->size() < 128)
            return;
        std::array<unsigned char, 8> arena_magic{};
        root_->read(64, arena_magic.data(), arena_magic.size());
        if (!std::memcmp(arena_magic.data(), "FQAHYB01", 8)) {
            arena_ = std::make_unique<HybridFile>(root_, path_, read_only);
            return; // load only after the exclusive file lease is acquired
        }
        const auto name = std::filesystem::u8path(path).filename().u8string();
        if (!((name.size() == 21 && name[0] == 'g' && name.substr(17) == ".fdb") ||
              (name.size() == 57 && name.substr(0, 4) == "gen-" && name.substr(53) == ".fdb")))
            return;
        // Byte zero can be leased by a live generation. The durable mirror
        // allows read-only inspection without touching another owner's byte.
        std::array<unsigned char, 64> header{};
        root_->read(64, header.data(), header.size());
        if (std::memcmp(header.data(), "FQSEG002", 8))
            return;
        require(storage::get32(header.data() + 60) == storage::crc32(header.data(), 60));
        std::array<unsigned char, 16> uuid{};
        std::memcpy(uuid.data(), header.data() + 32, uuid.size());
        segmented_ =
            std::make_unique<SegmentedFile>(std::filesystem::u8path(path).parent_path().u8string(),
                                            uuid, storage::get64(header.data() + 16),
                                            storage::get64(header.data() + 8), 8, root_, read_only);
        require(segmented_->path(0) == path);
        segmented_->open();
    }
    void lock() {
        root_->lock();
        if (arena_)
            arena_->load();
        if (segmented_)
            segmented_->validate_primary_locked();
    }
    void unlock() noexcept {
        root_->unlock();
    }
    uint64_t size() const {
        if (deferred_.enabled())
            return deferred_.size();
        return arena_ ? arena_->size() : (segmented_ ? segmented_->size() : root_->size());
    }
    void read(uint64_t offset, void *data, size_t size) {
        if (deferred_.enabled())
            deferred_.read(offset, data, size,
                           [&](uint64_t p, void *v, size_t n) { read_raw(p, v, n); });
        else
            read_raw(offset, data, size);
        ++reads;
        read_bytes += size;
    }
    void write(uint64_t offset, const void *data, size_t size) {
        if (arena_ && arena_->overwritten(offset, size))
            arena_->materialize();
#ifdef FQDB_TESTING
        // A staged file write is still an I/O API boundary even when physical
        // writes are deferred until sync. Inject before publishing staged bytes.
        if (arena_ && size && fault_countdown(paged_fail_write))
            throw Error(FQDB_IO_ERROR);
#endif
        if (deferred_.enabled())
            deferred_.write(offset, data, size);
        else if (arena_)
            arena_->write(offset, data, size);
        else if (segmented_)
            segmented_->write(offset, data, size);
        else
            root_->write(offset, data, size);
        ++writes;
        written_bytes += size;
    }
    bool checksummed_records() const {
        return arena_ && arena_->checksums();
    }
    bool accelerated() const {
        return arena_ && arena_->accelerated();
    }
    void write_known(uint64_t offset, const void *data, size_t size, uint32_t crc,
                     std::shared_ptr<const void> owner = {}) {
        if (deferred_.enabled()) {
            deferred_.write(offset, data, size, std::move(owner));
            ++writes;
            written_bytes += size;
        } else if (arena_) {
            arena_->write_known(offset, data, size, crc, std::move(owner));
            ++writes;
            written_bytes += size;
        } else
            write(offset, data, size);
    }
    void sync() {
        if (deferred_.enabled())
            deferred_.sync([&](uint64_t p, const void *v, size_t n) { write_raw(p, v, n); },
                           [&] { sync_raw(); });
        else
            sync_raw();
        ++syncs;
    }
    void truncate(uint64_t length) {
        if (deferred_.enabled())
            sync();
        if (arena_ && length < arena_->size())
            arena_->materialize();
        if (arena_)
            arena_->truncate(length);
        else if (segmented_)
            segmented_->truncate(length);
        else
            root_->truncate(length);
        if (deferred_.enabled())
            deferred_.truncated(length);
    }
    void prepare_sparse() {
        if (arena_)
            arena_->prepare_sparse();
        else if (segmented_)
            segmented_->prepare_sparse();
        else
            root_->prepare_sparse();
    }
    bool prepare_sparse_slice() {
        if (arena_)
            return arena_->prepare_sparse_slice();
        prepare_sparse();
        return true;
    }
    void sync_materialization() {
        if (arena_)
            arena_->sync_materialization();
    }
    uint64_t allocated() const {
        return arena_ ? arena_->allocated()
                      : (segmented_ ? segmented_->allocated() : root_->allocated());
    }
    std::optional<std::pair<uint64_t, uint64_t>> next_data(uint64_t begin, uint64_t end) {
        if (arena_)
            return arena_->next_data(begin, end);
        return segmented_ ? segmented_->next_data(begin, end) : root_->next_data(begin, end);
    }
    void punch(uint64_t offset, uint64_t length) {
        if (arena_)
            arena_->punch(offset, length);
        else if (segmented_)
            segmented_->punch(offset, length);
        else
            root_->punch(offset, length);
        ++punches;
        punched_bytes += length;
    }
    void remove_files(const std::string &path) noexcept {
        try {
            if (segmented_)
                segmented_->remove_files();
            else {
                if (arena_)
                    arena_->remove_companion();
                remove_owned(path);
            }
        } catch (...) {
        }
    }
    std::function<void()> cleanup_plan() const {
        if (arena_) {
            auto root = path_;
            auto extra = arena_->companion();
            const bool own = arena_->owns_companion();
            return [root, extra, own] {
                if (own)
                    remove_owned(extra);
                remove_owned(root);
            };
        }
        return segmented_ ? segmented_->cleanup_plan() : std::function<void()>{};
    }
};
} // namespace fengqudb::paged
