#pragma once
#include "core.hpp"
#include "feature_keys.hpp"

namespace fengqudb::paged {
constexpr size_t max_history_segments = 128, max_history_pins = 64;
inline Bytes history_prefix() {
    return system_prefix() + "H";
}
inline Bytes history_pin_bytes(const Uuid &uuid, uint64_t version) {
    Bytes bytes(reinterpret_cast<const char *>(uuid.data()), uuid.size());
    bytes.resize(24);
    storage::put64(reinterpret_cast<unsigned char *>(bytes.data() + 16), version);
    return bytes;
}
struct HistoryPins {
    uint64_t minimum, active = 0, total = 0;
};
inline HistoryPins history_pins(const View &view) {
    HistoryPins pins{view.version};
    if ((view.checkpoint.features & 5) != 5)
        return pins;
    auto prefix = history_prefix();
    ViewCursor cursor(view, prefix);
    while (auto row = cursor.next()) {
        if (row->key.substr(0, prefix.size()) != prefix)
            break;
        if (row->value.deleted())
            continue;
        require(!row->value.expires && row->value.size == 24 && ++pins.total <= max_history_pins);
        auto name = uncomponent(row->key.substr(prefix.size()));
        require(!name.empty() && name.size() <= 128);
        auto value = view.generation->value(row->value, view.limit);
        if (std::memcmp(value->data(), view.generation->uuid.data(), 16))
            continue; // Foreign backup control, never a new database's retention policy.
        auto from = storage::get64(reinterpret_cast<const unsigned char *>(value->data() + 16));
        require(from >= history_floor(view) && from <= view.version);
        pins.minimum = std::min(pins.minimum, from);
        ++pins.active;
    }
    return pins;
}
inline void load_history(Core &core) {
    auto view = core.current();
    if (view->checkpoint.parent) {
        auto chain = std::make_shared<HistoryChain>(core.pin_budget);
        auto link = view->checkpoint;
        uint64_t minimum = history_floor(*view);
        while (link.parent) {
            require(chain->segments.size() < max_history_segments, FQDB_LIMIT);
            auto generation = std::make_shared<Generation>(0);
            auto name = link.parent_legacy
                            ? legacy_generation_name(view->generation->uuid, link.parent)
                            : generation_name(view->generation->uuid, link.parent);
            generation->open(join(core.directory, name), view->generation->uuid, link.parent, true);
            auto checkpoint = generation->checkpoint();
            require(link.parent_limit <= generation->tail && checkpoint.end <= link.parent_limit &&
                    checkpoint.version <= link.parent_version &&
                    checkpoint.floor <= link.parent_version);
            chain->segments.push_back(
                {generation, link.parent_limit, link.parent_version, checkpoint.floor,
                 checkpoint.history_start ? checkpoint.history_start : data_start});
            chain->charge.resize(sizeof(HistoryChain) +
                                 chain->segments.capacity() * sizeof(HistoryChain::Segment) +
                                 chain->segments.size() * 1024 + 128);
            if (minimum >= checkpoint.floor)
                break;
            require(checkpoint.parent && minimum >= checkpoint.retained_floor);
            link = checkpoint;
        }
        require(!chain->segments.empty() && minimum >= chain->segments.back().floor);
        auto next = std::make_shared<View>(*view);
        next->history = std::move(chain);
        core.state = std::move(next);
    }
    (void)history_pins(*core.current());
}
inline bool plan_history(Core &core, const View &source, View &target) {
    auto pins = history_pins(source);
    if (auto hub = core.watches.lock())
        hub->retain(pins.minimum, pins.active);
    auto &checkpoint = target.checkpoint;
    checkpoint.parent = checkpoint.parent_version = checkpoint.parent_limit =
        checkpoint.parent_legacy = checkpoint.retained_floor = 0;
    target.history.reset();
    if (!pins.active || pins.minimum == source.version)
        return false;
    auto chain = std::make_shared<HistoryChain>(core.pin_budget);
    if (source.version > source.checkpoint.floor) {
        chain->segments.push_back(
            {source.generation, source.limit, source.version, source.checkpoint.floor,
             source.checkpoint.history_start ? source.checkpoint.history_start : data_start});
        // Buffered published WAL must survive a crash after CURRENT selects the child.
        try {
            source.generation->file->sync();
        } catch (...) {
            core.poisoned = true;
            throw Error(FQDB_COMMIT_UNKNOWN);
        }
    }
    if (pins.minimum < source.checkpoint.floor) {
        require(bool(source.history));
        for (const auto &segment : source.history->segments) {
            chain->segments.push_back(segment);
            if (pins.minimum >= segment.floor)
                break;
        }
    }
    require(!chain->segments.empty() && chain->segments.size() <= max_history_segments, FQDB_LIMIT);
    require(pins.minimum >= chain->segments.back().floor);
    chain->charge.resize(sizeof(HistoryChain) +
                         chain->segments.capacity() * sizeof(HistoryChain::Segment) +
                         chain->segments.size() * 1024 + 128);
    const auto &parent = chain->segments.front();
    checkpoint.parent = parent.generation->number;
    checkpoint.parent_version = parent.version;
    checkpoint.parent_limit = parent.limit;
    checkpoint.parent_legacy =
        std::filesystem::u8path(parent.generation->path).filename().u8string().size() == 57;
    checkpoint.retained_floor = pins.minimum;
    checkpoint.features |= 4;
    target.history = std::move(chain);
    return true;
}
inline void retire_history(const View &source, const View &target) {
    auto keep = history_keep(target);
    auto retire = [&](const std::shared_ptr<Generation> &generation) {
        generation->remove_on_close.store(std::find(keep.begin(), keep.end(), generation->number) ==
                                              keep.end(),
                                          std::memory_order_relaxed);
    };
    retire(source.generation);
    if (source.history)
        for (const auto &segment : source.history->segments)
            retire(segment.generation);
}

// Streams immutable generation segments in commit order. No in-memory event
// queue and no materialization of overwritten values unless the caller asks.
class HistoryReader {
    std::shared_ptr<const View> view_;
    size_t segment_ = 0;
    uint64_t position_ = data_start, expected_;
    HistoryChain::Segment current() const {
        if (view_->history && segment_ < view_->history->segments.size())
            return view_->history->segments[view_->history->segments.size() - 1 - segment_];
        return {view_->generation, view_->limit, view_->version, view_->checkpoint.floor,
                view_->checkpoint.history_start ? view_->checkpoint.history_start : data_start};
    }

  public:
    struct Position {
        uint64_t generation = 0, offset = data_start, version = 0;
    };
    struct Record {
        std::shared_ptr<Generation> generation;
        Frame frame;
    };
    explicit HistoryReader(std::shared_ptr<const View> view, uint64_t from,
                           const Position *resume = nullptr)
        : view_(std::move(view)), expected_(from) {
        require(from >= history_floor(*view_) && from <= view_->version, FQDB_HISTORY_LOST);
        const size_t ancestors = view_->history ? view_->history->segments.size() : 0;
        if (from == view_->version) {
            segment_ = ancestors;
            position_ = view_->limit;
        } else if (resume && resume->generation && resume->version == from) {
            for (size_t i = 0; i <= ancestors; ++i) {
                segment_ = i;
                auto source = current();
                if (source.generation->number == resume->generation &&
                    resume->offset >= data_start && resume->offset <= source.limit &&
                    from >= source.floor && from <= source.version) {
                    position_ = resume->offset;
                    return;
                }
            }
            segment_ = 0;
        }
    }
    Position position() const {
        auto source = current();
        return {source.generation->number, position_, expected_};
    }
    uint64_t version() const noexcept {
        return expected_;
    }
    template <class Check> std::optional<Record> next(Check check) {
        if (expected_ == view_->version)
            return {};
        const size_t total = (view_->history ? view_->history->segments.size() : 0) + 1;
        while (segment_ < total) {
            check();
            auto source = current();
            if (source.version <= expected_) {
                ++segment_;
                position_ = data_start;
                continue;
            }
            require(expected_ >= source.floor, FQDB_HISTORY_LOST);
            position_ = std::max(position_, source.start);
            while (position_ < source.limit) {
                check();
                auto frame = source.generation->frame(position_, source.limit, false);
                require(bool(frame));
                position_ = frame->end;
                if (frame->kind != Frame::Kind::wal || frame->version <= expected_)
                    continue;
                require(expected_ != UINT64_MAX && frame->version == expected_ + 1,
                        FQDB_HISTORY_LOST);
                (void)source.generation->frame(frame->offset, source.limit, true);
                expected_ = frame->version;
                return Record{std::move(source.generation), *frame};
            }
            require(expected_ == source.version, FQDB_HISTORY_LOST);
            ++segment_;
            position_ = data_start;
        }
        require(expected_ == view_->version, FQDB_HISTORY_LOST);
        return {};
    }
};
} // namespace fengqudb::paged
