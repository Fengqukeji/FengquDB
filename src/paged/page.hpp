#pragma once
#include "io.hpp"
#include "fengqudb_wire.hpp"
#include "value_bytes.hpp"
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace fengqudb::paged {
using Bytes = std::string;
using detail::compare;
// This namespace is enabled only by an atomic feature-bearing transaction.
// Older user records in the range are rejected before activation, never hidden.
inline const Bytes &system_prefix() {
    static const Bytes value("\0FQDB\xff", 6);
    return value;
}
inline bool system_key(std::string_view key) noexcept {
    return key.size() >= 6 && key.substr(0, 6) == std::string_view("\0FQDB\xff", 6);
}
inline Bytes prefix_end(Bytes prefix) {
    for (size_t i = prefix.size(); i; --i)
        if (static_cast<unsigned char>(prefix[i - 1]) != 255) {
            ++prefix[i - 1];
            prefix.resize(i);
            return prefix;
        }
    return {};
}
constexpr size_t max_internal_key_bytes = 4 * FQDB_MAX_KEY_BYTES + 1024;
constexpr size_t max_page_bytes = 1048576;
struct Ref {
    uint64_t version = 0, offset = 0, size = 0, expires = 0;
    uint32_t crc = 0, depth = 0, recipe_size = 0;
    bool deleted() const noexcept {
        return offset == 0;
    }
    bool visible(uint64_t now) const noexcept {
        return offset && (!expires || expires > now);
    }
    bool expired(uint64_t now) const noexcept {
        return offset && expires && expires <= now;
    }
    uint64_t earliest_expiry() const noexcept {
        return offset && expires ? expires : UINT64_MAX;
    }
    bool valid() const noexcept {
        if (!version || size > FQDB_MAX_VALUE_BYTES || depth > 8)
            return false;
        if (!offset)
            return !size && !expires && !crc && !depth && !recipe_size;
        return offset >= 12288 && offset < INT64_MAX &&
               (depth ? recipe_size && recipe_size <= FQDB_MAX_TRANSACTION_BYTES : !recipe_size);
    }
};
struct Slot {
    std::string_view key;
    Ref value;
    // Zero is an unknown legacy summary; UINT64_MAX means no expiry.
    uint64_t child = 0, maximum = 0, earliest = 0;
    size_t start = 0, length = 0, key_offset = 0;
};
struct Page {
    bool leaf = true;
    uint64_t maximum = 0, earliest = UINT64_MAX;
    uint64_t frame_offset = 0, frame_end = 0, frame_version = 0;
    Bytes binary;
    std::vector<Slot> slots;
    explicit Page(bool is_leaf = true) : leaf(is_leaf) {
        wire::Writer out;
        out.uint_field(1, leaf ? 1 : 2);
        out.fixed64_field(2, 0);
        binary = out.take();
    }
    void rebase(size_t begin = 0) noexcept {
        for (size_t i = begin; i < slots.size(); ++i) {
            auto &slot = slots[i];
            slot.key = std::string_view(binary.data() + slot.key_offset, slot.key.size());
        }
    }
    Page(const Page &other)
        : leaf(other.leaf), maximum(other.maximum), earliest(other.earliest),
          frame_offset(other.frame_offset), frame_end(other.frame_end),
          frame_version(other.frame_version), binary(other.binary), slots(other.slots) {
        rebase();
    }
    Page(Page &&other) noexcept
        : leaf(other.leaf), maximum(other.maximum), earliest(other.earliest),
          frame_offset(other.frame_offset), frame_end(other.frame_end),
          frame_version(other.frame_version), binary(std::move(other.binary)),
          slots(std::move(other.slots)) {
        rebase();
    }
    Page &operator=(const Page &other) {
        if (this != &other) {
            Page copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    Page &operator=(Page &&other) noexcept {
        if (this != &other) {
            leaf = other.leaf;
            maximum = other.maximum;
            earliest = other.earliest;
            frame_offset = other.frame_offset;
            frame_end = other.frame_end;
            frame_version = other.frame_version;
            binary = std::move(other.binary);
            slots = std::move(other.slots);
            rebase();
        }
        return *this;
    }
    static Bytes encode(const Slot &slot, bool leaf) {
        wire::Writer item;
        item.bytes_field(1, slot.key);
        if (leaf) {
            item.fixed64_field(2, slot.value.version);
            item.fixed64_field(3, slot.value.offset);
            item.fixed64_field(4, slot.value.size);
            item.fixed32_field(5, slot.value.crc);
            item.fixed64_field(6, slot.value.expires);
            item.fixed32_field(9, slot.value.depth);
            item.fixed32_field(10, slot.value.recipe_size);
        } else {
            item.fixed64_field(7, slot.child);
            item.fixed64_field(8, slot.maximum);
            if (slot.earliest)
                item.fixed64_field(11, slot.earliest);
        }
        wire::Writer out;
        out.bytes_field(3, item.data());
        return out.take();
    }
    void summarize() noexcept {
        maximum = 0;
        earliest = UINT64_MAX;
        for (const auto &slot : slots) {
            maximum = std::max(maximum, leaf ? slot.value.version : slot.maximum);
            earliest = std::min(earliest, leaf ? slot.value.earliest_expiry() : slot.earliest);
        }
        // Canonical prefix: tag/type (2 bytes), fixed64 tag (1), payload (8).
        storage::put64(reinterpret_cast<unsigned char *>(binary.data()) + 3, maximum);
    }
    size_t lower(std::string_view key) const noexcept {
        return static_cast<size_t>(std::lower_bound(slots.begin(), slots.end(), key,
                                                    [](const Slot &slot, std::string_view value) {
                                                        return compare(slot.key, value) < 0;
                                                    }) -
                                   slots.begin());
    }
    void set(size_t at, Slot slot, bool insert) {
        require(at <= slots.size() && (insert || at < slots.size()), FQDB_INTERNAL_ERROR);
        const auto next_maximum = leaf ? slot.value.version : slot.maximum;
        const auto next_earliest = leaf ? slot.value.earliest_expiry() : slot.earliest;
        // Insertion and monotone updates keep summaries in constant time.
        // Replacing an extremum in the opposite direction needs a full scan.
        const bool rescan = !insert &&
                            (((leaf ? slots[at].value.version : slots[at].maximum) == maximum &&
                              next_maximum < maximum) ||
                             ((leaf ? slots[at].value.earliest_expiry() : slots[at].earliest) ==
                                  earliest &&
                              next_earliest > earliest));
        auto encoded = encode(slot, leaf);
        const auto original = binary.data();
        const auto position = at == slots.size() ? binary.size() : slots[at].start;
        const auto removed = insert ? 0 : slots[at].length;
        // Prepare all potentially allocating bookkeeping before binary mutation.
        if (insert)
            slots.reserve(slots.size() + 1);
        binary.replace(position, removed, encoded);
        slot.start = position;
        slot.length = encoded.size();
        slot.key_offset = position + encoded.size() - (leaf ? 51 : 18 + (slot.earliest ? 9 : 0)) -
                          slot.key.size();
        if (insert)
            slots.insert(slots.begin() + static_cast<ptrdiff_t>(at), std::move(slot));
        else
            slots[at] = std::move(slot);
        if (removed != encoded.size()) {
            for (size_t i = at + 1; i < slots.size(); ++i) {
                slots[i].start = slots[i].start - removed + encoded.size();
                slots[i].key_offset = slots[i].key_offset - removed + encoded.size();
            }
            rebase(binary.data() == original ? at : 0);
        } else if (binary.data() != original)
            rebase();
        else {
            auto &changed = slots[at];
            changed.key = std::string_view(binary.data() + changed.key_offset, changed.key.size());
        }
        if (rescan)
            summarize();
        else {
            maximum = std::max(maximum, next_maximum);
            earliest = std::min(earliest, next_earliest);
            storage::put64(reinterpret_cast<unsigned char *>(binary.data()) + 3, maximum);
        }
    }
    Page split(size_t at) {
        require(at && at < slots.size(), FQDB_INTERNAL_ERROR);
        // Clone the complete cache once and subtract opposite ranges. Neither
        // side concatenates each retained entry again.
        Page right = *this;
        const auto cut = slots[at].start;
        right.binary.erase(11, cut - 11);
        right.slots.erase(right.slots.begin(), right.slots.begin() + static_cast<ptrdiff_t>(at));
        for (auto &slot : right.slots) {
            slot.start -= cut - 11;
            slot.key_offset -= cut - 11;
        }
        binary.erase(cut);
        slots.erase(slots.begin() + static_cast<ptrdiff_t>(at), slots.end());
        rebase();
        right.rebase();
        summarize();
        right.summarize();
        return right;
    }
    uint64_t allocation() const noexcept {
        return sizeof(Page) + binary.capacity() + slots.capacity() * sizeof(Slot);
    }
    static Page decode(Bytes bytes) {
        require(bytes.size() >= 11 && bytes.size() <= max_page_bytes);
        wire::Reader reader(bytes);
        wire::Field field;
        uint64_t type = 0, declared_maximum = 0;
        std::vector<Slot> values;
        unsigned seen = 0;
        while (reader.next(field)) {
            if (field.number == 1) {
                require(field.type == wire::Type::Varint && reader.varint(type));
                seen |= 1;
            } else if (field.number == 2) {
                require(field.type == wire::Type::Fixed64 && reader.fixed(declared_maximum, 8));
                seen |= 2;
            } else if (field.number == 3) {
                std::string_view message;
                const auto start = reader.position() - 1;
                require(field.type == wire::Type::Bytes && reader.bytes(message));
                Slot slot;
                slot.start = start;
                slot.length = reader.position() - start;
                wire::Reader item(message);
                wire::Field part;
                unsigned fields = 0;
                while (item.next(part)) {
                    if (part.number == 1) {
                        std::string_view key;
                        require(part.type == wire::Type::Bytes && item.bytes(key) &&
                                key.size() <= max_internal_key_bytes);
                        slot.key = key;
                        fields |= 1;
                    } else if (part.number >= 2 && part.number <= 11) {
                        uint64_t value = 0;
                        auto width =
                            part.number == 5 || part.number == 9 || part.number == 10 ? 4u : 8u;
                        require(part.type ==
                                    (width == 4 ? wire::Type::Fixed32 : wire::Type::Fixed64) &&
                                item.fixed(value, width));
                        switch (part.number) {
                        case 2:
                            slot.value.version = value;
                            break;
                        case 3:
                            slot.value.offset = value;
                            break;
                        case 4:
                            slot.value.size = value;
                            break;
                        case 5:
                            slot.value.crc = static_cast<uint32_t>(value);
                            break;
                        case 6:
                            slot.value.expires = value;
                            break;
                        case 7:
                            slot.child = value;
                            break;
                        case 8:
                            slot.maximum = value;
                            break;
                        case 9:
                            slot.value.depth = static_cast<uint32_t>(value);
                            break;
                        case 10:
                            slot.value.recipe_size = static_cast<uint32_t>(value);
                            break;
                        case 11:
                            slot.earliest = value;
                            break;
                        }
                        fields |= 1u << (part.number - 1);
                    } else
                        require(item.skip(part));
                }
                require(item.done() && fields &&
                        (values.empty() || compare(values.back().key, slot.key) < 0));
                values.push_back(std::move(slot));
            } else
                require(reader.skip(field));
        }
        require(reader.done() && seen == 3 && (type == 1 || type == 2) && !values.empty());
        Page result(type == 1);
        result.slots = std::move(values);
        uint64_t maximum = 0, earliest = UINT64_MAX;
        for (auto &slot : result.slots) {
            require(result.leaf
                        ? slot.value.valid()
                        : slot.child >= 12288 && slot.child < INT64_MAX && slot.maximum != 0);
            maximum = std::max(maximum, result.leaf ? slot.value.version : slot.maximum);
            earliest =
                std::min(earliest, result.leaf ? slot.value.earliest_expiry() : slot.earliest);
        }
        require(maximum == declared_maximum);
        // Internal pages are canonical. This also validates recorded edit spans
        // before any future subtraction/overwrite of the cached binary.
        wire::Writer header;
        header.uint_field(1, type);
        header.fixed64_field(2, declared_maximum);
        require(bytes.compare(0, 11, header.data()) == 0);
        size_t total = 11;
        for (auto &slot : result.slots) {
            auto encoded = encode(slot, result.leaf);
            require(bytes.compare(slot.start, slot.length, encoded) == 0);
            slot.key_offset = slot.start + slot.length -
                              (result.leaf ? 51 : 18 + (slot.earliest ? 9 : 0)) - slot.key.size();
            total += slot.length;
        }
        require(total == bytes.size());
        result.binary = std::move(bytes);
        result.rebase();
        result.maximum = maximum;
        result.earliest = earliest;
        return result;
    }
};
} // namespace fengqudb::paged
