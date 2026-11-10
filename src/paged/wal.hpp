#pragma once
#include "generation.hpp"
#include "../crc_delta.hpp"
#include "../binary_diff.hpp"

namespace fengqudb::paged {
enum class OpKind : uint32_t { put = 1, erase = 2, patch = 3, expiry = 4, multi = 5 };
inline bool patch_kind(OpKind kind) noexcept {
    return kind == OpKind::patch || kind == OpKind::multi;
}
// Bounded automatic delta candidates. Explicit patches already know their range.
inline bool save_candidate(OpKind kind, const Value &value) noexcept {
    return kind == OpKind::put && value && value->size() >= 1024 && value->size() <= 65536;
}
inline void valid_ref(const Ref &ref, uint64_t limit, bool allow_deleted = false) {
    require(ref.valid());
    if (ref.deleted()) {
        require(allow_deleted && !ref.size && !ref.expires && !ref.crc && !ref.depth &&
                !ref.recipe_size);
    } else {
        require(ref.offset >= data_start && ref.offset < limit &&
                (ref.depth ? ref.recipe_size && ref.recipe_size <= limit - ref.offset
                           : !ref.recipe_size && ref.size <= limit - ref.offset));
    }
}
inline uint32_t value_crc(const Value &value) {
    auto cached = value->cached_crc.load(std::memory_order_relaxed);
    if (cached)
        return static_cast<uint32_t>(cached);
    auto result = storage::crc32(value->data(), value->size());
    value->cached_crc.store((UINT64_C(1) << 32) | result, std::memory_order_relaxed);
    return result;
}
struct WalOperation {
    OpKind kind = OpKind::erase;
    Bytes key;
    Ref base, result;
    uint64_t patch_offset = 0, remove = 0, insert_offset = 0, insert_size = 0;
    struct Range {
        uint32_t offset, size;
        uint64_t data;
    };
    std::vector<Range> ranges;
};
inline WalOperation read_operation(SharedFile &file, uint64_t begin, uint64_t end,
                                   uint64_t version) {
    require(begin < end && end - begin <= FQDB_MAX_TRANSACTION_BYTES);
    Input input(file, begin, end);
    WalOperation result;
    result.result.version = version;
    uint32_t seen = 0;
    while (input.position() < input.end()) {
        auto tag = input.tag(), number = tag >> 3, type = tag & 7;
        if (number > 14) {
            input.skip_field(tag);
            continue;
        }
        require(!(seen & (1u << number)));
        seen |= 1u << number;
        if (number == 1) {
            require(type == 0);
            auto kind = input.varint();
            require(kind >= 1 && kind <= 5);
            result.kind = static_cast<OpKind>(kind);
        } else if (number == 2) {
            require(type == 2);
            result.key = input.bytes(max_internal_key_bytes);
        } else if (number == 3) {
            require(type == 2);
            result.insert_size = input.varint();
            result.insert_offset = input.position();
            require(result.insert_size <= FQDB_MAX_VALUE_BYTES);
            input.skip(result.insert_size);
        } else {
            auto width = number == 8 || number == 12 || number >= 13 ? 4u : 8u;
            require(type == (width == 4 ? 5u : 1u));
            auto value = input.fixed(width);
            switch (number) {
            case 4:
                result.base.version = value;
                break;
            case 5:
                result.patch_offset = value;
                break;
            case 6:
                result.remove = value;
                break;
            case 7:
                result.result.expires = value;
                break;
            case 8:
                result.result.crc = static_cast<uint32_t>(value);
                break;
            case 9:
                result.result.size = value;
                break;
            case 10:
                result.base.offset = value;
                break;
            case 11:
                result.base.size = value;
                break;
            case 12:
                result.base.crc = static_cast<uint32_t>(value);
                break;
            case 13:
                result.base.depth = static_cast<uint32_t>(value);
                break;
            case 14:
                result.base.recipe_size = static_cast<uint32_t>(value);
                break;
            }
        }
    }
    constexpr uint32_t common =
        (1u << 1) | (1u << 2) | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 9);
    constexpr uint32_t base = (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14);
    require((seen & common) == common && version && result.result.size <= FQDB_MAX_VALUE_BYTES &&
            result.base.version < version);
    if (result.kind == OpKind::put) {
        require(seen == (common | (1u << 3)) && result.result.size == result.insert_size);
        result.result.offset = result.insert_offset;
    } else if (result.kind == OpKind::erase) {
        require(seen == common && !result.result.size && !result.result.crc &&
                !result.result.expires);
    } else {
        valid_ref(result.base, begin);
        if (patch_kind(result.kind)) {
            require(seen == (common | base | (1u << 3) | (1u << 5) | (1u << 6)) &&
                    result.base.depth < 8 && result.patch_offset <= result.base.size &&
                    result.remove <= result.base.size - result.patch_offset &&
                    (result.kind == OpKind::multi
                         ? result.result.size == result.base.size && !result.patch_offset &&
                               !result.remove
                         : result.result.size ==
                               result.base.size - result.remove + result.insert_size));
            if (result.kind == OpKind::multi) {
                require(result.insert_size >= 4 && result.insert_size <= 1024 &&
                        result.base.size >= 1024 && result.base.size <= 65536);
                Input ranges(file, result.insert_offset, result.insert_offset + result.insert_size);
                auto count = ranges.fixed(4);
                require(count <= 8);
                result.ranges.reserve(static_cast<size_t>(count));
                uint64_t previous_end = 0;
                for (uint64_t i = 0; i < count; ++i) {
                    auto offset = ranges.fixed(4), size = ranges.fixed(4);
                    require(size && offset >= previous_end && offset <= result.base.size &&
                            size <= result.base.size - offset);
                    auto data = ranges.position();
                    ranges.skip(size);
                    result.ranges.push_back(
                        {static_cast<uint32_t>(offset), static_cast<uint32_t>(size), data});
                    previous_end = offset + size;
                }
                require(ranges.position() == ranges.end());
            }
            result.result.offset = begin;
            result.result.depth = result.base.depth + 1;
            result.result.recipe_size = static_cast<uint32_t>(end - begin);
        } else {
            require(seen == (common | base) && result.result.size == result.base.size &&
                    result.result.crc == result.base.crc);
            result.result.offset = result.base.offset;
            result.result.depth = result.base.depth;
            result.result.recipe_size = result.base.recipe_size;
        }
    }
    return result;
}

// Compare only a bounded, already-cached immutable base. Never perform a cold read
// just to discover whether compression would help. New bytes remain fully owned.
inline Value save_delta(Generation &generation, const Ref &base, const Value &next) {
    if (!base.version || base.deleted() || base.depth >= 8 || base.size != next->size())
        return {};
    uint64_t tag = 0;
    auto old = generation.values.get(base.offset, &tag);
    if (!old)
        return {};
    require(old->size() == base.size && value_crc(old) == base.crc &&
            tag == ((uint64_t(base.depth) << 32) | base.recipe_size));
    const auto plan = detail::binary_diff(*old, *next);
    if (!plan.accepted)
        return {};
    auto packed = std::make_shared<Data>();
    packed->resize(plan.bytes);
    auto out = reinterpret_cast<unsigned char *>(packed->data());
    storage::put32(out, plan.count);
    out += 4;
    for (uint32_t i = 0; i < plan.count; ++i) {
        const auto &range = plan.ranges[i];
        storage::put32(out, range.offset);
        storage::put32(out + 4, range.size);
        out += 8;
        std::memcpy(out, next->data() + range.offset, range.size);
        out += range.size;
    }
    return packed;
}

// Own only small operation headers; immutable large value/insert bytes are
// borrowed until append completes. A single gather is used only for small WALs.
class WalBatch {
    struct Item {
        Bytes prefix;
        Value bytes;
        Ref base, result;
        bool recipe;
        uint64_t value_at;
    };
    std::vector<Item> items_;
    Bytes timestamp_;
    Bytes last_;
    uint64_t size_ = 0, version_;

  public:
    explicit WalBatch(uint64_t version, uint64_t timestamp, uint64_t features = 0)
        : version_(version) {
        require(version != 0, FQDB_INVALID_ARGUMENT);
        wire::Writer out;
        out.fixed64_field(2, timestamp);
        if (features)
            out.fixed64_field(3, features);
        timestamp_ = out.take();
        size_ = timestamp_.size();
    }
    void add(OpKind kind, Bytes key, Value bytes, Ref base = {}, uint64_t expires = 0,
             uint64_t offset = 0, uint64_t remove = 0, uint32_t final_crc = 0) {
        require(key.size() <= max_internal_key_bytes && (items_.empty() || compare(last_, key) < 0),
                FQDB_INVALID_ARGUMENT);
        require(base.version < version_, FQDB_INVALID_ARGUMENT);
        auto length = bytes ? bytes->size() : 0;
        require(length <= FQDB_MAX_VALUE_BYTES, FQDB_LIMIT);
        const bool recipe = patch_kind(kind), existing = recipe || kind == OpKind::expiry;
        require(kind == OpKind::put || kind == OpKind::erase || existing, FQDB_INVALID_ARGUMENT);
        require((kind == OpKind::put || recipe) ? bytes != nullptr : !bytes, FQDB_INVALID_ARGUMENT);
        require(kind != OpKind::multi || (!offset && !remove && length >= 4 && length <= 1024 &&
                                          base.size >= 1024 && base.size <= 65536),
                FQDB_INVALID_ARGUMENT);
        if (existing) {
            valid_ref(base, UINT64_MAX);
            require(base.version && (recipe ? base.depth < 8 && offset <= base.size &&
                                                  remove <= base.size - offset
                                            : true),
                    FQDB_INVALID_ARGUMENT);
        }
        Ref result;
        result.version = version_;
        result.expires = expires;
        result.size =
            kind == OpKind::put
                ? length
                : (recipe ? (kind == OpKind::multi ? base.size : base.size - remove + length)
                          : (kind == OpKind::expiry ? base.size : 0));
        require(result.size <= FQDB_MAX_VALUE_BYTES, FQDB_LIMIT);
        result.crc = kind == OpKind::put
                         ? value_crc(bytes)
                         : (recipe ? final_crc : (kind == OpKind::expiry ? base.crc : 0));
        require(kind != OpKind::erase || expires == 0, FQDB_INVALID_ARGUMENT);
        wire::Writer head;
        head.reserve(key.size() + (existing ? 112 : 64));
        head.uint_field(1, static_cast<uint32_t>(kind));
        head.bytes_field(2, key);
        head.fixed64_field(4, base.version);
        if (recipe) {
            head.fixed64_field(5, offset);
            head.fixed64_field(6, remove);
        }
        head.fixed64_field(7, expires);
        head.fixed32_field(8, result.crc);
        head.fixed64_field(9, result.size);
        if (existing) {
            head.fixed64_field(10, base.offset);
            head.fixed64_field(11, base.size);
            head.fixed32_field(12, base.crc);
            head.fixed32_field(13, base.depth);
            head.fixed32_field(14, base.recipe_size);
        }
        if (bytes)
            head.message_header(3, length);
        auto operation_size = head.data().size() + length;
        wire::Writer outer;
        outer.message_header(1, operation_size);
        auto value_at = outer.data().size() + head.data().size();
        Bytes prefix = outer.take();
        prefix.append(head.data());
        if (recipe) {
            result.depth = base.depth + 1;
            result.recipe_size = static_cast<uint32_t>(operation_size);
        } else if (kind == OpKind::expiry) {
            result.offset = base.offset;
            result.depth = base.depth;
            result.recipe_size = base.recipe_size;
        }
        auto new_size = size_ + prefix.size() + length;
        require(new_size <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
        // Do not update ordering/size until every allocation has succeeded.
        items_.push_back({std::move(prefix), std::move(bytes), base, result, recipe, value_at});
        last_ = std::move(key);
        size_ = new_size;
    }
    // append() has finished reading these copied payloads. Keep large payload
    // references, which can still be borrowed by the deferred file layer.
    void release_small_values() noexcept {
        for (auto &item : items_)
            if (item.bytes && item.bytes->size() < 16384)
                item.bytes.reset();
    }
    uint64_t size() const noexcept {
        return size_;
    }
    std::vector<Ref> references(uint64_t position) const {
        require(position <= INT64_MAX && size_ + 56 <= INT64_MAX - position, FQDB_LIMIT);
        std::vector<Ref> result;
        result.reserve(items_.size());
        uint64_t cursor = position + 32 + timestamp_.size();
        for (const auto &item : items_) {
            auto ref = item.result;
            if (item.recipe)
                ref.offset =
                    cursor + wire::varint_size(10) + wire::varint_size(item.result.recipe_size);
            else if (item.bytes)
                ref.offset = cursor + item.value_at;
            result.push_back(ref);
            cursor += item.prefix.size() + (item.bytes ? item.bytes->size() : 0);
        }
        return result;
    }
    std::vector<Ref> append(Generation &generation, std::vector<Ref> *prepared = nullptr) const {
        require(!items_.empty(), FQDB_INVALID_ARGUMENT);
        require(!generation.write_failed, FQDB_COMMIT_UNKNOWN);
        for (const auto &item : items_)
            if (item.recipe || (!item.bytes && !item.result.deleted()))
                valid_ref(item.base, generation.tail);
        uint32_t crc = storage::crc32(timestamp_.data(), timestamp_.size());
        for (const auto &item : items_) {
            crc = storage::crc32(item.prefix.data(), item.prefix.size(), crc);
            if (item.bytes) {
                const auto cached = item.bytes->cached_crc.load(std::memory_order_relaxed);
                // Immutable payloads already checksummed by add() need no
                // second byte scan. Small inserts keep the cheaper direct CRC.
                crc = cached && item.bytes->size() >= 512
                          ? storage::crc32_combine(crc, static_cast<uint32_t>(cached),
                                                   item.bytes->size())
                          : storage::crc32(item.bytes->data(), item.bytes->size(), crc);
            }
        }
        storage::RecordFrame frame(wal_magic, version_, static_cast<size_t>(size_), crc);
        auto position = generation.tail;
        require(position <= INT64_MAX && size_ + 56 <= INT64_MAX - position, FQDB_LIMIT);
        IOWritePermit permit(generation.file->reservation_bytes(position, size_ + 56));
        auto result = prepared ? std::move(*prepared) : references(position);
        require(result.size() == items_.size(), FQDB_INTERNAL_ERROR);
        try {
            if (generation.file->checksummed_records()) {
                auto write = [&](const void *data, size_t size, uint32_t checksum,
                                 std::shared_ptr<const void> owner = {}) {
                    generation.file->write_known(position, data, size, checksum, std::move(owner));
                    position += size;
                };
                // Keep tiny keys, headers and values in bounded chunks. Borrow
                // only genuinely large immutable payloads, avoiding hundreds
                // of descriptors for batches with many small rows.
                std::array<unsigned char, 16384> buffer;
                size_t used = 0;
                auto flush = [&] {
                    if (used) {
                        auto checksum =
                            size_ + 56 <= buffer.size() && size_ >= 1024
                                ? storage::crc32_combine(
                                      storage::crc32_combine(
                                          storage::crc32(frame.header.data(), 32), crc, size_),
                                      storage::crc32(frame.trailer.data(), 24), 24)
                                : storage::crc32(buffer.data(), used);
                        write(buffer.data(), used, checksum);
                        used = 0;
                    }
                };
                auto append = [&](const void *data, size_t size) {
                    auto p = static_cast<const unsigned char *>(data);
                    while (size) {
                        const auto n = std::min(size, buffer.size() - used);
                        std::memcpy(buffer.data() + used, p, n);
                        used += n;
                        p += n;
                        size -= n;
                        if (used == buffer.size())
                            flush();
                    }
                };
                append(frame.header.data(), 32);
                append(timestamp_.data(), timestamp_.size());
                for (const auto &item : items_) {
                    append(item.prefix.data(), item.prefix.size());
                    if (item.bytes) {
                        if (item.bytes->size() >= buffer.size()) {
                            flush();
                            write(item.bytes->data(), item.bytes->size(), value_crc(item.bytes),
                                  item.bytes);
                        } else
                            append(item.bytes->data(), item.bytes->size());
                    }
                }
                append(frame.trailer.data(), 24);
                flush();
            } else if (size_ + 56 <= 4096) {
                std::array<char, 4096> buffer;
                size_t at = 0;
                auto copy = [&](const void *data, size_t size) {
                    std::memcpy(buffer.data() + at, data, size);
                    at += size;
                };
                copy(frame.header.data(), 32);
                copy(timestamp_.data(), timestamp_.size());
                for (const auto &item : items_) {
                    copy(item.prefix.data(), item.prefix.size());
                    if (item.bytes)
                        copy(item.bytes->data(), item.bytes->size());
                }
                copy(frame.trailer.data(), 24);
                generation.file->write(position, buffer.data(), at);
            } else {
                // Bound transient memory and coalesce small headers/payloads.
                // A large contiguous payload still goes directly to the file;
                // neither values nor subtraction recipes are reconstructed.
                std::array<char, 16384> buffer;
                size_t used = 0;
                auto flush = [&] {
                    if (used) {
                        generation.file->write(position, buffer.data(), used);
                        position += used;
                        used = 0;
                    }
                };
                auto append = [&](const void *input, size_t size) {
                    auto data = static_cast<const char *>(input);
                    while (size) {
                        if (!used && size >= buffer.size()) {
                            generation.file->write(position, data, size);
                            position += size;
                            return;
                        }
                        const auto count = std::min(size, buffer.size() - used);
                        std::memcpy(buffer.data() + used, data, count);
                        used += count;
                        data += count;
                        size -= count;
                        if (used == buffer.size())
                            flush();
                    }
                };
                append(frame.header.data(), 32);
                append(timestamp_.data(), timestamp_.size());
                for (const auto &item : items_) {
                    append(item.prefix.data(), item.prefix.size());
                    if (item.bytes)
                        append(item.bytes->data(), item.bytes->size());
                }
                append(frame.trailer.data(), 24);
                flush();
            }
            generation.tail += size_ + 56;
            return result;
        } catch (...) {
            generation.write_failed = true;
            throw;
        }
    }
};

template <class Callback>
inline uint64_t read_wal(Generation &generation, const Frame &frame, Callback &&consume,
                         uint64_t *features = nullptr) {
    require(frame.kind == Frame::Kind::wal && frame.version);
    Input input(*generation.file, frame.payload, frame.payload + frame.length);
    bool timestamp_seen = false, operation_seen = false, features_seen = false;
    if (features)
        *features = 0;
    uint64_t timestamp = 0;
    Bytes previous;
    while (input.position() < input.end()) {
        auto tag = input.tag();
        if (tag == 17) {
            require(!timestamp_seen && !operation_seen);
            timestamp = input.fixed(8);
            timestamp_seen = true;
        } else if (tag == 25) {
            require(timestamp_seen && !operation_seen && !features_seen);
            auto value = input.fixed(8);
            require(value == 0 || value == 1 || value == 4 || value == 5);
            if (features)
                *features = value;
            features_seen = true;
        } else if (tag == 10) {
            require(timestamp_seen);
            auto size = input.varint(), begin = input.position();
            input.skip(size);
            auto operation = read_operation(*generation.file, begin, begin + size, frame.version);
            require(!operation_seen || compare(previous, operation.key) < 0);
            previous = operation.key;
            operation_seen = true;
            consume(operation);
        } else
            input.skip_field(tag);
    }
    require(timestamp_seen && operation_seen);
    return timestamp;
}

inline Value Generation::value(const Ref &reference, uint64_t limit, bool retain, bool use_cache) {
    valid_ref(reference, limit);
    auto cached = [&](const Ref &ref) -> Value {
        if (!use_cache)
            return {};
        uint64_t tag = 0;
        auto value = values.get(ref.offset, &tag);
        if (value)
            require(value->size() == ref.size && value_crc(value) == ref.crc &&
                    tag == ((static_cast<uint64_t>(ref.depth) << 32) | ref.recipe_size));
        return value;
    };
    if (auto result = cached(reference))
        return result;
    std::vector<WalOperation> recipes;
    recipes.reserve(reference.depth);
    Ref base = reference;
    Value source;
    while (base.depth) {
        auto recipe =
            read_operation(*file, base.offset, base.offset + base.recipe_size, base.version);
        require(patch_kind(recipe.kind) && recipe.result.size == base.size &&
                recipe.result.depth == base.depth && recipe.result.crc == base.crc);
        base = recipe.base;
        recipes.push_back(std::move(recipe));
        if ((source = cached(base)))
            break;
    }
    auto result = std::make_shared<Data>();
    if (source)
        result->assign(*source); // One isolation copy, then all edits reuse it.
    else if (!recipes.empty() && !base.depth && recipes.back().kind == OpKind::patch) {
        // A cold subtraction reads only the surviving raw extents. The deleted
        // range is neither read nor concatenated into a temporary full value.
        auto &first = recipes.back();
        result->resize(static_cast<size_t>(first.result.size));
        auto prefix = static_cast<size_t>(first.patch_offset),
             insert = static_cast<size_t>(first.insert_size);
        auto remaining = static_cast<size_t>(base.size - first.patch_offset - first.remove);
        file->read(base.offset, result->data(), prefix);
        file->read(first.insert_offset, result->data() + prefix, insert);
        file->read(base.offset + first.patch_offset + first.remove,
                   result->data() + prefix + insert, remaining);
        recipes.pop_back();
    } else {
        require(!base.depth);
        result->resize(static_cast<size_t>(base.size));
        file->read(base.offset, result->data(), result->size());
        // With no recipes these are already the final bytes. The common
        // result check below validates the same full value exactly once.
    }
    for (auto it = recipes.rbegin(); it != recipes.rend(); ++it) {
        auto offset = static_cast<size_t>(it->patch_offset),
             remove = static_cast<size_t>(it->remove);
        auto insert = static_cast<size_t>(it->insert_size), old = result->size(),
             next = static_cast<size_t>(it->result.size);
        require(old == it->base.size);
        if (it->kind == OpKind::multi) {
            require(next == old);
            for (const auto &range : it->ranges)
                file->read(range.data, result->data() + range.offset, range.size);
            continue;
        }
        if (next > old)
            result->resize(next);
        if (insert != remove)
            std::memmove(result->data() + offset + insert, result->data() + offset + remove,
                         old - offset - remove);
        if (next < old)
            result->resize(next);
        file->read(it->insert_offset, result->data() + offset, insert);
    }
    require(result->size() == reference.size &&
            storage::crc32(result->data(), result->size()) == reference.crc);
    result->cached_crc.store((UINT64_C(1) << 32) | reference.crc, std::memory_order_relaxed);
    try {
        if (retain)
            values.put(reference.offset, result, sizeof(Data) + result->capacity(),
                       (static_cast<uint64_t>(reference.depth) << 32) | reference.recipe_size);
    } catch (const std::bad_alloc &) {
    }
    return result;
}
} // namespace fengqudb::paged
