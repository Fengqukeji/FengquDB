#pragma once
#include "native_io.hpp"
#include <map>
#include <shared_mutex>

namespace fengqudb::paged {
// Logical append cache for direct/segmented generations. Hybrid generations
// already own equivalent pending bytes. The Store writer serializes mutations;
// readers may continue using immutable cached bytes during a physical flush.
class DeferredIO {
    struct Block {
        std::shared_ptr<const void> owner;
        const unsigned char *data;
        size_t size;
    };
    mutable std::shared_mutex mutex_;
    std::map<uint64_t, Block> blocks_;
    std::atomic<bool> enabled_{false};
    uint64_t end_ = 0, bytes_ = 0, maximum_ = 0;

  public:
    template <class Size> void enable(uint64_t maximum, Size &&size) {
        if (enabled_.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::shared_mutex> lock(mutex_);
        end_ = size();
        maximum_ = maximum;
        enabled_.store(true, std::memory_order_release);
    }
    bool enabled() const {
        return enabled_.load(std::memory_order_acquire);
    }
    uint64_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return end_;
    }
    void write(uint64_t offset, const void *data, size_t size,
               std::shared_ptr<const void> owner = {}) {
        if (!size)
            return;
        std::lock_guard<std::shared_mutex> lock(mutex_);
        require(offset <= INT64_MAX && size <= uint64_t(INT64_MAX) - offset, FQDB_LIMIT);
        auto it = blocks_.lower_bound(offset);
        uint64_t previous = 0;
        if (it != blocks_.end() && it->first == offset) {
            require(it->second.size == size, FQDB_INTERNAL_ERROR);
            previous = size;
        } else {
            require(it == blocks_.end() || offset + size <= it->first, FQDB_INTERNAL_ERROR);
            if (it != blocks_.begin()) {
                auto before = std::prev(it);
                require(before->first + before->second.size <= offset, FQDB_INTERNAL_ERROR);
            }
        }
        require(size <= maximum_ && bytes_ - previous <= maximum_ - size, FQDB_LIMIT);
        if (!owner) {
            auto copy = std::make_shared<std::string>(static_cast<const char *>(data), size);
            data = copy->data();
            owner = std::move(copy);
        }
        blocks_.insert_or_assign(
            offset, Block{std::move(owner), static_cast<const unsigned char *>(data), size});
        bytes_ = bytes_ - previous + size;
        end_ = std::max(end_, offset + size);
    }
    template <class Read> void read(uint64_t offset, void *data, size_t size, Read &&raw) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        require(offset <= end_ && size <= end_ - offset);
        auto target = static_cast<unsigned char *>(data);
        while (size) {
            auto next = blocks_.upper_bound(offset);
            if (next != blocks_.begin()) {
                auto previous = std::prev(next);
                const auto within = offset - previous->first;
                if (within < previous->second.size) {
                    auto n = std::min(size, previous->second.size - size_t(within));
                    std::memcpy(target, previous->second.data + within, n);
                    offset += n;
                    target += n;
                    size -= n;
                    continue;
                }
            }
            auto n = next == blocks_.end() ? size
                                           : size_t(std::min<uint64_t>(size, next->first - offset));
            raw(offset, target, n);
            offset += n;
            target += n;
            size -= n;
        }
    }
    template <class Write, class Sync> void sync(Write &&write, Sync &&sync) {
        // blocks_ cannot mutate while the enclosing Store writer is held.
        // Do not hold its mutex across disk I/O and stall ordinary readers.
        for (const auto &entry : blocks_)
            write(entry.first, entry.second.data, entry.second.size);
        sync();
        std::lock_guard<std::shared_mutex> lock(mutex_);
        blocks_.clear();
        bytes_ = 0;
    }
    void truncated(uint64_t end) {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        require(blocks_.empty(), FQDB_INTERNAL_ERROR);
        end_ = end;
    }
};
} // namespace fengqudb::paged
