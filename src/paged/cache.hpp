#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fengqudb::paged {
// Shards bound contention as well as retained memory. Borrowers own immutable
// objects independently: eviction drops only the cache's reference, never data.
template <class T> class Cache {
    struct Entry {
        std::shared_ptr<const T> value;
        uint64_t cost;
        uint64_t tag;
        uint64_t key;
        bool read = true;
        Entry *previous = nullptr, *next = nullptr;
    };
    struct Shard {
        mutable std::mutex mutex;
        std::unordered_map<uint64_t, Entry> entries;
        Entry *first = nullptr, *last = nullptr;
        uint64_t bytes = 0, hits = 0, misses = 0, evictions = 0;
    };
    std::array<Shard, 16> shards_;
    std::atomic<uint64_t> budget_;
    std::atomic<uint64_t> retained_{0};
    bool reserve(uint64_t cost) noexcept {
        auto current = retained_.load(std::memory_order_relaxed);
        const auto budget = budget_.load(std::memory_order_relaxed);
        while (current <= budget && cost <= budget - current) {
            if (retained_.compare_exchange_weak(current, current + cost, std::memory_order_relaxed))
                return true;
        }
        return false;
    }
    static void detach(Shard &s, Entry &entry) noexcept {
        if (entry.previous)
            entry.previous->next = entry.next;
        else
            s.first = entry.next;
        if (entry.next)
            entry.next->previous = entry.previous;
        else
            s.last = entry.previous;
    }
    static void prepend(Shard &s, Entry &entry) noexcept {
        entry.previous = nullptr;
        entry.next = s.first;
        if (s.first)
            s.first->previous = &entry;
        else
            s.last = &entry;
        s.first = &entry;
    }
    void evict(Shard &s) noexcept {
        auto victim = s.last;
        const auto key = victim->key, cost = victim->cost;
        detach(s, *victim);
        s.bytes -= cost;
        s.entries.erase(key);
        ++s.evictions;
        retained_.fetch_sub(cost, std::memory_order_relaxed);
        compact(s);
    }
    static void compact(Shard &s) noexcept {
        // A mostly emptied map must not keep an old large bucket allocation
        // after its entries have stopped being charged to the cache budget.
        if (s.entries.bucket_count() > s.entries.size() * 16 + 64) {
            try {
                s.entries.rehash(std::max<size_t>(1, s.entries.size() * 2));
            } catch (...) {
            }
        }
    }
    void insert(Shard &s, uint64_t key, std::shared_ptr<const T> value, uint64_t cost, uint64_t tag,
                bool read) {
        // The caller already reserved global capacity. Allocation failure must
        // return it if map insertion cannot finish. Standard unordered_map node
        // references survive rehash; link only after successful allocation.
        try {
            auto result = s.entries.emplace(key, Entry{std::move(value), cost, tag, key, read});
            if (!result.second) {
                retained_.fetch_sub(cost, std::memory_order_relaxed);
                return;
            }
            prepend(s, result.first->second);
        } catch (...) {
            retained_.fetch_sub(cost, std::memory_order_relaxed);
            throw;
        }
        s.bytes += cost;
    }
    bool put_large(uint64_t key, std::shared_ptr<const T> value, uint64_t cost, uint64_t tag,
                   bool read) {
        // Large-object admission is uncommon. Lock shards in a fixed order so a
        // hot 16 MiB value may use spare global budget without adding a lock or
        // an atomic operation to any cache hit.
        std::array<std::unique_lock<std::mutex>, 16> locks;
        for (size_t i = 0; i < shards_.size(); ++i)
            locks[i] = std::unique_lock<std::mutex>(shards_[i].mutex);
        const auto budget = budget_.load(std::memory_order_relaxed);
        if (cost > budget)
            return false;
        auto &target = shard(key);
        if (target.entries.find(key) != target.entries.end())
            return true;
        while (retained_.load(std::memory_order_relaxed) > budget - cost) {
            for (auto &s : shards_) {
                if (s.last)
                    evict(s);
                if (retained_.load(std::memory_order_relaxed) <= budget - cost)
                    break;
            }
        }
        retained_.fetch_add(cost, std::memory_order_relaxed);
        insert(target, key, std::move(value), cost, tag, read);
        return true;
    }
    Shard &shard(uint64_t key) {
        key ^= key >> 30;
        key *= UINT64_C(0xbf58476d1ce4e5b9);
        key ^= key >> 27;
        key *= UINT64_C(0x94d049bb133111eb);
        key ^= key >> 31;
        return shards_[key % shards_.size()];
    }

  public:
    struct Info {
        uint64_t bytes = 0, entries = 0, hits = 0, misses = 0, evictions = 0;
    };
    explicit Cache(uint64_t budget) : budget_(budget) {}
    bool admits(uint64_t allocation) const noexcept {
        return allocation <= UINT64_MAX - sizeof(Entry) - 96 &&
               allocation + sizeof(Entry) + 96 <= budget_.load(std::memory_order_relaxed);
    }
    // Generation retirement drops retained aliases and disables new admission.
    // Existing borrowers remain valid. The normal hit path stays unchanged.
    void configure(uint64_t budget) {
        std::array<std::unique_lock<std::mutex>, 16> locks;
        for (size_t i = 0; i < shards_.size(); ++i)
            locks[i] = std::unique_lock<std::mutex>(shards_[i].mutex);
        for (auto &s : shards_) {
            s.evictions += s.entries.size();
            s.entries.clear();
            s.first = s.last = nullptr;
            s.bytes = 0;
            try {
                s.entries.rehash(1);
            } catch (...) {
            }
        }
        retained_.store(0, std::memory_order_relaxed);
        budget_.store(budget, std::memory_order_relaxed);
    }
    std::shared_ptr<const T> get(uint64_t key, uint64_t *tag = nullptr) {
        auto &s = shard(key);
        std::lock_guard<std::mutex> lock(s.mutex);
        auto found = s.entries.find(key);
        if (found == s.entries.end()) {
            ++s.misses;
            return {};
        }
        ++s.hits;
        if (tag)
            *tag = found->second.tag;
        auto &entry = found->second;
        entry.read = true;
        if (s.first != &entry) {
            detach(s, entry);
            prepend(s, entry);
        }
        return found->second.value;
    }
    bool recently_read(uint64_t key) {
        auto &s = shard(key);
        std::lock_guard<std::mutex> lock(s.mutex);
        auto found = s.entries.find(key);
        return found != s.entries.end() && found->second.read;
    }
    bool put(uint64_t key, std::shared_ptr<const T> value, uint64_t allocation_bytes,
             uint64_t tag = 0, bool read = true) {
        // One map node owns the LRU links; include conservative bucket/node and
        // allocation bookkeeping without a separately allocated list node.
        if (allocation_bytes > UINT64_MAX - sizeof(Entry) - 96)
            return false;
        const uint64_t cost = allocation_bytes + sizeof(Entry) + 96;
        const auto budget = budget_.load(std::memory_order_relaxed);
        if (cost > budget)
            return false;
        const auto allowance = budget / shards_.size();
        if (cost > allowance) {
            return put_large(key, std::move(value), cost, tag, read);
        }
        auto &s = shard(key);
        std::lock_guard<std::mutex> lock(s.mutex);
        // configure() can run after the initial admission calculation.
        const auto current_allowance = budget_.load(std::memory_order_relaxed) / shards_.size();
        if (cost > current_allowance)
            return false;
        if (s.entries.find(key) != s.entries.end())
            return true;
        // Preserve an admitted large object in this shard. Small misses must
        // not immediately evict it merely because of the per-shard allowance.
        if (s.bytes > current_allowance)
            return false;
        while (s.bytes + cost > current_allowance)
            evict(s);
        while (!reserve(cost)) {
            if (!s.last)
                return false;
            evict(s);
        }
        insert(s, key, std::move(value), cost, tag, read);
        return true;
    }
    // A replacement has been admitted. Remove an unborrowed obsolete version;
    // snapshots can still reconstruct it from the immutable WAL if needed.
    void retire(uint64_t key) {
        auto &s = shard(key);
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.entries.find(key);
        if (it == s.entries.end() || it->second.value.use_count() != 1)
            return;
        auto cost = it->second.cost;
        detach(s, it->second);
        s.entries.erase(it);
        s.bytes -= cost;
        ++s.evictions;
        retained_.fetch_sub(cost, std::memory_order_relaxed);
        compact(s);
    }
    Info info() const {
        Info result;
        for (auto &s : shards_) {
            std::lock_guard<std::mutex> lock(s.mutex);
            result.entries += s.entries.size();
            result.hits += s.hits;
            result.misses += s.misses;
            result.evictions += s.evictions;
        }
        result.bytes = retained_.load(std::memory_order_relaxed);
        return result;
    }
};
} // namespace fengqudb::paged
