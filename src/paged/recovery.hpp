#pragma once
#include "view.hpp"

namespace fengqudb::paged {
inline bool same_storage(const Ref &a, const Ref &b) noexcept {
    return a.version == b.version && a.offset == b.offset && a.size == b.size && a.crc == b.crc &&
           a.depth == b.depth && a.recipe_size == b.recipe_size;
}
inline void count_change(View &view, const std::optional<Ref> &old, const Ref &value) {
    if (!old)
        ++view.entries;
    if (old && !old->deleted()) {
        require(view.keys && view.live_bytes >= old->size);
        --view.keys;
        view.live_bytes -= old->size;
    }
    if (!value.deleted()) {
        ++view.keys;
        view.live_bytes += value.size;
    }
}
inline void verify_raw(Generation &generation, const Ref &value, uint64_t limit) {
    valid_ref(value, limit);
    std::array<char, 65536> buffer;
    uint64_t offset = value.offset, remaining = value.size;
    uint32_t crc = 0;
    while (remaining) {
        auto size = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
        generation.file->read(offset, buffer.data(), size);
        crc = storage::crc32(buffer.data(), size, crc);
        offset += size;
        remaining -= size;
    }
    require(crc == value.crc);
}
struct RecoveryResult {
    std::shared_ptr<View> view;
    uint64_t repaired = 0, replayed = 0;
};
inline RecoveryResult recover(std::shared_ptr<Generation> generation, const std::string &directory,
                              uint64_t delta_budget, uint64_t writer_budget,
                              const Checkpoint *checked = nullptr) {
    auto checkpoint = checked ? *checked : generation->checkpoint();
    auto physical_end = generation->tail;
    auto result = std::make_shared<View>();
    result->generation = generation;
    result->checkpoint = checkpoint;
    result->version = result->durable = checkpoint.version;
    result->limit = checkpoint.end;
    result->entries = checkpoint.entries;
    result->keys = checkpoint.user_keys;
    result->live_bytes = checkpoint.live_bytes;
    require(checkpoint.user_keys <= checkpoint.entries &&
            (!checkpoint.root ? !checkpoint.entries : true));
    if (checkpoint.root) {
        auto root = generation->page(checkpoint.root, checkpoint.end);
        require(root->maximum <= checkpoint.version);
    }
    std::shared_ptr<Generation> scratch;
    uint64_t scratch_root = 0, position = checkpoint.end, replayed = 0;
    auto find = [&](std::string_view key) -> std::optional<Ref> {
        if (auto node = delta_find(result->delta, key))
            return node->value;
        if (scratch_root)
            if (auto value = Table(scratch, scratch_root, scratch->tail).find(key))
                return value;
        return Table(generation, checkpoint.root, checkpoint.end).find(key);
    };
    auto spill = [&](uint64_t version) {
        if (!scratch) {
            scratch = std::make_shared<Generation>(0);
            auto id = new_uuid();
            scratch->create(join(directory, temporary_name('r', id)), id, 1);
            scratch->remove_on_close.store(true, std::memory_order_relaxed);
        }
        scratch_root = fold_delta(scratch, scratch_root, result->delta, version, writer_budget);
        result->delta.reset();
    };
    // Nothing below this loop writes or truncates the original generation.
    while (position < physical_end) {
        auto frame = generation->frame(position, physical_end);
        if (!frame)
            break;
        if (frame->kind == Frame::Kind::wal) {
            require(result->version != UINT64_MAX && frame->version == result->version + 1);
            uint64_t features = 0;
            auto timestamp = read_wal(
                *generation, *frame,
                [&](const WalOperation &operation) {
                    auto old = find(operation.key);
                    require(operation.base.version == (old ? old->version : 0));
                    if (patch_kind(operation.kind) || operation.kind == OpKind::expiry)
                        require(old && same_storage(*old, operation.base));
                    if (operation.kind == OpKind::put)
                        verify_raw(*generation, operation.result, frame->end);
                    else if (patch_kind(operation.kind))
                        (void)generation->value(operation.result, frame->end);
                    DeltaBuilder edit(std::move(result->delta));
                    edit.set(operation.key, operation.result);
                    result->delta = edit.finish();
                    count_change(*result, old, operation.result);
                    if (result->delta->bytes > delta_budget)
                        spill(frame->version);
                },
                &features);
            require((features & result->checkpoint.features) == result->checkpoint.features);
            result->checkpoint.features = features;
            result->checkpoint.time = std::max(result->checkpoint.time, timestamp);
            result->version = frame->version;
            ++replayed;
        } else
            require(frame->version <= result->version);
        position = frame->end;
    }
    const auto repaired = physical_end - position;
    const bool needs_seal = replayed || position != checkpoint.end || repaired;
    // A legacy exhausted generation may contain a tail. Refuse it without
    // truncating or appending to the original file when no new root can seal.
    require(!needs_seal || checkpoint.sequence != UINT64_MAX, FQDB_LIMIT);
    // A semantically invalid complete record never reaches this mutation point.
    if (repaired)
        generation->file->truncate(position);
    generation->tail = position;
    if (needs_seal) {
        auto root = checkpoint.root;
        if (scratch_root) {
            View changes;
            changes.generation = scratch;
            changes.checkpoint.root = scratch_root;
            changes.delta = result->delta;
            changes.limit = scratch->tail;
            ViewCursor cursor(changes);
            root = fold_metadata(generation, root, result->version, writer_budget,
                                 [&] { return cursor.next(); });
        } else if (result->delta)
            root = fold_delta(generation, root, result->delta, result->version, writer_budget);
        checkpoint.root = root;
        checkpoint.version = result->version;
        checkpoint.entries = result->entries;
        checkpoint.user_keys = result->keys;
        checkpoint.live_bytes = result->live_bytes;
        checkpoint.time = std::max(result->checkpoint.time, unix_ms());
        checkpoint.features = result->checkpoint.features;
        generation->seal(checkpoint);
        result->checkpoint = checkpoint;
        result->delta.reset();
    }
    result->limit = generation->tail;
    result->durable = result->version;
    return {std::move(result), repaired, replayed};
}
} // namespace fengqudb::paged
