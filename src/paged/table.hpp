#pragma once
#include "generation.hpp"
#include <array>
#include <unordered_map>

namespace fengqudb::paged {
// Internal slots store each child's minimum key and maximum modifying version.
// Physical children always precede their parent; this bounds traversal even for
// corrupt input and makes immutable roots safe to share across checkpoints.
class Table {
  public:
    // A batch owns this bounded, unsynchronized lookup path. It never survives
    // the request or changes the shared cache. Arbitrary key order is valid.
    class ReadPath {
        struct Entry {
            std::shared_ptr<const Page> page;
            uint64_t offset = 0, ceiling = 0, cost = 0;
            size_t position = SIZE_MAX;
        };
        std::shared_ptr<Generation> owner_;
        std::array<Entry, 3> entries_{};
        std::shared_ptr<const Page> transient_;
        uint64_t budget_, retained_ = 0;

      public:
        explicit ReadPath(uint64_t budget) noexcept : budget_(budget) {}
        uint64_t retained_bytes() const noexcept {
            return retained_;
        }
        size_t lower(const Page &page, std::string_view key, unsigned depth) {
            // A position is only a hint: verify its ordering bracket, otherwise
            // use the full lower_bound. Duplicates and arbitrary order are valid.
            if (depth >= entries_.size() || entries_[depth].page.get() != &page)
                return page.lower(key);
            auto &hint = entries_[depth].position;
            const auto count = page.slots.size();
            if (hint <= count) {
                if (hint < count) {
                    const auto order = compare(page.slots[hint].key, key);
                    if (!order)
                        return hint;
                    if (order < 0 &&
                        (hint + 1 == count || compare(page.slots[hint + 1].key, key) >= 0))
                        return ++hint;
                    if (order > 0 && (!hint || compare(page.slots[hint - 1].key, key) < 0))
                        return hint;
                } else if (!count || compare(page.slots.back().key, key) < 0)
                    return hint;
            }
            return hint = page.lower(key);
        }

        // Consume the returned page before the next get(). A cache miss may
        // replace its depth slot or the ordinary one-page transient owner.
        const Page *get(const std::shared_ptr<Generation> &generation, uint64_t offset,
                        uint64_t ceiling, unsigned depth) {
            transient_.reset();
            if (owner_ != generation) {
                for (auto &entry : entries_)
                    entry = {};
                retained_ = 0;
                owner_ = generation;
            }
            if (depth < entries_.size()) {
                auto &entry = entries_[depth];
                if (entry.page && entry.offset == offset && entry.ceiling == ceiling)
                    return entry.page.get();
                retained_ -= entry.cost;
                entry = {};
                auto page = generation->page(offset, ceiling);
                const auto cost = page->allocation();
                if (cost <= budget_ - retained_) {
                    entry = {std::move(page), offset, ceiling, cost};
                    retained_ += cost;
                    return entry.page.get();
                }
                transient_ = std::move(page);
            } else
                transient_ = generation->page(offset, ceiling);
            return transient_.get();
        }
    };

  private:
    std::shared_ptr<Generation> generation_;
    uint64_t root_, limit_;
    template <class Load>
    static std::optional<Ref> find_with(uint64_t root, uint64_t limit, std::string_view key,
                                        Load &&load, ReadPath *path = nullptr) {
        auto offset = root, ceiling = limit;
        for (unsigned depth = 0; offset; ++depth) {
            require(depth < 64 && offset < ceiling);
            auto page = load(offset, ceiling, depth);
            auto at = path ? path->lower(*page, key, depth) : page->lower(key);
            if (page->leaf) {
                if (at < page->slots.size() && compare(page->slots[at].key, key) == 0)
                    return page->slots[at].value;
                return {};
            }
            if (at == page->slots.size() || compare(page->slots[at].key, key) > 0) {
                if (!at)
                    return {};
                --at;
            }
            ceiling = offset;
            offset = page->slots[at].child;
        }
        return {};
    }

  public:
    Table(std::shared_ptr<Generation> generation, uint64_t root, uint64_t limit)
        : generation_(std::move(generation)), root_(root), limit_(limit) {}
    std::optional<Ref> find(std::string_view key) const {
        return find_with(root_, limit_, key, [&](uint64_t offset, uint64_t ceiling, unsigned) {
            return generation_->page(offset, ceiling);
        });
    }
    std::optional<Ref> find(std::string_view key, ReadPath &path) const {
        return find_with(
            root_, limit_, key,
            [&](uint64_t offset, uint64_t ceiling, unsigned depth) {
                return path.get(generation_, offset, ceiling, depth);
            },
            &path);
    }
    static std::optional<Ref> lookup(const std::shared_ptr<Generation> &generation, uint64_t root,
                                     uint64_t limit, std::string_view key,
                                     ReadPath *path = nullptr) {
        // The caller's View owns the generation for this entire lookup. Keep
        // page ownership locally; no borrowed pointer escapes the call.
        std::shared_ptr<const Page> owner;
        return find_with(
            root, limit, key,
            [&](uint64_t offset, uint64_t ceiling, unsigned depth) {
                if (path)
                    return path->get(generation, offset, ceiling, depth);
                owner = generation->page(offset, ceiling);
                return owner.get();
            },
            path);
    }
    class Cursor {
        struct Level {
            std::shared_ptr<const Page> page;
            size_t at;
            uint64_t offset;
        };
        std::shared_ptr<Generation> generation_;
        std::vector<Level> stack_;
        bool reverse_;
        uint64_t since_;
        void descend(uint64_t offset, uint64_t ceiling, std::optional<std::string_view> begin) {
            while (offset) {
                require(stack_.size() < 64 && offset < ceiling);
                auto page = generation_->page(offset, ceiling);
                if (page->maximum <= since_)
                    return;
                size_t at = reverse_ ? page->slots.size() - 1 : 0;
                if (begin) {
                    at = page->lower(*begin);
                    if (!page->leaf || reverse_) {
                        if (at == page->slots.size() || compare(page->slots[at].key, *begin) > 0) {
                            if (at)
                                --at;
                            else if (page->leaf) {
                                stack_.push_back({page, page->slots.size(), offset});
                                return;
                            }
                        }
                    }
                }
                stack_.push_back({page, at, offset});
                if (page->leaf)
                    return;
                if (page->slots[at].maximum <= since_)
                    return;
                ceiling = offset;
                offset = page->slots[at].child;
            }
        }

      public:
        Cursor(std::shared_ptr<Generation> generation, uint64_t root, uint64_t limit,
               std::optional<std::string_view> begin = {}, bool reverse = false, uint64_t since = 0)
            : generation_(std::move(generation)), reverse_(reverse), since_(since) {
            stack_.reserve(64);
            descend(root, limit, begin);
        }
        // Returned key views remain valid until the following next() call.
        const Slot *next() {
            while (!stack_.empty()) {
                auto &top = stack_.back();
                if (top.page->leaf && top.at < top.page->slots.size()) {
                    auto at = top.at;
                    top.at = reverse_ ? (at ? at - 1 : top.page->slots.size()) : at + 1;
                    if (top.page->slots[at].value.version > since_)
                        return &top.page->slots[at];
                    continue;
                }
                if (top.page->leaf) {
                    stack_.pop_back();
                    continue;
                }
                bool more = reverse_ ? top.at != 0 : top.at + 1 < top.page->slots.size();
                if (!more) {
                    stack_.pop_back();
                    continue;
                }
                top.at = reverse_ ? top.at - 1 : top.at + 1;
                if (top.page->slots[top.at].maximum <= since_)
                    continue;
                auto child = top.page->slots[top.at].child, ceiling = top.offset;
                descend(child, ceiling, {});
            }
            return nullptr;
        }
    };
    Cursor cursor(std::optional<std::string_view> begin = {}, bool reverse = false,
                  uint64_t since = 0) const {
        return Cursor(generation_, root_, limit_, begin, reverse, since);
    }
    uint64_t range_max(std::string_view begin, std::string_view end) const {
        std::function<uint64_t(uint64_t, uint64_t, std::optional<std::string_view>, unsigned)>
            visit;
        visit = [&](uint64_t offset, uint64_t ceiling, std::optional<std::string_view> upper,
                    unsigned depth) {
            if (!offset)
                return uint64_t{0};
            require(depth < 64 && offset < ceiling);
            auto page = generation_->page(offset, ceiling);
            if (!end.empty() && compare(page->slots.front().key, end) >= 0)
                return uint64_t{0};
            if ((begin.empty() || compare(begin, page->slots.front().key) <= 0) &&
                (end.empty() || (upper && compare(end, *upper) >= 0)))
                return page->maximum;
            uint64_t result = 0;
            if (page->leaf) {
                for (size_t i = page->lower(begin); i < page->slots.size(); ++i) {
                    const auto &slot = page->slots[i];
                    if (!end.empty() && compare(slot.key, end) >= 0)
                        break;
                    result = std::max(result, slot.value.version);
                }
            } else {
                for (size_t i = 0; i < page->slots.size(); ++i) {
                    const auto &slot = page->slots[i];
                    auto high = i + 1 < page->slots.size()
                                    ? std::optional<std::string_view>(page->slots[i + 1].key)
                                    : upper;
                    if (high && !begin.empty() && compare(*high, begin) <= 0)
                        continue;
                    if (!end.empty() && compare(slot.key, end) >= 0)
                        break;
                    result = std::max(result, visit(slot.child, offset, high, depth + 1));
                }
            }
            return result;
        };
        if (!end.empty() && compare(begin, end) >= 0)
            return 0;
        return visit(root_, limit_, {}, 0);
    }
};

// A bounded private edit. Each touched cached page is cloned once, and later
// edits replace/crop that complete binary. Only dirty pages are written, once,
// children before parents. On any exception discard the entire builder.
class TableBuilder {
    static constexpr uint64_t dirty_bit = UINT64_C(1) << 63;
    struct Dirty {
        Page page;
        uint64_t persisted = 0;
        explicit Dirty(Page p) : page(std::move(p)) {}
    };
    std::shared_ptr<Generation> generation_;
    uint64_t root_, limit_, version_, budget_, retained_ = 0;
    size_t target_;
    bool failed_ = false, finished_ = false;
    std::vector<std::unique_ptr<Dirty>> dirty_;
    std::unordered_map<uint64_t, uint64_t> copied_;
    static bool dirty(uint64_t offset) noexcept {
        return (offset & dirty_bit) != 0;
    }
    Dirty &entry(uint64_t offset) {
        require(dirty(offset) && (offset & ~dirty_bit) < dirty_.size(), FQDB_INTERNAL_ERROR);
        return *dirty_[static_cast<size_t>(offset & ~dirty_bit)];
    }
    void account() const {
        require(retained_ <= budget_, FQDB_LIMIT);
    }
    uint64_t add(Page page) {
        retained_ += page.allocation() + sizeof(Dirty) + 128;
        account();
        dirty_.push_back(std::make_unique<Dirty>(std::move(page)));
        return dirty_bit | (dirty_.size() - 1);
    }
    uint64_t own(uint64_t offset) {
        if (dirty(offset))
            return offset;
        auto found = copied_.find(offset);
        if (found != copied_.end())
            return found->second;
        auto cached = generation_->page(offset, limit_);
        require(cached->maximum <= version_);
        require(cached->allocation() + sizeof(Dirty) + 128 <= budget_ - retained_, FQDB_LIMIT);
        auto id = add(Page(*cached));
        copied_.emplace(offset, id);
        return id;
    }
    void set(Page &page, size_t at, Slot slot, bool insert) {
        auto before = page.allocation();
        page.set(at, slot, insert);
        retained_ = retained_ - before + page.allocation();
        account();
    }
    Slot link(uint64_t offset) {
        auto &page = entry(offset).page;
        Slot result;
        result.key = page.slots.front().key;
        result.child = offset;
        result.maximum = page.maximum;
        result.earliest = page.earliest;
        return result;
    }
    std::pair<uint64_t, uint64_t> edit(uint64_t offset, std::string_view key, Ref value,
                                       unsigned depth) {
        require(depth < 64);
        offset = own(offset);
        auto &page = entry(offset).page;
        auto at = page.lower(key);
        if (page.leaf) {
            Slot slot;
            slot.key = key;
            slot.value = value;
            set(page, at, slot, at == page.slots.size() || compare(page.slots[at].key, key) != 0);
        } else {
            if (at == page.slots.size() || compare(page.slots[at].key, key) > 0) {
                if (at)
                    --at;
            }
            require(page.slots[at].child != offset);
            if (!dirty(page.slots[at].child))
                require(page.frame_offset && page.slots[at].child < page.frame_offset);
            auto children = edit(page.slots[at].child, key, value, depth + 1);
            set(page, at, link(children.first), false);
            if (children.second)
                set(page, at + 1, link(children.second), true);
        }
        const auto minimum = page.leaf ? size_t{2} : size_t{4};
        if ((page.binary.size() <= target_ || page.slots.size() < minimum) &&
            page.binary.size() <= max_page_bytes)
            return {offset, 0};
        require(page.slots.size() >= 2, FQDB_LIMIT);
        size_t cut = 1;
        while (cut + 1 < page.slots.size() && page.slots[cut].start < page.binary.size() / 2)
            ++cut;
        if (!page.leaf && page.slots.size() >= 4)
            cut = std::clamp(cut, size_t{2}, page.slots.size() - 2);
        auto before = page.allocation();
        auto right = page.split(cut);
        retained_ = retained_ - before + page.allocation();
        return {offset, add(std::move(right))};
    }
    uint64_t persist(uint64_t offset, unsigned depth) {
        if (!dirty(offset))
            return offset;
        require(depth < 64);
        auto &item = entry(offset);
        if (item.persisted)
            return item.persisted;
        if (!item.page.leaf) {
            for (size_t i = 0; i < item.page.slots.size(); ++i) {
                auto slot = item.page.slots[i];
                slot.child = persist(slot.child, depth + 1);
                set(item.page, i, slot, false);
            }
        }
        item.persisted = generation_->write_page(item.page, version_);
        return item.persisted;
    }

  public:
    TableBuilder(std::shared_ptr<Generation> generation, uint64_t root, uint64_t limit,
                 uint64_t version, uint64_t budget = 16 * 1024 * 1024, size_t target = 16384)
        : generation_(std::move(generation)), root_(root), limit_(limit), version_(version),
          budget_(budget), target_(target) {
        require(target >= 256 && target <= max_page_bytes && version, FQDB_INVALID_ARGUMENT);
    }
    void set(std::string_view key, Ref value) {
        require(!failed_ && !finished_, FQDB_INTERNAL_ERROR);
        require(key.size() <= max_internal_key_bytes && value.valid() && value.version <= version_,
                FQDB_INVALID_ARGUMENT);
        try {
            if (!root_)
                root_ = add(Page{});
            auto split = edit(root_, key, value, 0);
            root_ = split.first;
            if (split.second) {
                Page root(false);
                root.set(0, link(root_), true);
                root.set(1, link(split.second), true);
                root_ = add(std::move(root));
            }
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    uint64_t finish() {
        require(!failed_ && !finished_, FQDB_INTERNAL_ERROR);
        try {
            auto result = persist(root_, 0);
            finished_ = true;
            return result;
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
    uint64_t retained() const noexcept {
        return retained_;
    }
    size_t dirty_pages() const noexcept {
        return dirty_.size();
    }
};
} // namespace fengqudb::paged
