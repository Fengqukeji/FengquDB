#pragma once
#include "table.hpp"
#include "budget.hpp"
#include "wal.hpp"

namespace fengqudb::paged {
struct HistoryChain {
    struct Segment {
        std::shared_ptr<Generation> generation;
        uint64_t limit, version, floor, start = data_start;
    };
    Charge charge;
    std::vector<Segment> segments; // Newest first, excluding the active generation.
    explicit HistoryChain(std::shared_ptr<Budget> budget)
        : charge(std::move(budget), {}, sizeof(HistoryChain) + 128) {}
};
struct View {
    std::shared_ptr<Generation> generation;
    mutable std::shared_ptr<ReadEpoch> reader_epoch;
    Checkpoint checkpoint;
    std::shared_ptr<const HistoryChain> history;
    DeltaRoot delta;
    uint64_t version = 0, durable = 0, limit = data_start;
    uint64_t entries = 0, keys = 0, live_bytes = 0;
    std::optional<Ref> find(std::string_view key) const {
        if (auto node = delta_find(delta, key))
            return node->value;
        return Table::lookup(generation, checkpoint.root, limit, key);
    }
    std::optional<Ref> find(std::string_view key, Table::ReadPath &path) const {
        if (auto node = delta_find(delta, key))
            return node->value;
        return Table::lookup(generation, checkpoint.root, limit, key, &path);
    }
    uint64_t range_max(std::string_view begin, std::string_view end) const {
        return std::max(delta_range_max(delta, begin, end),
                        Table(generation, checkpoint.root, limit).range_max(begin, end));
    }
};
inline uint64_t history_floor(const View &view) noexcept {
    return view.checkpoint.parent ? view.checkpoint.retained_floor : view.checkpoint.floor;
}
inline std::vector<uint64_t> history_keep(const View &view) {
    std::vector<uint64_t> numbers;
    if (view.history)
        for (const auto &segment : view.history->segments)
            numbers.push_back(segment.generation->number);
    return numbers;
}
struct Metadata {
    std::string_view key;
    Ref value;
};
class ViewCursor {
    Table::Cursor disk_;
    DeltaCursor memory_;
    const Slot *slot_ = nullptr;
    const DeltaNode *node_ = nullptr;
    bool advance_disk_ = true, advance_memory_ = true, reverse_;

  public:
    ViewCursor(const View &view, std::optional<std::string_view> begin = {}, bool reverse = false,
               uint64_t since = 0)
        : disk_(view.generation, view.checkpoint.root, view.limit, begin, reverse, since),
          memory_(view.delta, begin, reverse, since), reverse_(reverse) {}
    std::optional<Metadata> next() {
        if (advance_disk_)
            slot_ = disk_.next();
        if (advance_memory_)
            node_ = memory_.next();
        advance_disk_ = advance_memory_ = false;
        if (!slot_ && !node_)
            return {};
        auto order = !slot_ ? 1 : (!node_ ? -1 : compare(slot_->key, node_->key.view()));
        if (!node_ || (slot_ && (reverse_ ? order > 0 : order < 0))) {
            advance_disk_ = true;
            return Metadata{slot_->key, slot_->value};
        }
        advance_memory_ = true;
        advance_disk_ = slot_ && !order;
        return Metadata{node_->key.view(), node_->value};
    }
};
template <class Next>
inline uint64_t fold_metadata(std::shared_ptr<Generation> generation, uint64_t root,
                              uint64_t version, uint64_t budget, Next &&next) {
    auto builder =
        std::make_unique<TableBuilder>(generation, root, generation->tail, version, budget);
    while (auto row = next()) {
        // Leave headroom for the next search path and a possible page split.
        if (builder->retained() > budget / 3) {
            root = builder->finish();
            builder =
                std::make_unique<TableBuilder>(generation, root, generation->tail, version, budget);
        }
        builder->set(row->key, row->value);
    }
    return builder->finish();
}
inline uint64_t fold_delta(std::shared_ptr<Generation> generation, uint64_t root, DeltaRoot delta,
                           uint64_t version, uint64_t budget) {
    DeltaCursor cursor(std::move(delta));
    return fold_metadata(generation, root, version, budget, [&]() -> std::optional<Metadata> {
        auto node = cursor.next();
        if (!node)
            return {};
        return Metadata{node->key.view(), node->value};
    });
}
} // namespace fengqudb::paged
