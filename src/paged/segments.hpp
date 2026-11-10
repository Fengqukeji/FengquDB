#pragma once
#include "native_io.hpp"
#include <map>
#include <optional>
#include <random>
#include <functional>

namespace fengqudb::paged {

// Internal segmented positional I/O for Store generations.
// One writer/maintenance owner; concurrent readers retain file handles. The
// caller must prove a segment unreachable and retire its entire reader epoch.
class SegmentedFile {
    struct Handle {
        std::shared_ptr<NativeFile> file;
        uint64_t stamp = 0;
    };
    std::string directory_;
    std::shared_ptr<NativeFile> primary_;
    bool read_only_ = false;
    std::array<unsigned char, 16> uuid_{};
    uint64_t generation_, segment_bytes_;
    size_t capacity_;
    mutable std::mutex mutex_;
    mutable std::mutex mutation_;
    std::map<uint32_t, Handle> handles_;
    // Consecutive segment IDs share a range. Holes do not require an array
    // proportional to the historical tail; cap fragmented range metadata.
    using Ranges = std::map<uint32_t, uint32_t>;
    std::shared_ptr<Ranges> ranges_ = std::make_shared<Ranges>();
    bool created_here_ = false;
    bool sparse_ = false;
    // Writer-owned dirty references survive read-cache eviction. Readers never
    // flush files; each dirty eviction is flushed by the single writer.
    std::map<uint32_t, std::shared_ptr<NativeFile>> dirty_;
    std::atomic<uint64_t> length_{0};
    uint32_t highest_ = 0;
    uint64_t clock_ = 0;
    bool directory_dirty_ = false, poisoned_ = false;

    void remember_locked(uint32_t id) {
        auto right = ranges_->upper_bound(id);
        auto left = right == ranges_->begin() ? ranges_->end() : std::prev(right);
        if (left != ranges_->end() && left->second >= id)
            return;
        const bool joins_left = left != ranges_->end() && uint64_t(left->second) + 1 == id;
        const bool joins_right = right != ranges_->end() && uint64_t(id) + 1 == right->first;
        if (joins_left) {
            left->second = joins_right ? right->second : id;
            if (joins_right)
                ranges_->erase(right);
        } else if (joins_right) {
            auto node = ranges_->extract(right);
            node.key() = id;
            ranges_->insert(std::move(node));
        } else {
            require(ranges_->size() < 65536, FQDB_LIMIT);
            ranges_->emplace(id, id);
        }
    }
    void retire_locked(uint32_t id) {
        require(id && id < highest_, FQDB_INVALID_ARGUMENT);
        require(dirty_.find(id) == dirty_.end(), FQDB_LOCKED);
        auto owned = acquire_locked(id, false);
        auto range = ranges_->upper_bound(id);
        require(range != ranges_->begin());
        --range;
        require(range->first <= id && range->second >= id);
        const bool split = range->first < id && id < range->second;
        if (split) {
            require(ranges_->size() < 65536, FQDB_LIMIT);
            ranges_->emplace(id + 1, range->second); // Allocate before deletion.
        }
        std::error_code error;
        const bool removed = std::filesystem::remove(std::filesystem::u8path(path(id)), error);
        if (!removed || error) {
            if (split)
                ranges_->erase(id + 1);
            throw Error(FQDB_IO_ERROR);
        }
        if (range->first == id) {
            auto node = ranges_->extract(range);
            if (node.mapped() != id) {
                node.key() = id + 1;
                ranges_->insert(std::move(node));
            }
        } else
            range->second = id - 1;
        handles_.erase(id);
        directory_dirty_ = true;
    }

    static std::string digits(uint64_t value, unsigned count) {
        std::string out(count, '0');
        for (unsigned i = 0; i < count; ++i) {
            out[count - i - 1] = "0123456789abcdef"[value & 15];
            value >>= 4;
        }
        return out;
    }
    std::string name(uint32_t id) const {
        return id ? "s" + digits(generation_, 16) + "_" + digits(id, 8) + ".fdb"
                  : "g" + digits(generation_, 16) + ".fdb";
    }
    std::optional<uint32_t> identify(std::string_view text) const {
        if (text == name(0))
            return 0;
        const auto prefix = "s" + digits(generation_, 16) + "_";
        if (text.size() != 30 || text.substr(0, 18) != prefix || text.substr(26) != ".fdb")
            return {};
        uint32_t id = 0;
        for (size_t i = 18; i < 26; ++i) {
            const auto ch = text[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                return {};
            id = (id << 4) | (ch <= '9' ? ch - '0' : ch - 'a' + 10);
        }
        return id ? std::optional<uint32_t>(id) : std::optional<uint32_t>{};
    }
    void validate(NativeFile &file, uint32_t id) const {
        std::array<unsigned char, 64> header{};
        require(file.size() >= header_bytes && file.size() <= header_bytes + segment_bytes_);
        file.read(id ? 0 : 64, header.data(), header.size());
        require(std::memcmp(header.data(), "FQSEG002", 8) == 0 &&
                storage::get64(header.data() + 8) == segment_bytes_ &&
                storage::get64(header.data() + 16) == generation_ &&
                storage::get32(header.data() + 24) == id &&
                std::memcmp(header.data() + 32, uuid_.data(), uuid_.size()) == 0 &&
                storage::get32(header.data() + 60) == storage::crc32(header.data(), 60));
    }
    std::shared_ptr<NativeFile> acquire_locked(uint32_t id, bool create) {
        auto found = handles_.find(id);
        if (found != handles_.end()) {
            found->second.stamp = ++clock_;
            return found->second.file;
        }
        auto file = !id && primary_ ? primary_ : std::make_shared<NativeFile>();
        const auto filename = path(id);
        if (create) {
            // Publish only a durable identity header. A killed/failed creator
            // may leave an unpublished temp, never a torn authoritative header.
            std::random_device random;
            const auto token = (uint64_t(random()) << 32) ^ random();
            const auto temporary =
                (std::filesystem::u8path(directory_) / ("t" + digits(token, 16) + ".tmp"))
                    .u8string();
            bool uncertain = false;
            try {
                file->open(temporary, true);
                std::array<unsigned char, 64> header{};
                std::memcpy(header.data(), "FQSEG002", 8);
                storage::put64(header.data() + 8, segment_bytes_);
                storage::put64(header.data() + 16, generation_);
                storage::put32(header.data() + 24, id);
                std::memcpy(header.data() + 32, uuid_.data(), uuid_.size());
                storage::put32(header.data() + 60, storage::crc32(header.data(), 60));
                std::array<unsigned char, 128> mirrored{};
                std::memcpy(mirrored.data(), header.data(), 64);
                std::memcpy(mirrored.data() + 64, header.data(), 64);
                file->write(0, mirrored.data(), mirrored.size());
                file->truncate(header_bytes);
                if (sparse_)
                    file->prepare_sparse();
                file->sync();
                // These same-directory no-replace primitives accept files as
                // well as directories on the supported Windows/Linux paths.
                const auto status = publish_directory(temporary, filename, false);
                uncertain = status == FQDB_COMMIT_UNKNOWN;
                require(status == FQDB_OK, status);
                if (!id) {
                    primary_ = file;
                    created_here_ = true;
                }
                // New IDs extend the already allocated final range. Record
                // ownership before cache-node allocation can fail.
                remember_locked(id);
            } catch (...) {
                if (file->exclusively_created() && !uncertain)
                    remove_owned(temporary);
                throw;
            }
        } else {
            if (id || !primary_)
                file->open(filename, false, true, read_only_);
            validate(*file, id);
        }
        if (handles_.size() == capacity_) {
            auto victim = std::min_element(
                handles_.begin(), handles_.end(),
                [](const auto &a, const auto &b) { return a.second.stamp < b.second.stamp; });
            handles_.erase(victim);
        }
        handles_.emplace(id, Handle{file, ++clock_});
        return file;
    }
    std::shared_ptr<NativeFile> acquire(uint32_t id, bool create = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        return acquire_locked(id, create);
    }
    uint64_t maximum_length() const noexcept {
        return (uint64_t(UINT32_MAX) + 1) * segment_bytes_;
    }
    void mark_dirty(uint32_t id, const std::shared_ptr<NativeFile> &file) {
        if (dirty_.find(id) != dirty_.end())
            return;
        if (dirty_.size() == capacity_) {
            auto victim = dirty_.begin();
            victim->second->sync();
            dirty_.erase(victim);
        }
        dirty_.emplace(id, file);
    }

  public:
    static constexpr uint64_t header_bytes = 65536;
    SegmentedFile(std::string directory, std::array<unsigned char, 16> uuid, uint64_t generation,
                  uint64_t segment_bytes, size_t handles = 8,
                  std::shared_ptr<NativeFile> primary = {}, bool read_only = false)
        : directory_(std::move(directory)), primary_(std::move(primary)), read_only_(read_only),
          uuid_(uuid), generation_(generation), segment_bytes_(segment_bytes), capacity_(handles) {
        require(generation && handles && handles <= 64 && segment_bytes >= 65536 &&
                    segment_bytes <= (uint64_t(1) << 30) && !(segment_bytes & (segment_bytes - 1)),
                FQDB_INVALID_ARGUMENT);
        // No directory creation or writes before checking the longest name.
        require(std::filesystem::u8path(path(UINT32_MAX)).native().size() <
#ifdef _WIN32
                    240,
#else
                    960,
#endif
                FQDB_LIMIT);
    }
    std::string path(uint32_t id) const {
        return (std::filesystem::u8path(directory_) / name(id)).u8string();
    }
    void create() {
        require(length_.load() == 0 && handles_.empty(), FQDB_INVALID_ARGUMENT);
        try {
            auto file = acquire(0, true);
            primary_ = file;
            remember_locked(0);
            mark_dirty(0, file);
        } catch (...) {
            poisoned_ = true;
            throw;
        }
    }
    void open() {
        require(handles_.empty(), FQDB_INVALID_ARGUMENT);
        // No creation fallback: a missing segment is an IO failure, never an
        // implicit zero extent. Old missing segments are not needed for size.
        (void)acquire(0);
        uint32_t last = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(directory_)))
            if (auto id = identify(entry.path().filename().u8string())) {
                remember_locked(*id);
                last = std::max(last, *id);
            }
        auto file = acquire(last);
        highest_ = last;
        length_.store(uint64_t(last) * segment_bytes_ + file->size() - header_bytes,
                      std::memory_order_release);
    }
    uint64_t size() const noexcept {
        return length_.load(std::memory_order_acquire);
    }
    std::shared_ptr<NativeFile> primary() const {
        return primary_;
    }
    void validate_primary_locked() {
        std::array<unsigned char, 128> both{};
        primary_->read(0, both.data(), both.size());
        require(!std::memcmp(both.data(), both.data() + 64, 64));
        validate(*primary_, 0);
    }
    uint64_t geometry() const noexcept {
        return segment_bytes_;
    }
    bool poisoned() const noexcept {
        return poisoned_;
    } // writer-owned
    size_t cached_handles() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handles_.size();
    }
    size_t dirty_handles() const noexcept {
        return dirty_.size();
    } // writer-only
    void read(uint64_t offset, void *destination, size_t length) {
        const auto end = size();
        require(offset <= end && length <= end - offset, FQDB_IO_ERROR);
        auto out = static_cast<unsigned char *>(destination);
        while (length) {
            auto id = static_cast<uint32_t>(offset / segment_bytes_);
            const auto within = offset % segment_bytes_;
            const auto count =
                static_cast<size_t>(std::min<uint64_t>(length, segment_bytes_ - within));
            auto file = acquire(id);
            file->read(header_bytes + within, out, count);
            offset += count;
            out += count;
            length -= count;
        }
    }
    void write(uint64_t offset, const void *source, size_t length) {
        std::lock_guard<std::mutex> mutation(mutation_);
        require(!poisoned_, FQDB_RECOVERY_REQUIRED);
        require(!read_only_, FQDB_INVALID_ARGUMENT);
        const auto end = size();
        require(offset <= end && offset <= maximum_length() && length <= maximum_length() - offset,
                FQDB_LIMIT);
        if (!length)
            return;
        // Preflight the complete logical write plus worst-case segment headers.
        // The reservation rejects insufficient quotas before changing any file.
        const auto last = (offset + length - 1) / segment_bytes_;
        const auto new_segments = last > highest_ ? last - highest_ : 0;
        require(new_segments <= (UINT64_MAX - length) / 128, FQDB_LIMIT);
        IOWritePermit permit(length + new_segments * 128);
        auto in = static_cast<const unsigned char *>(source);
        const auto result_length = std::max(end, offset + length);
        try {
            while (length) {
                auto id = static_cast<uint32_t>(offset / segment_bytes_);
                const auto within = offset % segment_bytes_;
                const auto count =
                    static_cast<size_t>(std::min<uint64_t>(length, segment_bytes_ - within));
                const bool create = id > highest_;
                auto file = acquire(id, create);
                if (create) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    remember_locked(id);
                }
                highest_ = std::max(highest_, id);
                mark_dirty(id, file);
                file->write(header_bytes + within, in, count);
                offset += count;
                in += count;
                length -= count;
            }
            length_.store(result_length, std::memory_order_release);
        } catch (...) {
            poisoned_ = true;
            throw;
        }
    }
    void sync() {
        std::lock_guard<std::mutex> mutation(mutation_);
        require(!poisoned_, FQDB_RECOVERY_REQUIRED);
        try {
            for (auto &item : dirty_)
                item.second->sync();
            dirty_.clear();
            if (directory_dirty_) {
                // Every created segment is flushed before its directory entry.
                require(storage::sync_parent(path(0).c_str()), FQDB_COMMIT_UNKNOWN);
                directory_dirty_ = false;
            }
        } catch (...) {
            poisoned_ = true;
            throw;
        }
    }
    // Recovery-only mutation: the caller excludes readers of the removed tail.
    // Sparse growth across absent segments is intentionally unsupported here.
    void truncate(uint64_t length) {
        std::lock_guard<std::mutex> mutation(mutation_);
        require(!poisoned_, FQDB_RECOVERY_REQUIRED);
        require(length <= maximum_length(), FQDB_LIMIT);
        const auto before = size();
        if (length == before)
            return;
        const auto last = static_cast<uint32_t>(length ? (length - 1) / segment_bytes_ : 0);
        require(length <= before || last == highest_, FQDB_UNSUPPORTED);
        try {
            // Validate every candidate before the first destructive operation.
            for (const auto &entry :
                 std::filesystem::directory_iterator(std::filesystem::u8path(directory_)))
                if (auto id = identify(entry.path().filename().u8string()); id && *id > last)
                    (void)acquire(*id);
            auto tail = acquire(last);
            mark_dirty(last, tail);
            tail->truncate(header_bytes + length - uint64_t(last) * segment_bytes_);
            for (const auto &entry :
                 std::filesystem::directory_iterator(std::filesystem::u8path(directory_))) {
                auto id = identify(entry.path().filename().u8string());
                if (!id || *id <= last)
                    continue;
                std::error_code error;
                require(std::filesystem::remove(entry.path(), error) && !error, FQDB_IO_ERROR);
                dirty_.erase(*id);
                std::lock_guard<std::mutex> lock(mutex_);
                handles_.erase(*id);
                directory_dirty_ = true;
            }
            highest_ = last;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                auto current = ranges_->upper_bound(last);
                ranges_->erase(current, ranges_->end());
                if (!ranges_->empty())
                    ranges_->rbegin()->second = last;
            }
            length_.store(length, std::memory_order_release);
        } catch (...) {
            poisoned_ = true;
            throw;
        }
    }
    void retire(uint32_t id, const std::weak_ptr<void> &old_readers) {
        std::lock_guard<std::mutex> mutation(mutation_);
        require(!poisoned_, FQDB_RECOVERY_REQUIRED);
        require(id && id < highest_, FQDB_INVALID_ARGUMENT);
        require(old_readers.expired(), FQDB_LOCKED);
        std::lock_guard<std::mutex> lock(mutex_);
        retire_locked(id);
    }
    void prepare_sparse() {
        std::lock_guard<std::mutex> mutation(mutation_);
        // Called under the database writer lease before creating a reclaim root.
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(directory_)))
            if (auto id = identify(entry.path().filename().u8string()))
                acquire(*id)->prepare_sparse();
        sparse_ = true;
    }
    uint64_t allocated() const {
        std::lock_guard<std::mutex> mutation(mutation_);
        uint64_t total = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(directory_)))
            if (auto id = identify(entry.path().filename().u8string())) {
                auto file = const_cast<SegmentedFile *>(this)->acquire(*id);
                const auto bytes = file->allocated();
                require(bytes <= UINT64_MAX - total, FQDB_LIMIT);
                total += bytes;
            }
        return total;
    }
    std::optional<std::pair<uint64_t, uint64_t>> next_data(uint64_t begin, uint64_t end) {
        require(begin <= end && end <= size(), FQDB_LIMIT);
        while (begin < end) {
            uint32_t id = static_cast<uint32_t>(begin / segment_bytes_);
            std::shared_ptr<NativeFile> file;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                auto after = ranges_->upper_bound(id);
                if (after == ranges_->begin() || std::prev(after)->second < id) {
                    if (after == ranges_->end())
                        return {};
                    id = after->first;
                    begin = uint64_t(id) * segment_bytes_;
                    if (begin >= end)
                        return {};
                }
                file = acquire_locked(id, false);
            }
            const auto base = uint64_t(id) * segment_bytes_;
            const auto local_end =
                std::min({segment_bytes_, end - base, file->size() - header_bytes});
            if (auto range = file->next_data(header_bytes + begin - base, header_bytes + local_end))
                return std::make_pair(base + range->first - header_bytes,
                                      base + range->second - header_bytes);
            begin = base + segment_bytes_;
        }
        return {};
    }
    void punch(uint64_t offset, uint64_t length) {
        std::lock_guard<std::mutex> mutation(mutation_);
        require(offset >= reclaim_block_bytes && length && !(offset % reclaim_block_bytes) &&
                    !(length % reclaim_block_bytes) && offset <= size() &&
                    length <= size() - offset,
                FQDB_LIMIT);
        while (length) {
            const auto id = static_cast<uint32_t>(offset / segment_bytes_);
            const auto within = offset % segment_bytes_;
            const auto count = std::min(length, segment_bytes_ - within);
            if (id && id < highest_ && !within && count == segment_bytes_) {
                IOWritePermit permit(count);
                if (!storage::io_request(true, count))
                    throw IOBudgetStop();
#ifdef FQDB_TESTING
                if (fault_countdown(paged_fail_punch))
                    throw Error(FQDB_IO_ERROR);
#endif
                std::lock_guard<std::mutex> guard(mutex_);
                retire_locked(id);
            } else {
                auto file = acquire(id);
                file->punch(header_bytes + within, count);
                if (id && id < highest_ && !file->next_data(header_bytes, file->size())) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    retire_locked(id);
                }
            }
            offset += count;
            length -= count;
        }
    }
    void remove_files() {
        std::lock_guard<std::mutex> mutation(mutation_);
        if (auto cleanup = cleanup_plan()) {
            cleanup();
            return;
        }
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(directory_)))
            if (auto id = identify(entry.path().filename().u8string()))
                (void)acquire(*id);
        // Every owned header has been checked before deleting any file. Delete
        // the primary last so failure leaves a discoverable generation anchor.
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(directory_))) {
            auto id = identify(entry.path().filename().u8string());
            if (!id || !*id)
                continue;
            std::error_code error;
            require(std::filesystem::remove(entry.path(), error) && !error, FQDB_IO_ERROR);
        }
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(directory_)))
            if (auto id = identify(entry.path().filename().u8string()); id && *id)
                throw Error(FQDB_IO_ERROR);
        std::error_code error;
        require(std::filesystem::remove(std::filesystem::u8path(path(0)), error) && !error,
                FQDB_IO_ERROR);
    }
    std::function<void()> cleanup_plan() const {
        if (!created_here_)
            return {};
        return [ranges = ranges_, directory = directory_, generation = generation_] {
            auto filename = [&](uint32_t id) {
                auto name = id ? "s" + digits(generation, 16) + "_" + digits(id, 8) + ".fdb"
                               : "g" + digits(generation, 16) + ".fdb";
                return std::filesystem::u8path(directory) / name;
            };
            for (const auto &range : *ranges)
                for (uint64_t id = std::max<uint64_t>(1, range.first); id <= range.second; ++id) {
                    std::error_code error;
                    std::filesystem::remove(filename(static_cast<uint32_t>(id)), error);
                    if (error)
                        return; // Keep the primary if a child cannot be removed.
                }
            std::error_code error;
            std::filesystem::remove(filename(0), error);
        };
    }
    uint64_t reservation_bytes(uint64_t offset, uint64_t size) const {
        std::lock_guard<std::mutex> mutation(mutation_);
        require(offset <= maximum_length() && size <= maximum_length() - offset, FQDB_LIMIT);
        const auto last = size ? (offset + size - 1) / segment_bytes_ : 0;
        const auto count = last > highest_ ? last - highest_ : 0;
        require(count <= (UINT64_MAX - size) / 128, FQDB_LIMIT);
        return size + count * 128;
    }
};
} // namespace fengqudb::paged
