#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fengqudb::paged {
// Open-addressing key -> node map for the cache shards. Nodes keep stable
// addresses for the LRU links; probing compares keys inside the contiguous slot
// array, so a hit touches one slot line and its node instead of a bucket, the
// preceding list node and the node. Load factor <= 1/2, backward-shift erase.
template <class Entry> class EntryIndex {
    struct Slot {
        uint64_t key;
        Entry *entry;
    };
    std::vector<Slot> slots_;
    size_t size_ = 0;
    static size_t hash(uint64_t k) noexcept {
        k ^= k >> 33;
        k *= UINT64_C(0xff51afd7ed558ccd);
        k ^= k >> 33;
        return static_cast<size_t>(k);
    }
    size_t mask() const noexcept {
        return slots_.size() - 1;
    }
    void place(Slot s) noexcept {
        for (size_t i = hash(s.key) & mask();; i = (i + 1) & mask())
            if (!slots_[i].entry) {
                slots_[i] = s;
                return;
            }
    }
    // Allocates before changing anything, so a failure leaves the index intact.
    void rebuild(size_t capacity) {
        std::vector<Slot> old(capacity, Slot{0, nullptr});
        old.swap(slots_);
        for (const auto &s : old)
            if (s.entry)
                place(s);
    }

  public:
    EntryIndex() = default;
    EntryIndex(const EntryIndex &) = delete;
    EntryIndex &operator=(const EntryIndex &) = delete;
    ~EntryIndex() {
        clear();
    }
    size_t size() const noexcept {
        return size_;
    }
    size_t capacity() const noexcept {
        return slots_.size();
    }
    Entry *find(uint64_t key) const noexcept {
        if (!size_)
            return nullptr;
        for (size_t i = hash(key) & mask();; i = (i + 1) & mask()) {
            const auto &s = slots_[i];
            if (!s.entry)
                return nullptr;
            if (s.key == key)
                return s.entry;
        }
    }
    // Returns {existing, false} for a duplicate key; throws before any change.
    std::pair<Entry *, bool> emplace(uint64_t key, Entry value) {
        if (auto existing = find(key))
            return {existing, false};
        auto node = std::make_unique<Entry>(std::move(value));
        if ((size_ + 1) * 2 > slots_.size())
            rebuild(std::max<size_t>(16, slots_.size() * 2));
        place({key, node.get()});
        ++size_;
        return {node.release(), true};
    }
    void erase(uint64_t key) noexcept {
        if (!size_)
            return;
        size_t i = hash(key) & mask();
        for (;; i = (i + 1) & mask()) {
            if (!slots_[i].entry)
                return;
            if (slots_[i].key == key)
                break;
        }
        delete slots_[i].entry;
        slots_[i] = {0, nullptr};
        --size_;
        // Close the hole: move a later entry back when the hole lies between
        // its home slot and its current slot.
        for (size_t j = (i + 1) & mask(); slots_[j].entry; j = (j + 1) & mask()) {
            const size_t home = hash(slots_[j].key) & mask();
            if (((j - home) & mask()) >= ((j - i) & mask())) {
                slots_[i] = slots_[j];
                slots_[j] = {0, nullptr};
                i = j;
            }
        }
    }
    void clear() noexcept {
        for (auto &s : slots_)
            delete s.entry;
        std::vector<Slot>().swap(slots_);
        size_ = 0;
    }
    // Shrink a mostly emptied table; allocation failure keeps the old one.
    void compact() noexcept {
        if (slots_.size() <= size_ * 16 + 64)
            return;
        size_t capacity = 16;
        while (capacity < size_ * 4)
            capacity *= 2;
        try {
            rebuild(capacity);
        } catch (...) {
        }
    }
};
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
        EntryIndex<Entry> entries;
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
        // A mostly emptied index must not keep an old large slot allocation
        // after its entries have stopped being charged to the cache budget.
        s.entries.compact();
    }
    void insert(Shard &s, uint64_t key, std::shared_ptr<const T> value, uint64_t cost, uint64_t tag,
                bool read) {
        // The caller already reserved global capacity. Allocation failure must
        // return it if index insertion cannot finish. Index nodes keep stable
        // addresses across growth; link only after successful allocation.
        try {
            auto result = s.entries.emplace(key, Entry{std::move(value), cost, tag, key, read});
            if (!result.second) {
                retained_.fetch_sub(cost, std::memory_order_relaxed);
                return;
            }
            prepend(s, *result.first);
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
        if (target.entries.find(key))
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
        }
        retained_.store(0, std::memory_order_relaxed);
        budget_.store(budget, std::memory_order_relaxed);
    }
    std::shared_ptr<const T> get(uint64_t key, uint64_t *tag = nullptr) {
        auto &s = shard(key);
        std::lock_guard<std::mutex> lock(s.mutex);
        auto found = s.entries.find(key);
        if (!found) {
            ++s.misses;
            return {};
        }
        ++s.hits;
        if (tag)
            *tag = found->tag;
        auto &entry = *found;
        entry.read = true;
        if (s.first != &entry) {
            detach(s, entry);
            prepend(s, entry);
        }
        return entry.value;
    }
    bool recently_read(uint64_t key) {
        auto &s = shard(key);
        std::lock_guard<std::mutex> lock(s.mutex);
        auto found = s.entries.find(key);
        return found && found->read;
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
        if (s.entries.find(key))
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
        if (!it || it->value.use_count() != 1)
            return;
        auto cost = it->cost;
        detach(s, *it);
        s.entries.erase(key);
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
