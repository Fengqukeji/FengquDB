#pragma once
#include "feature_keys.hpp"

namespace fengqudb::paged {
struct IndexPart {
    uint32_t field = 0, type = 0;
    bool operator==(const IndexPart &other) const noexcept {
        return field == other.field && type == other.type;
    }
};
struct IndexRule {
    Bytes name, prefix;
    uint64_t field = 0, type = 0, flags = 0, ready = 0;
    std::vector<IndexPart> parts;
    size_t count() const noexcept {
        return parts.empty() ? 1 : parts.size();
    }
    IndexPart part(size_t i) const {
        return parts.empty() ? IndexPart{static_cast<uint32_t>(field), static_cast<uint32_t>(type)}
                             : parts[i];
    }
    bool valid() const {
        if (name.empty() || name.size() > 128 || prefix.size() > 4096 || flags > 3 || ready > 1 ||
            parts.size() > 8)
            return false;
        if (!parts.empty() && (field || type))
            return false;
        if (parts.empty() && (field > 0x1fffffff || type > 11))
            return false;
        for (size_t i = 0; i < count(); ++i) {
            auto value = part(i);
            if (!value.field || value.field > 0x1fffffff || !value.type || value.type > 11)
                return false;
            for (size_t j = 0; j < i; ++j)
                if (part(j).field == value.field)
                    return false;
        }
        return true;
    }
    Bytes encode() const {
        wire::Writer out;
        out.bytes_field(1, name);
        out.bytes_field(2, prefix);
        out.fixed64_field(3, field);
        out.fixed64_field(4, type);
        out.fixed64_field(5, flags);
        out.fixed64_field(6, ready);
        for (auto value : parts) {
            wire::Writer item;
            item.uint_field(1, value.field);
            item.uint_field(2, value.type);
            out.bytes_field(7, item.data());
        }
        return out.take();
    }
    IndexRule() = default;
    explicit IndexRule(std::string_view bytes) {
        wire::Reader reader(bytes);
        wire::Field part;
        while (reader.next(part)) {
            if (part.number == 1 || part.number == 2) {
                std::string_view v;
                require(part.type == wire::Type::Bytes && reader.bytes(v));
                (part.number == 1 ? name : prefix).assign(v);
            } else if (part.number >= 3 && part.number <= 6) {
                uint64_t v = 0;
                require(part.type == wire::Type::Fixed64 && reader.fixed(v, 8));
                (part.number == 3   ? field
                 : part.number == 4 ? type
                 : part.number == 5 ? flags
                                    : ready) = v;
            } else if (part.number == 7) {
                std::string_view v;
                require(parts.size() < 8 && part.type == wire::Type::Bytes && reader.bytes(v));
                wire::Reader item(v);
                wire::Field tag;
                uint64_t number = 0, kind = 0;
                unsigned seen = 0;
                while (item.next(tag)) {
                    uint64_t data = 0;
                    require(tag.number >= 1 && tag.number <= 2 && !(seen & (1u << tag.number)) &&
                            tag.type == wire::Type::Varint && item.varint(data));
                    seen |= 1u << tag.number;
                    (tag.number == 1 ? number : kind) = data;
                }
                require(item.done() && seen == 6 && number <= UINT32_MAX && kind <= UINT32_MAX);
                parts.push_back({static_cast<uint32_t>(number), static_cast<uint32_t>(kind)});
            } else
                require(reader.skip(part));
        }
        require(reader.done() && valid());
    }
};
inline Bytes scalar_index_value(wire::Reader &reader, wire::Field field, uint32_t type) {
    uint64_t value = 0;
    if (type == 4) {
        std::string_view text;
        require(field.type == wire::Type::Bytes && reader.bytes(text) &&
                    text.size() <= FQDB_MAX_KEY_BYTES,
                FQDB_CONSTRAINT);
        return Bytes(text);
    }
    auto kind = (type == 1 || type == 2 || type == 5 || type == 11) ? wire::Type::Varint
                : (type == 6 || type == 9 || type == 10)            ? wire::Type::Fixed32
                                                                    : wire::Type::Fixed64;
    require(field.type == kind && (kind == wire::Type::Varint
                                       ? reader.varint(value)
                                       : reader.fixed(value, kind == wire::Type::Fixed32 ? 4 : 8)),
            FQDB_CONSTRAINT);
    if (type == 3 || type == 6) {
        double score;
        if (type == 3)
            std::memcpy(&score, &value, 8);
        else {
            uint32_t bits = static_cast<uint32_t>(value);
            float number;
            std::memcpy(&number, &bits, 4);
            score = number;
        }
        require(std::isfinite(score), FQDB_CONSTRAINT);
        return score_key(score);
    }
    if (type == 5)
        value = static_cast<uint64_t>(wire::unzigzag(value));
    if (type == 11)
        require(value <= 1, FQDB_CONSTRAINT);
    if (type == 10) {
        uint32_t bits = static_cast<uint32_t>(value);
        int32_t number;
        std::memcpy(&number, &bits, 4);
        value = static_cast<uint64_t>(static_cast<int64_t>(number));
    }
    if (type == 2 || type == 5 || type == 8 || type == 10)
        value ^= UINT64_C(1) << 63;
    return big_endian(value);
}
using IndexValues = std::array<std::optional<Bytes>, 8>;
inline IndexValues indexed_values(const IndexRule &rule, std::string_view bytes,
                                  bool required = true) {
    wire::Reader reader(bytes);
    wire::Field field;
    IndexValues result;
    while (reader.next(field)) {
        size_t i = 0;
        while (i < rule.count() && rule.part(i).field != field.number)
            ++i;
        if (i == rule.count())
            require(reader.skip(field), FQDB_CONSTRAINT);
        else
            result[i] = scalar_index_value(reader, field, rule.part(i).type);
    }
    require(reader.done(), FQDB_CONSTRAINT);
    if (required && (rule.flags & 2))
        for (size_t i = 0; i < rule.count(); ++i)
            require(bool(result[i]), FQDB_CONSTRAINT);
    return result;
}
inline Bytes index_root(const IndexRule &rule) {
    return system_prefix() + (rule.parts.empty() ? "I" : "J") + component(rule.name);
}
inline std::optional<Bytes> indexed_field(const IndexRule &rule, std::string_view bytes,
                                          bool required = true) {
    auto values = indexed_values(rule, bytes, required);
    Bytes out;
    for (size_t i = 0; i < rule.count(); ++i) {
        if (!values[i])
            return {};
        if (rule.parts.empty())
            return std::move(values[i]);
        out += component(*values[i]);
        require(out.size() <= FQDB_MAX_KEY_BYTES, FQDB_CONSTRAINT);
    }
    return out;
}
inline Bytes index_prefix(const IndexRule &rule, std::string_view field) {
    return index_root(rule) + (rule.parts.empty() ? component(field) : Bytes(field));
}
} // namespace fengqudb::paged
