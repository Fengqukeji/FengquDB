#pragma once
#include "native_io.hpp"
#include <cstdlib>
#include <chrono>
#include "reserve_pool.hpp"
#include "../binary_diff.hpp"

namespace fengqudb::paged {
// Four separate cells per logical
// page: two confirmed copies stay untouched while two replacements are written.
class ArenaFile {
    static constexpr uint64_t prefix = 4096, cell = 4096, payload = 4032;
    using Body = std::array<unsigned char, payload>;
    struct Page {
        std::array<uint64_t, 4> sequences{};
        std::array<bool, 4> valid{true, true, true, true};
        uint64_t sequence = 0;
        uint32_t length = 0;
        std::shared_ptr<const Body> body;
    };
    static constexpr uint64_t maximum =
        std::min<uint64_t>((INT64_MAX - prefix) / (4 * cell), SIZE_MAX / sizeof(Page));
    struct Dirty {
        std::shared_ptr<Body> body;
        uint32_t length = 0;
    };
    struct Work {
        size_t page;
        std::array<unsigned, 2> slots;
        uint64_t sequence;
        Dirty image;
    };
    std::shared_ptr<NativeFile> file_;
    std::shared_ptr<const Body> empty_ = std::make_shared<const Body>();
    std::array<unsigned char, 16> uuid_{};
    uint64_t generation_ = 0, end_ = 0, physical_pages_ = 0;
    std::vector<Page> pages_;
    std::vector<Dirty> dirty_;
    std::vector<size_t> dirty_pages_;
    std::vector<Work> work_;
    mutable std::mutex mutex_;
    bool loaded_ = false, failed_ = false, recovering_ = false;
    uint64_t planned_begin_ = UINT64_MAX, planned_end_ = 0, recent_bytes_ = 0;
    uint64_t charged_ = 0, activity_bytes_ = 0, activity_flushes_ = 0;
    std::chrono::steady_clock::time_point activity_begin_{};
    static constexpr size_t body_cache_pages = 16;
    size_t released_pages_ = 0;
    void release_consumed() {
        const auto needed = std::max<uint64_t>(4, (end_ + payload - 1) / payload);
        const auto spare = physical_pages_ > needed ? physical_pages_ - needed : 0;
        if (charged_ > spare) {
            ReservePool::used.fetch_sub(charged_ - spare);
            charged_ = spare;
        }
    }
    uint64_t at(size_t page, unsigned slot) const {
        return prefix + (page * 4 + slot) * cell;
    }
    std::array<unsigned char, cell> encode(size_t page, uint64_t sequence, uint32_t length,
                                           const Body &body) const {
        std::array<unsigned char, cell> b{};
        std::memcpy(b.data(), "FQCELL01", 8);
        storage::put64(b.data() + 8, sequence);
        storage::put64(b.data() + 16, page);
        storage::put32(b.data() + 24, length);
        std::memcpy(b.data() + 32, uuid_.data(), uuid_.size());
        storage::put64(b.data() + 48, generation_);
        storage::put32(b.data() + 56, storage::crc32(body.data(), body.size()));
        storage::put32(b.data() + 60, storage::crc32(b.data(), 60));
        std::memcpy(b.data() + 64, body.data(), body.size());
        return b;
    }
    Page empty_page() const {
        Page p;
        p.body = empty_;
        return p;
    }
    std::shared_ptr<const Body> body(size_t page) const {
        const auto &p = pages_.at(page);
        if (p.body)
            return p.body;
        for (unsigned slot = 0; slot < 4; ++slot)
            if (p.valid[slot] && p.sequences[slot] == p.sequence) {
                std::array<unsigned char, cell> b{};
                file_->read(at(page, slot), b.data(), b.size());
                if (std::memcmp(b.data(), "FQCELL01", 8) ||
                    storage::get64(b.data() + 8) != p.sequence ||
                    storage::get64(b.data() + 16) != page ||
                    storage::get32(b.data() + 24) != p.length || storage::get32(b.data() + 28) ||
                    std::memcmp(b.data() + 32, uuid_.data(), uuid_.size()) ||
                    storage::get64(b.data() + 48) != generation_ ||
                    storage::get32(b.data() + 60) != storage::crc32(b.data(), 60) ||
                    storage::get32(b.data() + 56) != storage::crc32(b.data() + 64, payload))
                    continue;
                auto result = std::make_shared<Body>();
                std::memcpy(result->data(), b.data() + 64, payload);
                return result;
            }
        fail(FQDB_CORRUPT);
    }
    void release_old_bodies() {
        const auto live_pages = static_cast<size_t>((end_ + payload - 1) / payload);
        released_pages_ = std::min(released_pages_, live_pages);
        while (released_pages_ + body_cache_pages < live_pages)
            pages_[released_pages_++].body.reset();
    }
    void initialize(size_t page) {
        const auto b = encode(page, 0, 0, *empty_);
        for (unsigned s = 0; s < 4; ++s)
            file_->write(at(page, s), b.data(), b.size());
    }
    void clear_unreachable(size_t page) {
        // A retained spare page can already be fully initialized. Verify all
        // four exact empty images before reuse; any abandoned version, torn
        // initialization, wrong identity or damage still forces full clearing.
        const auto expected = encode(page, 0, 0, *empty_);
        std::array<unsigned char, cell * 4> current{};
        file_->read(at(page, 0), current.data(), current.size());
        for (unsigned slot = 0; slot < 4; ++slot)
            if (!detail::binary_equal(
                    {reinterpret_cast<const char *>(current.data() + slot * cell), cell},
                    {reinterpret_cast<const char *>(expected.data()), cell})) {
                initialize(page);
                return;
            }
    }
    void reserve(size_t count) {
        if (planned_begin_ != UINT64_MAX)
            count = std::max(count, static_cast<size_t>((planned_end_ + payload - 1) / payload));
        require(count <= maximum, FQDB_UNSUPPORTED);
        count = std::max<size_t>(count, 4);
        const auto old = physical_pages_;
        const bool fully_indexed = pages_.size() == old;
        auto wanted = old;
        if (count > old) {
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = activity_begin_ == std::chrono::steady_clock::time_point{}
                                         ? 0
                                         : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               now - activity_begin_)
                                               .count();
                const bool active =
                    (activity_flushes_ >= 32 && elapsed > 0 && elapsed < 1000000000) ||
                    end_ > 262144;
                if (active) {
                    const auto pending =
                        planned_begin_ == UINT64_MAX ? 0 : planned_end_ - planned_begin_;
                    const auto rate =
                        elapsed > 1000000 && elapsed < 1000000000
                            ? activity_bytes_ * 1000000000 / static_cast<uint64_t>(elapsed)
                            : 0;
                    const auto forecast = std::max(std::max(pending, recent_bytes_) * 8, rate / 10);
                    const auto headroom =
                        std::clamp<uint64_t>((forecast + payload - 1) / payload, 4, 128);
                    const auto growth =
                        std::min<uint64_t>(128, std::max<uint64_t>(old / 8, headroom));
                    wanted = std::min<uint64_t>(maximum, count + growth);
                } else
                    wanted = std::min<uint64_t>(maximum, (count + 3) / 4 * 4);
                const auto mandatory = std::max<uint64_t>(old, count);
                const auto speculative = wanted > mandatory ? wanted - mandatory : 0;
                const auto granted =
                    ReservePool::acquire(std::min(speculative, ReservePool::file_limit()));
                charged_ +=
                    granted; // held until consumption or destruction, including I/O failures
                wanted = mandatory + granted;
            }
        }
        for (auto page = old; page < wanted; ++page)
            initialize(page);
        physical_pages_ = std::max<uint64_t>(old, wanted);
        if (fully_indexed)
            while (pages_.size() < physical_pages_)
                pages_.push_back(empty_page());
        // An unindexed page after a recovered EOF may contain abandoned data.
        // Clear all copies before reuse so old high sequence numbers cannot revive it.
        while (pages_.size() < count) {
            const auto page = pages_.size();
            if (page < old)
                clear_unreachable(page);
            pages_.push_back(empty_page());
        }
    }
    Dirty &edit(size_t page) {
        if (dirty_.size() < pages_.size())
            dirty_.resize(pages_.size());
        auto &dirty = dirty_.at(page);
        if (dirty.body)
            return dirty;
        const auto &p = pages_.at(page);
        auto body = std::make_shared<Body>(*this->body(page));
        dirty_pages_.push_back(page);
        dirty = {std::move(body), p.length};
        return dirty;
    }
    void extend(uint64_t size) {
        reserve(static_cast<size_t>((size + payload - 1) / payload));
        for (auto pos = end_; pos < size;) {
            const auto page = static_cast<size_t>(pos / payload),
                       within = static_cast<size_t>(pos % payload);
            const auto count =
                static_cast<size_t>(std::min<uint64_t>(payload - within, size - pos));
            auto &d = edit(page);
            std::memset(d.body->data() + within, 0, count);
            d.length = static_cast<uint32_t>(within + count);
            pos += count;
        }
        end_ = size;
        release_consumed();
    }

  public:
    explicit ArenaFile(std::shared_ptr<NativeFile> file) : file_(std::move(file)) {
        ReservePool::live.fetch_add(1);
    }
    ~ArenaFile() {
        if (charged_)
            ReservePool::used.fetch_sub(charged_);
        ReservePool::live.fetch_sub(1);
    }
    void prepare(uint64_t offset, uint64_t length) {
        std::lock_guard<std::mutex> lock(mutex_);
        require(offset <= maximum * payload && length <= maximum * payload - offset,
                FQDB_UNSUPPORTED);
        planned_begin_ = std::min(planned_begin_, offset);
        planned_end_ = std::max(planned_end_, offset + length);
    }
    void create(std::array<unsigned char, 16> uuid, uint64_t generation) {
        require(!storage::io_quota, FQDB_UNSUPPORTED);
        require(file_->exclusively_created() && file_->size() == 0, FQDB_INVALID_ARGUMENT);
        uuid_ = uuid;
        generation_ = generation;
        std::array<unsigned char, prefix> header{};
        for (unsigned offset : {0u, 64u}) {
            auto p = header.data() + offset;
            std::memcpy(p, "FQAHYB01", 8);
            storage::put32(p + 8, 1);
            storage::put32(p + 12, payload);
            std::memcpy(p + 16, uuid_.data(), uuid_.size());
            storage::put64(p + 32, generation_);
            storage::put32(p + 40, 4);
            storage::put32(p + 44, cell);
            storage::put32(p + 60, storage::crc32(p, 60));
        }
        file_->write(0, header.data(), header.size());
        reserve(1);
        file_->sync();
        loaded_ = true;
    }
    void load() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_)
            return;
        std::array<unsigned char, 64> h{};
        file_->read(64, h.data(), h.size());
        require(!std::memcmp(h.data(), "FQAHYB01", 8) && storage::get32(h.data() + 8) == 1 &&
                storage::get32(h.data() + 12) == payload && storage::get32(h.data() + 40) == 4 &&
                storage::get32(h.data() + 44) == cell &&
                storage::get32(h.data() + 60) == storage::crc32(h.data(), 60));
        std::memcpy(uuid_.data(), h.data() + 16, uuid_.size());
        generation_ = storage::get64(h.data() + 32);
        const auto physical = file_->size();
        require(physical >= prefix + 4 * cell);
        physical_pages_ = (physical - prefix) / (4 * cell);
        require(physical_pages_ <= maximum, FQDB_UNSUPPORTED);
        for (size_t page = 0; page < physical_pages_; ++page) {
            Page p = empty_page();
            bool selected = false;
            unsigned invalid = 0;
            for (unsigned slot = 0; slot < 4; ++slot) {
                std::array<unsigned char, cell> b{};
                file_->read(at(page, slot), b.data(), b.size());
                const bool valid =
                    !std::memcmp(b.data(), "FQCELL01", 8) &&
                    storage::get32(b.data() + 60) == storage::crc32(b.data(), 60) &&
                    storage::get32(b.data() + 56) == storage::crc32(b.data() + 64, payload);
                p.valid[slot] = valid;
                if (!valid) {
                    ++invalid;
                    continue;
                }
                require(storage::get64(b.data() + 16) == page &&
                        !std::memcmp(b.data() + 32, uuid_.data(), uuid_.size()) &&
                        storage::get64(b.data() + 48) == generation_ &&
                        storage::get32(b.data() + 28) == 0);
                const auto seq = storage::get64(b.data() + 8);
                const auto len = storage::get32(b.data() + 24);
                require(len <= payload);
                p.sequences[slot] = seq;
                if (!selected || seq > p.sequence) {
                    p.sequence = seq;
                    p.length = len;
                    auto body = std::make_shared<Body>();
                    std::memcpy(body->data(), b.data() + 64, payload);
                    p.body = std::move(body);
                    selected = true;
                } else if (seq == p.sequence)
                    require(len == p.length &&
                            !std::memcmp(p.body->data(), b.data() + 64, payload));
            }
            require(selected &&
                    invalid <= 1); // ambiguous double damage must not roll back a confirmed page
            end_ = page * payload + p.length;
            const bool last = p.length < payload;
            pages_.push_back(std::move(p));
            if (last)
                break;
            release_old_bodies();
        }
        loaded_ = true;
        recovering_ = true;
    }
    void identity(const std::array<unsigned char, 16> &uuid, uint64_t generation) const {
        require(loaded_ && uuid == uuid_ && generation == generation_);
    }
    uint64_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return end_;
    }
    void read(uint64_t offset, void *output, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        require(loaded_ && offset <= end_ && size <= end_ - offset);
        auto target = static_cast<unsigned char *>(output);
        while (size) {
            const auto page = static_cast<size_t>(offset / payload),
                       within = static_cast<size_t>(offset % payload);
            const auto count = std::min(size, static_cast<size_t>(payload) - within);
            auto bytes = page < dirty_.size() && dirty_[page].body
                             ? std::shared_ptr<const Body>(dirty_[page].body)
                             : body(page);
            std::memcpy(target, bytes->data() + within, count);
            target += count;
            offset += count;
            size -= count;
        }
    }
    void write(uint64_t offset, const void *input, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        require(loaded_ && !failed_, FQDB_COMMIT_UNKNOWN);
        require(!storage::io_quota, FQDB_UNSUPPORTED);
        require(offset <= end_ && offset <= maximum * payload && size <= maximum * payload - offset,
                FQDB_UNSUPPORTED);
        require(offset >= end_ || ((offset == 4096 || offset == 8192) && size == 64),
                FQDB_UNSUPPORTED);
        if (!size)
            return;
        try {
            reserve(static_cast<size_t>((offset + size + payload - 1) / payload));
            auto source = static_cast<const unsigned char *>(input);
            while (size) {
                const auto page = static_cast<size_t>(offset / payload),
                           within = static_cast<size_t>(offset % payload);
                const auto count = std::min(size, static_cast<size_t>(payload) - within);
                auto &d = edit(page);
                std::memcpy(d.body->data() + within, source, count);
                d.length = std::max(d.length, static_cast<uint32_t>(within + count));
                source += count;
                offset += count;
                size -= count;
            }
            end_ = std::max(end_, offset);
            release_consumed();
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    void truncate(uint64_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        require(loaded_ && !failed_, FQDB_COMMIT_UNKNOWN);
        require(!storage::io_quota && size <= maximum * payload && (size >= end_ || recovering_),
                FQDB_UNSUPPORTED);
        try {
            if (size >= end_) {
                extend(size);
                return;
            }
            const auto page = static_cast<size_t>(size / payload);
            const auto within = static_cast<uint32_t>(size % payload);
            if (within) {
                auto &d = edit(page);
                d.length = within;
                std::memset(d.body->data() + within, 0, payload - within);
            }
            for (auto n = page + (within ? 1 : 0); n < pages_.size(); ++n) {
                auto &d = edit(n);
                std::fill(d.body->begin(), d.body->end(), 0);
                d.length = 0;
            }
            end_ = size;
            release_consumed();
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    void sync() {
        auto &work = work_;
        work.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            require(loaded_ && !failed_, FQDB_COMMIT_UNKNOWN);
            require(!storage::io_quota, FQDB_UNSUPPORTED);
            std::sort(dirty_pages_.begin(), dirty_pages_.end());
            work.reserve(dirty_pages_.size());
            for (const auto page : dirty_pages_) {
                const auto &p = pages_[page];
                const auto &d = dirty_[page];
                require(p.sequence != UINT64_MAX, FQDB_LIMIT);
                std::array<unsigned, 4> order{0, 1, 2, 3};
                std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
                    if (p.valid[a] != p.valid[b])
                        return !p.valid[a];
                    return p.sequences[a] != p.sequences[b] ? p.sequences[a] < p.sequences[b]
                                                            : a < b;
                });
                work.push_back({page, {order[0], order[1]}, p.sequence + 1, d});
            }
            if (work.empty())
                return;
        }
        try {
            for (const auto &w : work) {
                const auto b = encode(w.page, w.sequence, w.image.length, *w.image.body);
                for (auto slot : w.slots)
                    file_->write(at(w.page, slot), b.data(), b.size());
            }
            file_->data_sync(); // one barrier for both payload and self-contained boundary metadata
            std::lock_guard<std::mutex> lock(mutex_);
            const auto live_pages = (end_ + payload - 1) / payload;
            for (const auto &w : work) {
                auto &p = pages_[w.page];
                p.sequence = w.sequence;
                p.length = w.image.length;
                p.body = w.page >= live_pages || w.page + body_cache_pages < live_pages
                             ? std::shared_ptr<const Body>{}
                             : w.image.body;
                for (auto slot : w.slots) {
                    p.sequences[slot] = w.sequence;
                    p.valid[slot] = true;
                }
            }
            if (planned_begin_ != UINT64_MAX) {
                const auto added = planned_end_ - planned_begin_;
                recent_bytes_ = std::max(added, recent_bytes_ * 15 / 16);
                const auto now = std::chrono::steady_clock::now();
                if (activity_begin_ == std::chrono::steady_clock::time_point{} ||
                    now - activity_begin_ > std::chrono::seconds(1)) {
                    activity_begin_ = now;
                    activity_bytes_ = 0;
                    activity_flushes_ = 0;
                }
                activity_bytes_ += added;
                ++activity_flushes_;
            }
            release_consumed();
            planned_begin_ = UINT64_MAX;
            planned_end_ = 0;
            for (const auto page : dirty_pages_)
                dirty_[page] = {};
            dirty_pages_.clear();
            work.clear();
            recovering_ = false;
            release_old_bodies();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_ = true;
            throw;
        }
    }
    void release_cache() {
        std::lock_guard<std::mutex> lock(mutex_);
        pages_.clear();
        dirty_.clear();
        dirty_pages_.clear();
        work_.clear();
        std::vector<Page>().swap(pages_);
        std::vector<Dirty>().swap(dirty_);
        std::vector<size_t>().swap(dirty_pages_);
        std::vector<Work>().swap(work_);
        if (charged_) {
            ReservePool::used.fetch_sub(charged_);
            charged_ = 0;
        }
    }
#ifdef FQDB_TESTING
    size_t cached_bodies() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto &p : pages_)
            count += bool(p.body && p.body != empty_);
        return count;
    }
#endif
};
} // namespace fengqudb::paged
