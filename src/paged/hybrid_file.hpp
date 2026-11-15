#pragma once
#include "cell_file.hpp"
#include <shared_mutex>

namespace fengqudb::paged {
#ifdef FQDB_TESTING
inline std::atomic<int> hybrid_publish_fault{0};
#endif
inline void hybrid_publication_at(int point) {
#ifdef FQDB_TESTING
    if (hybrid_publish_fault.compare_exchange_strong(point, 0))
        throw Error(FQDB_IO_ERROR);
#else
    (void)point;
#endif
}
// Each sync is one complete checksummed packet in
// exactly one lane. A global packet sequence orders the two durable streams.
class HybridFile {
    static constexpr uint64_t maximum = INT64_MAX, packet_limit = UINT32_MAX;
    static constexpr uint64_t bootstrap = 12288;
    struct Segment {
        uint64_t offset = 0, size = 0, position = 0;
        unsigned lane = 0;
    }; // small, large, pending, zero
    struct Op {
        std::array<unsigned char, 32> meta{};
        uint64_t offset = 0, size = 0;
        size_t segment = SIZE_MAX;
        std::vector<unsigned char> owned;
        std::shared_ptr<const void> owner;
        const void *borrowed = nullptr;
        const void *data() const {
            return owner ? borrowed : owned.data();
        }
    };
    struct Packet {
        uint64_t sequence = 0, end = 0, payload_at = 0;
        uint32_t length = 0;
    };
    std::shared_ptr<NativeFile> root_;
    ArenaFile small_;
    NativeFile large_;
    std::shared_ptr<NativeFile> materialized_, image_;
    std::string image_path_;
    uint64_t image_at_ = bootstrap;
    uint64_t image_synced_at_ = 0;
    std::mutex materialize_mutex_;
    std::string path_;
    std::string companion_;
    std::array<unsigned char, bootstrap> header_{};
    std::vector<Segment> segments_;
    uint64_t end_ = 0;
    std::vector<Op> pending_;
    uint64_t pending_bytes_ = 0, sequence_ = 0, large_tail_ = 128, threshold_ = 32768;
    std::atomic<bool> loaded_{false};
    bool failed_ = false, companion_owned_ = false, planned_large_ = false, read_only_ = false;
    mutable std::shared_mutex mutex_;
    void writable() const {
        require(loaded_ && !failed_, FQDB_COMMIT_UNKNOWN);
        require(!read_only_, FQDB_UNSUPPORTED);
    }
    static std::array<unsigned char, 32> metadata(uint32_t kind, uint64_t offset, uint64_t size,
                                                  uint32_t crc) {
        std::array<unsigned char, 32> m{};
        storage::put32(m.data(), kind);
        storage::put64(m.data() + 8, offset);
        storage::put64(m.data() + 16, size);
        storage::put32(m.data() + 24, crc);
        storage::put32(m.data() + 28, storage::crc32(m.data(), 28));
        return m;
    }
    void resize_view(uint64_t size) {
        require(size <= maximum);
        if (size < end_) {
            // A later append may reuse a truncated tail while conversion is
            // paused. Recopy that range instead of publishing stale image bytes.
            image_at_ = std::min(image_at_, std::max(size, bootstrap));
            image_synced_at_ = std::min(image_synced_at_, image_at_);
            while (!segments_.empty() && segments_.back().offset >= size)
                segments_.pop_back();
            if (!segments_.empty() && segments_.back().offset + segments_.back().size > size)
                segments_.back().size = size - segments_.back().offset;
            if (size < bootstrap)
                std::fill(header_.begin() + static_cast<size_t>(size), header_.end(), 0);
        } else if (size > end_ && size > bootstrap) {
            const auto begin = std::max(end_, bootstrap);
            segments_.push_back({begin, size - begin, 0, 3});
        }
        end_ = size;
    }
    template <class File>
    static std::optional<Packet> next(File &file, uint64_t &position, uint64_t limit) {
        require(position <= limit);
        if (limit - position < 32)
            return {};
        std::array<unsigned char, 32> h{};
        file.read(position, h.data(), h.size());
        require(!std::memcmp(h.data(), "FQHYPK01", 8) && storage::get32(h.data() + 28) == 0 &&
                storage::get32(h.data() + 24) == storage::crc32(h.data(), 24));
        const auto length = storage::get32(h.data() + 16), crc = storage::get32(h.data() + 20);
        require(length && length <= packet_limit && storage::get64(h.data() + 8));
        if (static_cast<uint64_t>(length) + 56 > limit - position)
            return {};
        Packet p;
        p.sequence = storage::get64(h.data() + 8);
        p.end = position + length + 56;
        p.payload_at = position + 32;
        p.length = length;
        std::array<unsigned char, 24> t{};
        file.read(p.payload_at + length, t.data(), t.size());
        require(!std::memcmp(t.data(), storage::end_magic, 8) &&
                storage::get64(t.data() + 8) == p.sequence &&
                storage::get32(t.data() + 16) == crc &&
                storage::get32(t.data() + 20) == storage::crc32(t.data(), 20));
        std::array<unsigned char, 65536> buffer;
        uint32_t actual = 0;
        for (uint64_t at = 0; at < length;) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), length - at));
            file.read(p.payload_at + at, buffer.data(), count);
            actual = storage::crc32(buffer.data(), count, actual);
            at += count;
        }
        require(actual == crc);
        position = p.end;
        return p;
    }
    template <class File> void replay(File &file, const Packet &packet, unsigned lane) {
        require(sequence_ != UINT64_MAX && packet.sequence == sequence_ + 1);
        uint64_t at = 0;
        std::array<unsigned char, 65536> buffer;
        while (at < packet.length) {
            require(packet.length - at >= 32);
            std::array<unsigned char, 32> meta{};
            file.read(packet.payload_at + at, meta.data(), meta.size());
            const auto m = meta.data();
            require(storage::get32(m + 4) == 0 && storage::get32(m + 28) == storage::crc32(m, 28));
            const auto kind = storage::get32(m), crc = storage::get32(m + 24);
            const auto offset = storage::get64(m + 8), length = storage::get64(m + 16);
            at += 32;
            if (kind == 2) {
                require(!length && !crc && offset <= maximum);
                resize_view(offset);
            } else {
                require(kind == 1 && length && length <= packet.length - at);
                require(offset <= end_ && offset <= maximum && length <= maximum - offset);
                uint32_t actual = 0;
                for (uint64_t n = 0; n < length;) {
                    const auto count =
                        static_cast<size_t>(std::min<uint64_t>(buffer.size(), length - n));
                    file.read(packet.payload_at + at + n, buffer.data(), count);
                    actual = storage::crc32(buffer.data(), count, actual);
                    n += count;
                }
                require(actual == crc);
                if (offset < bootstrap) {
                    require(length <= bootstrap - offset);
                    file.read(packet.payload_at + at, header_.data() + offset,
                              static_cast<size_t>(length));
                    end_ = std::max(end_, offset + length);
                } else {
                    require(offset == end_);
                    segments_.push_back({offset, length, packet.payload_at + at, lane});
                    end_ += length;
                }
                at += length;
            }
        }
        require(at == packet.length);
        sequence_ = packet.sequence;
    }
    void stage(uint64_t offset, const void *data, size_t size, uint32_t crc,
               std::shared_ptr<const void> owner = {}) {
        require(loaded_ && !failed_, FQDB_COMMIT_UNKNOWN);
        require(offset <= end_ && offset <= maximum && size <= maximum - offset, FQDB_UNSUPPORTED);
        if (!size)
            return;
        Op op;
        op.offset = offset;
        op.size = size;
        op.meta = metadata(1, offset, size, crc);
        if (offset < bootstrap)
            require(size <= bootstrap - offset, FQDB_UNSUPPORTED);
        else
            require(offset == end_, FQDB_UNSUPPORTED);
        if (owner) {
            op.owner = std::move(owner);
            op.borrowed = data;
        } else
            op.owned.assign(static_cast<const unsigned char *>(data),
                            static_cast<const unsigned char *>(data) + size);
        op.segment = offset >= bootstrap ? segments_.size() : SIZE_MAX;
        pending_.push_back(std::move(op));
        try {
            if (offset < bootstrap) {
                std::memcpy(header_.data() + offset, data, size);
                end_ = std::max<uint64_t>(end_, offset + size);
            } else {
                segments_.push_back({offset, size, pending_.size() - 1, 2});
                end_ += size;
            }
            pending_bytes_ += size;
        } catch (...) {
            failed_ = true;
            throw;
        }
    }

  public:
    explicit HybridFile(std::shared_ptr<NativeFile> root, const std::string &path,
                        bool read_only = false)
        : root_(std::move(root)), small_(root_), path_(path), read_only_(read_only) {
        auto file = std::filesystem::u8path(path);
        file.replace_extension(".blk");
        companion_ = file.u8string();
    }
    ~HybridFile() {
        if (image_ && !image_path_.empty())
            remove_owned(image_path_);
    }
    void create(std::array<unsigned char, 16> uuid, uint64_t generation) {
        small_.create(uuid, generation);
        try {
            large_.open(companion_, true);
            companion_owned_ = large_.exclusively_created();
        } catch (...) {
            companion_owned_ = large_.exclusively_created();
            throw;
        }
        std::array<unsigned char, 128> h{};
        for (unsigned pos : {0u, 64u}) {
            auto p = h.data() + pos;
            std::memcpy(p, "FQHYLG01", 8);
            storage::put32(p + 8, 1);
            std::memcpy(p + 16, uuid.data(), uuid.size());
            storage::put64(p + 32, generation);
            storage::put32(p + 60, storage::crc32(p, 60));
        }
        large_.write(0, h.data(), h.size());
        large_.sync();
        loaded_ = true;
    }
    void load() {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        if (loaded_)
            return;
        small_.load();
        std::array<unsigned char, 64> owner{}, h{};
        root_->read(64, owner.data(), owner.size());
        large_.open(companion_, false, true, read_only_);
        require(large_.size() >= 128);
        large_.read(64, h.data(), h.size());
        require(!std::memcmp(h.data(), "FQHYLG01", 8) && storage::get32(h.data() + 8) == 1 &&
                storage::get32(h.data() + 60) == storage::crc32(h.data(), 60) &&
                !std::memcmp(h.data() + 16, owner.data() + 16, 16) &&
                storage::get64(h.data() + 32) == storage::get64(owner.data() + 32));
        companion_owned_ = true;
        uint64_t a = 0, b = 128;
        auto left = next(small_, a, small_.size());
        auto right = next(large_, b, large_.size());
        while (left || right) {
            if (left && (!right || left->sequence < right->sequence)) {
                replay(small_, *left, 0);
                left = next(small_, a, small_.size());
            } else {
                replay(large_, *right, 1);
                right = next(large_, b, large_.size());
            }
        }
        // Validate both streams and their global order before touching either.
        if (a < small_.size())
            small_.truncate(a);
        large_tail_ = b;
        loaded_ = true;
    }
    void identity(const std::array<unsigned char, 16> &uuid, uint64_t generation) const {
        small_.identity(uuid, generation);
    }
    uint64_t size() const {
        if (!loaded_.load(std::memory_order_acquire))
            const_cast<HybridFile *>(this)->load();
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return materialized_ ? materialized_->size() : end_;
    }
    void prepare(uint64_t offset, uint64_t size) {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        writable();
        require(offset <= maximum && size <= maximum - offset, FQDB_UNSUPPORTED);
        planned_large_ = size >= threshold_ || pending_bytes_ >= threshold_;
    }
    bool checksums() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return !materialized_ && planned_large_;
    }
    bool accelerated() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return !materialized_;
    }
    void read(uint64_t offset, void *data, size_t size) {
        if (!loaded_.load(std::memory_order_acquire))
            load();
        std::shared_lock<std::shared_mutex> lock(mutex_);
        read_locked(offset, data, size);
    }

  private:
    void read_locked(uint64_t offset, void *data, size_t size) {
        if (materialized_) {
            materialized_->read(offset, data, size);
            return;
        }
        require(loaded_ && offset <= end_ && size <= end_ - offset);
        auto target = static_cast<unsigned char *>(data);
        if (offset < bootstrap && size) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(size, bootstrap - offset));
            std::memcpy(target, header_.data() + offset, count);
            offset += count;
            target += count;
            size -= count;
        }
        if (!size)
            return;
        auto it = std::upper_bound(segments_.begin(), segments_.end(), offset,
                                   [](uint64_t at, const Segment &s) { return at < s.offset; });
        require(it != segments_.begin());
        --it;
        while (size) {
            require(it != segments_.end() && offset >= it->offset &&
                    offset - it->offset < it->size);
            const auto within = offset - it->offset;
            const auto count = static_cast<size_t>(std::min<uint64_t>(size, it->size - within));
            if (it->lane == 0)
                small_.read(it->position + within, target, count);
            else if (it->lane == 1)
                large_.read(it->position + within, target, count);
            else if (it->lane == 3)
                std::memset(target, 0, count);
            else {
                require(it->position < pending_.size());
                const auto &op = pending_[static_cast<size_t>(it->position)];
                require(within <= op.size && count <= op.size - within);
                std::memcpy(target, static_cast<const unsigned char *>(op.data()) + within, count);
            }
            target += count;
            offset += count;
            size -= count;
            ++it;
        }
    }

  public:
    void write(uint64_t offset, const void *data, size_t size) {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        writable();
        if (materialized_) {
            materialized_->write(offset, data, size);
            return;
        }
        stage(offset, data, size, storage::crc32(data, size));
    }
    void write_known(uint64_t offset, const void *data, size_t size, uint32_t crc,
                     std::shared_ptr<const void> owner = {}) {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        writable();
        if (materialized_) {
            materialized_->write(offset, data, size);
            return;
        }
        stage(offset, data, size, crc, std::move(owner));
    }
    void truncate(uint64_t size) {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        writable();
        if (materialized_) {
            materialized_->truncate(size);
            return;
        }
        require(size <= maximum, FQDB_UNSUPPORTED);
        try {
            Op op;
            op.meta = metadata(2, size, 0, 0);
            pending_.push_back(std::move(op));
            resize_view(size);
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    bool overwritten(uint64_t offset, size_t size) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return !materialized_ &&
               ((offset >= bootstrap && offset < end_) ||
                (offset < bootstrap &&
                 !(size == 64 && (offset == 0 || offset == 4096 || offset == 8192))));
    }
    void sync() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        writable();
        if (materialized_) {
            materialized_->sync();
            return;
        }
        if (pending_.empty())
            return;
        require(sequence_ != UINT64_MAX, FQDB_LIMIT);
        uint64_t length = 0;
        uint32_t crc = 0;
        for (const auto &op : pending_) {
            length += 32 + op.size;
            require(length <= packet_limit, FQDB_LIMIT);
            crc = storage::crc32(op.meta.data(), op.meta.size(), crc);
            if (op.size)
                crc = storage::crc32_combine(crc, storage::get32(op.meta.data() + 24), op.size);
        }
        storage::RecordFrame frame("FQHYPK01", sequence_ + 1, static_cast<size_t>(length), crc);
        const bool big = pending_bytes_ >= threshold_;
        uint64_t position = big ? large_tail_ : small_.size();
        const auto packet_start = position;
        try {
            if (!big)
                small_.prepare(position, length + 56);
            // Store's writer gate serializes mutations. Pending bytes and their
            // owners remain stable until the barrier completes. Readers may
            // access the prior view while disk I/O is in flight.
            lock.unlock();
            std::array<unsigned char, 16384> buffer{};
            size_t used = 0;
            auto write = [&](const void *p, size_t n) {
                if (big)
                    large_.write(position, p, n);
                else
                    small_.write(position, p, n);
                position += n;
            };
            auto flush = [&] {
                if (used) {
                    write(buffer.data(), used);
                    used = 0;
                }
            };
            auto append = [&](const void *p, size_t n) {
                auto input = static_cast<const unsigned char *>(p);
                while (n) {
                    if (!used && n >= buffer.size()) {
                        write(input, n);
                        return;
                    }
                    const auto count = std::min(n, buffer.size() - used);
                    std::memcpy(buffer.data() + used, input, count);
                    used += count;
                    input += count;
                    n -= count;
                    if (used == buffer.size())
                        flush();
                }
            };
            append(frame.header.data(), frame.header.size());
            for (const auto &op : pending_) {
                append(op.meta.data(), op.meta.size());
                if (op.size)
                    append(op.data(), static_cast<size_t>(op.size));
            }
            append(frame.trailer.data(), frame.trailer.size());
            flush();
            if (big) {
                large_.truncate(position);
                large_.data_sync();
                large_tail_ = position;
            } else {
                small_.sync();
            }
            lock.lock();
            uint64_t at = packet_start + 32;
            for (size_t i = 0; i < pending_.size(); ++i) {
                const auto &op = pending_[i];
                at += 32;
                if (op.segment < segments_.size()) {
                    auto &s = segments_[op.segment];
                    if (s.lane == 2 && s.position == i) {
                        s.lane = big ? 1u : 0u;
                        s.position = at;
                    }
                }
                at += op.size;
            }
            ++sequence_;
            pending_.clear();
            pending_bytes_ = 0;
            planned_large_ = false;
        } catch (...) {
            if (!lock.owns_lock())
                lock.lock();
            failed_ = true;
            throw;
        }
    }
    // Metered writes and hole reclamation use the same Store records in a
    // direct image. Publish a byte-identical image before changing the backend;
    // existing snapshots keep their offsets and observe identical bytes.
    bool materialize(uint64_t maximum_copy = UINT64_MAX, uint64_t maximum_dirty = UINT64_MAX) {
        std::lock_guard<std::mutex> conversion(materialize_mutex_);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        writable();
        if (materialized_)
            return true;
        bool published = false;
        try {
            // Keep bounded jobs' established 4 KiB progress quantum. Unmetered
            // conversion amortizes destination writes over larger blocks.
            std::vector<unsigned char> buffer(storage::io_quota ? 4096 : 262144);
            const auto began = std::chrono::steady_clock::now();
            const auto first = image_at_;
            while (image_at_ < end_) {
                const auto count =
                    static_cast<size_t>(std::min<uint64_t>(buffer.size(), end_ - image_at_));
                IOWritePermit permit(count); // Refuse before allocating or touching the source.
                if (!image_) {
                    {
                        // Recoverable identity, with the same short filename.
                        // The source generation is verified before an orphan
                        // bearing this token is reclaimed on the next open.
                        const auto number = storage::get64(header_.data() + 16) ^
                                            storage::get64(header_.data() + 32);
                        std::string name = "x0000000000000000.tmp";
                        for (unsigned i = 0; i < 16; ++i)
                            name[16 - i] = "0123456789abcdef"[(number >> (i * 4)) & 15];
                        image_path_ =
                            (std::filesystem::u8path(path_).parent_path() / name).u8string();
                        auto candidate = std::make_shared<NativeFile>();
                        try {
                            candidate->open(image_path_, true);
                            candidate->lock();
                            image_ = std::move(candidate);
                        } catch (const Error &) {
                            if (candidate->exclusively_created())
                                remove_owned(image_path_);
                            throw;
                        }
                    }
                    require(bool(image_), FQDB_ALREADY_EXISTS);
                }
                read_locked(image_at_, buffer.data(), count);
                image_->write(image_at_, buffer.data(), count);
                image_at_ += count;
                if (maximum_copy != UINT64_MAX &&
                    (image_at_ - first >= maximum_copy ||
                     std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(2)))
                    break;
            }
            if (image_at_ < end_ || image_at_ - image_synced_at_ > maximum_dirty)
                return false;
            // Only these three 64-byte records can be nonzero in the bootstrap.
            // Copy them last, under the writer gate, so incremental jobs may
            // share a source with intervening ordinary appends/checkpoints.
            IOWritePermit header_permit(192);
            require(bool(image_), FQDB_INTERNAL_ERROR);
            for (uint64_t at : {UINT64_C(0), UINT64_C(4096), UINT64_C(8192)})
                image_->write(at, header_.data() + at, 64);
            image_->truncate(end_);
            image_->sync();
#ifdef _WIN32
            auto from = storage::native_path(image_path_.c_str()),
                 to = storage::native_path(path_.c_str());
            // Windows may reject replacing a byte-range-leased data file even
            // with FILE_SHARE_DELETE. Store's directory lease and this exclusive
            // lock protect the transition while both data handles are closed.
            root_->retire();
            image_->retire();
            if (!MoveFileExW(from.c_str(), to.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                failed_ = true;
                // Preserve reads when the rename failed before changing the
                // target. A complete direct image is equally safe to read.
                try {
                    root_->open(path_, false, true);
                    root_->lock();
                    std::array<unsigned char, 8> magic{};
                    root_->read(0, magic.data(), magic.size());
                    if (!std::memcmp(magic.data(), "FQDATA04", 8))
                        materialized_ = root_;
                } catch (...) {
                }
                throw Error(FQDB_COMMIT_UNKNOWN);
            }
#else
            require(::rename(image_path_.c_str(), path_.c_str()) == 0, FQDB_IO_ERROR);
#endif
            published = true;
            hybrid_publication_at(1);
            if (!storage::sync_parent(path_.c_str())) {
                failed_ = true;
                throw Error(FQDB_COMMIT_UNKNOWN);
            }
#ifdef _WIN32
            try {
                root_->open(path_, false, true);
                root_->lock();
            } catch (...) {
                failed_ = true;
                throw Error(FQDB_COMMIT_UNKNOWN);
            }
            materialized_ = root_;
            image_.reset();
#else
            materialized_ = std::move(image_);
#endif
            hybrid_publication_at(2);
            image_path_.clear();
            std::vector<Op>().swap(pending_);
            std::vector<Segment>().swap(segments_);
            pending_bytes_ = 0;
            small_.release_cache();
#ifndef _WIN32
            root_->retire();
#endif
            large_.retire();
            if (companion_owned_) {
                remove_owned(companion_);
                companion_owned_ = false;
            }
            return true;
        } catch (...) {
            if (published) {
                failed_ = true;
                throw Error(FQDB_COMMIT_UNKNOWN);
            }
            throw;
        }
    }
    // The maintenance lease keeps this conversion job alive. Serialize only
    // converters, leaving ordinary source appends and reads available during I/O.
    void sync_materialization() {
        std::lock_guard<std::mutex> conversion(materialize_mutex_);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (materialized_ || !image_ ||
            (image_at_ < end_ && image_at_ - image_synced_at_ < (8u << 20)))
            return;
        const auto at = image_at_;
        auto image = image_;
        lock.unlock();
        image->data_sync();
        lock.lock();
        image_synced_at_ = std::min(at, image_at_);
    }
    bool prepare_sparse_slice() {
        if (!materialize(2u << 20, 1u << 20))
            return false;
        materialized_->prepare_sparse();
        return true;
    }
    void prepare_sparse() {
        materialize();
        materialized_->prepare_sparse();
    }
    std::optional<std::pair<uint64_t, uint64_t>> next_data(uint64_t begin, uint64_t end) {
        materialize();
        return materialized_->next_data(begin, end);
    }
    void punch(uint64_t offset, uint64_t length) {
        materialize();
        materialized_->punch(offset, length);
    }
    uint64_t allocated() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return materialized_ ? materialized_->allocated() : root_->allocated() + large_.allocated();
    }
    const std::string &companion() const {
        return companion_;
    }
    bool owns_companion() const {
        return companion_owned_;
    }
    void remove_companion() noexcept {
        if (companion_owned_)
            remove_owned(companion_);
    }
};
} // namespace fengqudb::paged
