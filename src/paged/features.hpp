#pragma once
#include "index_schema.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <variant>

namespace fengqudb::paged {
inline std::optional<Value> maybe_get(Transaction &tx, const Bytes &key) {
    auto value = tx.try_get(key);
    return value ? std::optional<Value>(std::move(value)) : std::nullopt;
}
inline void enable_features(Transaction &tx) {
    if (tx.advanced)
        return;
    if (!(tx.base->view->checkpoint.features & 1)) {
        const auto &prefix = system_prefix();
        auto end = prefix_end(prefix);
        ViewCursor cursor(*tx.base->view, prefix);
        while (auto row = cursor.next()) {
            if (compare(row->key, end) >= 0)
                break;
            require(row->value.deleted(), FQDB_CONSTRAINT);
        }
        tx.range(prefix, end);
    }
    tx.advanced = true;
}
using FeatureBytes = std::variant<Bytes, Value>;
inline std::string_view feature_bytes(const FeatureBytes &value) {
    if (auto owned = std::get_if<Bytes>(&value))
        return *owned;
    const auto &borrowed = std::get<Value>(value);
    return borrowed ? std::string_view(*borrowed) : std::string_view{};
}
struct FeatureRow {
    Bytes key;
    FeatureBytes value;
    double score = 0;
};
struct FeatureResult {
    // Borrow immutable native values through encoding; owned bytes are needed
    // only for computed results such as field projection.
    std::variant<std::monostate, Bytes, Value> value;
    Bytes cursor;
    uint64_t count = 0;
    bool changed = false;
    std::vector<FeatureRow> rows;
    fqdb_buffer encode() const {
        std::optional<std::string_view> scalar;
        if (auto owned = std::get_if<Bytes>(&value))
            scalar = *owned;
        else if (auto borrowed = std::get_if<Value>(&value); borrowed && *borrowed)
            scalar = **borrowed;
        auto row_size = [](const FeatureRow &row) {
            return wire::bytes_size(1, row.key.size()) +
                   wire::bytes_size(2, feature_bytes(row.value).size()) + 9;
        };
        size_t size = 11; // fixed64 count and boolean changed, including tags
        if (scalar)
            size += wire::bytes_size(1, scalar->size());
        for (const auto &row : rows) {
            size += wire::bytes_size(4, row_size(row));
            require(size <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
        }
        if (!cursor.empty())
            size += wire::bytes_size(5, cursor.size());
        require(size <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
        // Allocate the C-owned response once. No temporary encoded string or
        // second full-result copy is needed at the ABI boundary.
        auto memory = static_cast<unsigned char *>(std::malloc(size));
        require(memory != nullptr, FQDB_NO_MEMORY);
        auto output = memory;
        auto varint = [&](uint64_t number) noexcept {
            while (number >= 128) {
                *output++ = static_cast<unsigned char>((number & 127) | 128);
                number >>= 7;
            }
            *output++ = static_cast<unsigned char>(number);
        };
        auto bytes = [&](unsigned field, std::string_view value) noexcept {
            *output++ = static_cast<unsigned char>((field << 3) | 2);
            varint(value.size());
            if (!value.empty()) {
                std::memcpy(output, value.data(), value.size());
                output += value.size();
            }
        };
        auto fixed = [&](unsigned field, uint64_t number) noexcept {
            *output++ = static_cast<unsigned char>((field << 3) | 1);
            for (unsigned i = 0; i < 8; ++i)
                *output++ = static_cast<unsigned char>(number >> (8 * i));
        };
        if (scalar)
            bytes(1, *scalar);
        fixed(2, count);
        *output++ = 24;
        *output++ = changed ? 1 : 0;
        for (const auto &row : rows) {
            *output++ = 34;
            varint(row_size(row));
            bytes(1, row.key);
            bytes(2, feature_bytes(row.value));
            uint64_t bits;
            std::memcpy(&bits, &row.score, 8);
            fixed(3, bits);
        }
        if (!cursor.empty())
            bytes(5, cursor);
        return {memory, size};
    }
};
struct FeatureRequest {
    uint64_t operation = 0, argument = 0, limit = 256, flags = 0;
    double score = 0;
    // Borrow only for execute_feature(); staging owns all data that escapes it.
    std::string_view scope, key, value;
    explicit FeatureRequest(std::string_view bytes) {
        wire::Reader reader(bytes);
        wire::Field field;
        uint32_t seen = 0;
        while (reader.next(field)) {
            if (field.number <= 8) {
                require(!(seen & (1u << field.number)), FQDB_INVALID_ARGUMENT);
                seen |= 1u << field.number;
            }
            if (field.number == 1 || field.number == 8) {
                uint64_t v = 0;
                require(field.type == wire::Type::Varint && reader.varint(v),
                        FQDB_INVALID_ARGUMENT);
                (field.number == 1 ? operation : flags) = v;
            } else if (field.number >= 2 && field.number <= 4) {
                std::string_view v;
                require(field.type == wire::Type::Bytes && reader.bytes(v), FQDB_INVALID_ARGUMENT);
                (field.number == 2 ? scope : field.number == 3 ? key : value) = v;
            } else if (field.number >= 5 && field.number <= 7) {
                uint64_t v = 0;
                require(field.type == wire::Type::Fixed64 && reader.fixed(v, 8),
                        FQDB_INVALID_ARGUMENT);
                if (field.number == 5)
                    argument = v;
                else if (field.number == 6)
                    std::memcpy(&score, &v, 8);
                else
                    limit = v;
            } else
                require(reader.skip(field), FQDB_INVALID_ARGUMENT);
        }
        const bool cursor = operation == FQDB_HASH_SCAN || operation == FQDB_SET_SCAN ||
                            operation == FQDB_SORTED_RANGE || operation == FQDB_INDEX_LOOKUP ||
                            operation == FQDB_INDEX_QUERY;
        require(reader.done() && operation && scope.size() <= 256 &&
                    key.size() <= (cursor ? max_internal_key_bytes : FQDB_MAX_KEY_BYTES) &&
                    value.size() <= FQDB_MAX_VALUE_BYTES && limit && limit <= 4096 && flags == 0,
                FQDB_INVALID_ARGUMENT);
    }
};
inline std::vector<FeatureRow> feature_scan(Transaction &tx, const Bytes &prefix, uint64_t limit,
                                            const Bytes &after = {}) {
    auto end = prefix_end(prefix);
    tx.range(prefix, end);
    ReadCursor cursor(tx.base, after.empty() ? prefix : after, end, false, tx.writes, true);
    std::vector<FeatureRow> rows;
    uint64_t bytes = 0;
    while (auto row = cursor.next()) {
        if (!after.empty() && compare(std::string_view(static_cast<const char *>(row->key.data),
                                                       static_cast<size_t>(row->key.size)),
                                      after) <= 0)
            continue;
        bytes += row->key.size + row->value.size + 128;
        require(bytes <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
        rows.push_back(
            {Bytes(static_cast<const char *>(row->key.data), static_cast<size_t>(row->key.size)),
             Bytes(static_cast<const char *>(row->value.data),
                   static_cast<size_t>(row->value.size))});
        if (rows.size() == limit)
            break;
    }
    return rows;
}
struct CollectionMeta {
    uint64_t kind = 0, count = 0, head = UINT64_C(1) << 63, tail = UINT64_C(1) << 63;
    Bytes encode() const {
        wire::Writer out;
        out.fixed64_field(1, kind);
        out.fixed64_field(2, count);
        out.fixed64_field(3, head);
        out.fixed64_field(4, tail);
        return out.take();
    }
    explicit CollectionMeta(uint64_t k) : kind(k) {}
    explicit CollectionMeta(std::string_view bytes) {
        wire::Reader reader(bytes);
        wire::Field field;
        unsigned seen = 0;
        while (reader.next(field)) {
            require(field.number >= 1 && field.number <= 4 && field.type == wire::Type::Fixed64 &&
                    !(seen & (1u << field.number)));
            uint64_t value = 0;
            require(reader.fixed(value, 8));
            seen |= 1u << field.number;
            (field.number == 1   ? kind
             : field.number == 2 ? count
             : field.number == 3 ? head
                                 : tail) = value;
        }
        require(reader.done() && seen == 30 && kind >= 1 && kind <= 4 && count <= INT64_MAX &&
                (kind != 3 || (tail >= head && tail - head == count)));
    }
};
inline Bytes catalog_prefix() {
    return system_prefix() + "D";
}
inline Bytes catalog_epoch() {
    return system_prefix() + "d";
}
inline void index_one(Transaction &tx, const IndexRule &rule, const Bytes &key,
                      std::optional<std::string_view> old_value, uint64_t old_expiry,
                      std::optional<std::string_view> new_value, uint64_t new_expiry) {
    auto previous = old_value ? indexed_field(rule, *old_value, false) : std::optional<Bytes>{};
    auto next = new_value ? indexed_field(rule, *new_value) : std::optional<Bytes>{};
    if (previous == next && old_expiry == new_expiry)
        return;
    if (previous) {
        auto entry = index_prefix(rule, *previous) + ((rule.flags & 1) ? Bytes{} : component(key));
        auto owner = maybe_get(tx, entry);
        if (owner && **owner == key)
            tx.erase(entry);
    }
    if (next) {
        auto entry = index_prefix(rule, *next) + ((rule.flags & 1) ? Bytes{} : component(key));
        if (rule.flags & 1) {
            auto owner = maybe_get(tx, entry);
            require(!owner || **owner == key, FQDB_CONSTRAINT);
        }
        tx.put(entry, key, new_expiry);
    }
}
struct RulesCache {
    uint64_t epoch;
    std::vector<IndexRule> rules;
    Charge charge;
    RulesCache(Core &core, uint64_t version, std::vector<IndexRule> values)
        : epoch(version), rules(std::move(values)), charge(core.pin_budget, {}, 0) {
        uint64_t bytes = sizeof(RulesCache) + rules.capacity() * sizeof(IndexRule) + 128;
        for (const auto &rule : rules)
            bytes += rule.name.capacity() + rule.prefix.capacity() +
                     rule.parts.capacity() * sizeof(IndexPart) + 128;
        charge.resize(bytes);
    }
};
inline void prepare_index_rule(Transaction &tx, const IndexRule &rule, const WriteRoot &original) {
    struct Change {
        Bytes key;
        std::optional<Bytes> old_field, field;
        uint64_t expiry;
    };
    std::vector<Change> changes;
    Charge charge(tx.budget, tx.base->core->transaction_budget, 0);
    uint64_t retained = 0;
    WriteCursor cursor(original);
    while (auto node = cursor.next()) {
        auto key = node->key.view();
        if (system_key(key) || key.substr(0, rule.prefix.size()) != rule.prefix)
            continue;
        auto old = tx.base->view->find(key);
        Value old_bytes, next_bytes;
        std::optional<Bytes> previous, next;
        if (old && old->visible(tx.base->time)) {
            old_bytes = tx.base->view->generation->value(*old, tx.base->view->limit);
            try {
                previous = indexed_field(rule, *old_bytes, false);
            } catch (const Error &error) {
                if (error.status != FQDB_CONSTRAINT)
                    throw;
            }
        }
        if (node->value.kind != OpKind::erase) {
            next_bytes = node->value.value ? Value(node->value.value)
                                           : tx.base->view->generation->value(node->value.original,
                                                                              tx.base->view->limit);
            next = indexed_field(rule, *next_bytes);
        }
        if ((!previous && !next) ||
            (previous == next && (old ? old->expires : 0) == node->value.expires))
            continue;
        retained += key.size() + (previous ? previous->size() : 0) + (next ? next->size() : 0) +
                    sizeof(Change) + 192;
        charge.resize(retained);
        changes.push_back({Bytes(key), std::move(previous), std::move(next), node->value.expires});
    }
    // Remove the whole transaction's old owners first, permitting atomic
    // unique-value swaps without exposing an intermediate state.
    for (const auto &change : changes)
        if (change.old_field) {
            auto entry = index_prefix(rule, *change.old_field) +
                         ((rule.flags & 1) ? Bytes{} : component(change.key));
            auto owner = maybe_get(tx, entry);
            if (owner && **owner == change.key)
                tx.erase(entry);
        }
    for (const auto &change : changes)
        if (change.field) {
            auto entry = index_prefix(rule, *change.field) +
                         ((rule.flags & 1) ? Bytes{} : component(change.key));
            if (rule.flags & 1) {
                auto owner = maybe_get(tx, entry);
                require(!owner || **owner == change.key, FQDB_CONSTRAINT);
            }
            tx.put(entry, change.key, change.expiry);
        }
}
inline void features_prepare(Transaction &tx, const View &current) {
    bool user_writes = false;
    WriteCursor scan(tx.writes);
    while (auto node = scan.next())
        if (!system_key(node->key.view())) {
            user_writes = true;
            break;
        }
    if (!user_writes)
        return;
    auto epoch_key = catalog_epoch();
    tx.depend(epoch_key);
    auto epoch = tx.base->view->find(epoch_key), latest = current.find(epoch_key);
    require((epoch ? epoch->version : 0) == (latest ? latest->version : 0), FQDB_CONFLICT);
    if (!epoch || epoch->deleted())
        return;
    auto &core = *tx.base->core;
    if (!core.rules_cache || core.rules_cache->epoch != epoch->version) {
        core.rules_cache.reset();
        std::vector<IndexRule> rules;
        auto prefix = catalog_prefix();
        ReadCursor cursor(tx.base, prefix, prefix_end(prefix), false, {}, true);
        while (auto row = cursor.next()) {
            require(rules.size() < 64, FQDB_LIMIT);
            rules.emplace_back(std::string_view(static_cast<const char *>(row->value.data),
                                                static_cast<size_t>(row->value.size)));
        }
        core.rules_cache = std::make_shared<RulesCache>(core, epoch->version, std::move(rules));
    }
    auto cache = core.rules_cache;
    auto original = tx.writes;
    for (const auto &rule : cache->rules)
        prepare_index_rule(tx, rule, original);
}

inline FeatureResult execute_feature(Transaction &tx, const FeatureRequest &request) {
    tx.ready();
    enable_features(tx);
    const auto op = request.operation;
    FeatureResult result;
    uint64_t kind = op <= 4                ? 1
                    : op >= 10 && op <= 13 ? 2
                    : op >= 20 && op <= 26 ? 3
                    : op >= 30 && op <= 33 ? 4
                                           : 0;
    require(kind || op == FQDB_COLLECTION_SIZE || op == FQDB_COLLECTION_CLEAR,
            FQDB_INVALID_ARGUMENT);
    auto prefix = system_prefix() + "C" + component(request.scope);
    auto raw = maybe_get(tx, prefix);
    CollectionMeta meta(kind ? kind : 1);
    if (raw)
        meta = CollectionMeta(**raw);
    require(!kind || meta.kind == kind, FQDB_CONSTRAINT);
    result.count = meta.count;
    auto member_prefix = prefix + "M", list_prefix = prefix + "L", sorted_prefix = prefix + "S";
    auto persist = [&] {
        require(meta.count <= INT64_MAX, FQDB_LIMIT);
        if (!meta.count && meta.kind == 3)
            meta.head = meta.tail = UINT64_C(1) << 63;
        tx.put(prefix, meta.encode(), 0);
        result.count = meta.count;
        result.changed = true;
    };
    if (op == FQDB_COLLECTION_SIZE)
        return result;
    if (op == FQDB_COLLECTION_CLEAR) {
        if (!raw)
            return result;
        auto entries =
            feature_scan(tx, meta.kind == 3 ? list_prefix : member_prefix, request.limit);
        for (const auto &row : entries) {
            if (meta.kind == 4)
                tx.erase(sorted_prefix + Bytes(feature_bytes(row.value)) +
                         row.key.substr(member_prefix.size()));
            tx.erase(row.key);
            require(meta.count);
            --meta.count;
            if (meta.kind == 3)
                ++meta.head;
        }
        if (!entries.empty())
            persist();
        return result;
    }
    if (op == FQDB_HASH_SCAN || op == FQDB_SET_SCAN || op == FQDB_SORTED_RANGE) {
        const auto &range = op == FQDB_SORTED_RANGE ? sorted_prefix : member_prefix;
        auto after = request.key.empty() ? Bytes{} : Bytes(range).append(request.key);
        auto end = prefix_end(range);
        tx.range(range, end);
        ReadCursor cursor(tx.base, after.empty() ? range : after, end, false, tx.writes, true);
        result.rows.reserve(static_cast<size_t>(std::min(request.limit, meta.count)));
        uint64_t retained = 0;
        while (auto row = cursor.next()) {
            std::string_view key(static_cast<const char *>(row->key.data),
                                 static_cast<size_t>(row->key.size));
            if (!after.empty() && compare(key, after) <= 0)
                continue;
            retained += row->key.size + row->value.size + 128;
            require(retained <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
            if (op == FQDB_SORTED_RANGE) {
                require(key.size() >= range.size() + 8);
                result.rows.push_back({Bytes(static_cast<const char *>(row->value.data),
                                             static_cast<size_t>(row->value.size)),
                                       {},
                                       decode_score(key.substr(range.size(), 8))});
            } else {
                FeatureRow item;
                item.key = uncomponent(key.substr(range.size()));
                item.value = cursor.value_ref();
                result.rows.push_back(std::move(item));
            }
            if (result.rows.size() == request.limit) {
                result.cursor.assign(key.substr(range.size()));
                break;
            }
        }
        return result;
    }
    if (meta.kind == 3) {
        if (op == FQDB_LIST_PUSH_LEFT || op == FQDB_LIST_PUSH_RIGHT) {
            require(op == FQDB_LIST_PUSH_LEFT ? meta.head != 0 : meta.tail != UINT64_MAX,
                    FQDB_LIMIT);
            auto position = op == FQDB_LIST_PUSH_LEFT ? --meta.head : meta.tail++;
            tx.put(list_prefix + big_endian(position), request.value, 0);
            ++meta.count;
            persist();
            return result;
        }
        if (!meta.count && op == FQDB_LIST_RANGE)
            return result;
        require(meta.count, FQDB_NOT_FOUND);
        auto index = static_cast<int64_t>(request.argument);
        uint64_t position = 0;
        if (op == FQDB_LIST_POP_LEFT)
            position = meta.head;
        else if (op == FQDB_LIST_POP_RIGHT)
            position = meta.tail - 1;
        else {
            require(index >= 0 ? static_cast<uint64_t>(index) < meta.count
                               : index != INT64_MIN && static_cast<uint64_t>(-index) <= meta.count,
                    FQDB_NOT_FOUND);
            position = index >= 0 ? meta.head + static_cast<uint64_t>(index)
                                  : meta.tail - static_cast<uint64_t>(-index);
        }
        if (op == FQDB_LIST_RANGE) {
            auto count = std::min(request.limit, meta.tail - position);
            // Contiguous list positions need one seek, not count point reads.
            // Fall back when the public cursor's pin budget cannot serve this
            // transient internal scan; the old point path has no such pin cost.
            if (count >= 16) {
                auto begin = list_prefix + big_endian(position);
                auto end = list_prefix + big_endian(position + count);
                try {
                    ReadCursor cursor(tx.base, begin, end, false, tx.writes, true);
                    uint64_t bytes = 0;
                    for (uint64_t i = 0; i < count; ++i) {
                        tx.ready();
                        auto row = cursor.next();
                        auto expected = big_endian(position + i);
                        require(bool(row));
                        std::string_view key(static_cast<const char *>(row->key.data),
                                             static_cast<size_t>(row->key.size));
                        require(key.size() == list_prefix.size() + expected.size() &&
                                key.substr(0, list_prefix.size()) == list_prefix &&
                                key.substr(list_prefix.size()) == expected);
                        bytes += row->value.size + 128;
                        require(bytes <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
                        result.rows.push_back({std::move(expected), cursor.value_ref(), 0});
                    }
                    tx.range(std::move(begin), std::move(end));
                    return result;
                } catch (const Error &error) {
                    if (error.status != FQDB_LIMIT)
                        throw;
                    result.rows.clear();
                }
            }
            uint64_t retained = 0;
            for (uint64_t i = 0; i < count; ++i) {
                auto value = maybe_get(tx, list_prefix + big_endian(position + i));
                require(bool(value));
                retained += (**value).size() + 128;
                require(retained <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
                result.rows.push_back({big_endian(position + i), *value, 0});
            }
            return result;
        }
        auto key = list_prefix + big_endian(position);
        auto old = maybe_get(tx, key);
        require(bool(old));
        if (op == FQDB_LIST_SET) {
            if (**old != request.value) {
                tx.put(key, request.value, 0);
                result.changed = true;
            }
        } else {
            result.value = *old;
            if (op == FQDB_LIST_POP_LEFT || op == FQDB_LIST_POP_RIGHT) {
                tx.erase(key);
                --meta.count;
                if (op == FQDB_LIST_POP_LEFT)
                    ++meta.head;
                else
                    --meta.tail;
                persist();
            }
        }
        return result;
    }
    auto member = member_prefix + component(request.key);
    auto old = maybe_get(tx, member);
    if (op == FQDB_HASH_GET) {
        require(bool(old), FQDB_NOT_FOUND);
        result.value = *old;
    } else if (op == FQDB_SET_CONTAINS)
        result.changed = bool(old);
    else if (op == FQDB_HASH_SET || op == FQDB_SET_ADD) {
        auto value = op == FQDB_HASH_SET ? request.value : std::string_view{};
        if (!old || **old != value) {
            tx.put(member, value, 0);
            result.changed = true;
            if (!old) {
                ++meta.count;
                persist();
            }
        }
    } else if (op == FQDB_HASH_DELETE || op == FQDB_SET_REMOVE || op == FQDB_SORTED_REMOVE) {
        if (old) {
            if (meta.kind == 4)
                tx.erase(sorted_prefix + Bytes(**old) + component(request.key));
            tx.erase(member);
            --meta.count;
            persist();
        }
    } else if (op == FQDB_SORTED_ADD) {
        auto score = score_key(request.score);
        if (!old || **old != score) {
            if (old)
                tx.erase(sorted_prefix + Bytes(**old) + component(request.key));
            tx.put(member, score, 0);
            tx.put(sorted_prefix + score + component(request.key), request.key, 0);
            result.changed = true;
            if (!old) {
                ++meta.count;
                persist();
            }
        }
    } else if (op == FQDB_SORTED_SCORE) {
        require(bool(old), FQDB_NOT_FOUND);
        result.rows.push_back({Bytes(request.key), {}, decode_score(**old)});
    } else
        throw Error(FQDB_INVALID_ARGUMENT);
    return result;
}
inline size_t component_length(std::string_view bytes) {
    for (size_t i = 0; i < bytes.size(); ++i)
        if (!bytes[i]) {
            require(i + 1 < bytes.size());
            if (!bytes[i + 1])
                return i + 2;
            require(static_cast<unsigned char>(bytes[++i]) == 255);
        }
    throw Error(FQDB_CORRUPT);
}
inline void features_verify(const View &view, uint64_t time, const std::function<void()> &check) {
    if (!(view.checkpoint.features & 1))
        return;
    auto get = [&](std::string_view key) -> std::optional<Value> {
        auto ref = view.find(key);
        if (!ref || !ref->visible(time))
            return {};
        return view.generation->value(*ref, view.limit, false, false);
    };
    auto rows = [&](const Bytes &prefix, const std::function<void(const Metadata &)> &consume) {
        ViewCursor cursor(view, prefix);
        while (auto row = cursor.next()) {
            check();
            if (row->key.substr(0, prefix.size()) != prefix)
                break;
            if (row->value.visible(time))
                consume(*row);
        }
    };
    // Validate every published ready rule in both directions: each live source
    // has an owner entry, and every live entry still names that exact field.
    rows(catalog_prefix(), [&](const Metadata &metadata) {
        auto bytes = get(metadata.key);
        require(bool(bytes));
        IndexRule rule(**bytes);
        require(metadata.key == catalog_prefix() + component(rule.name));
        if (!rule.ready)
            return;
        rows(rule.prefix, [&](const Metadata &row) {
            if (system_key(row.key))
                return;
            auto value = get(row.key);
            auto field = indexed_field(rule, **value);
            if (field) {
                auto key =
                    index_prefix(rule, *field) + ((rule.flags & 1) ? Bytes{} : component(row.key));
                auto owner = get(key);
                require(owner && **owner == row.key);
            }
        });
        auto prefix = index_root(rule);
        rows(prefix, [&](const Metadata &row) {
            auto owner = get(row.key);
            require(bool(owner));
            auto value = get(**owner);
            require(bool(value));
            require(std::string_view(**owner).substr(0, rule.prefix.size()) == rule.prefix);
            auto field = indexed_field(rule, **value);
            require(bool(field));
            require(row.key ==
                    index_prefix(rule, *field) + ((rule.flags & 1) ? Bytes{} : component(**owner)));
        });
    });
    auto prefix = system_prefix() + "C";
    rows(prefix, [&](const Metadata &row) {
        auto suffix = row.key.substr(prefix.size());
        auto length = component_length(suffix);
        if (length != suffix.size())
            return;
        auto bytes = get(row.key);
        require(bool(bytes));
        CollectionMeta meta(**bytes);
        auto members = Bytes(row.key) + (meta.kind == 3 ? "L" : "M");
        uint64_t count = 0;
        rows(members, [&](const Metadata &member) {
            if (meta.kind == 3) {
                require(member.key == members + big_endian(meta.head + count));
            } else
                (void)uncomponent(member.key.substr(members.size()));
            if (meta.kind == 2)
                require(member.value.size == 0);
            if (meta.kind == 4) {
                auto score = get(member.key);
                require(bool(score));
                (void)decode_score(**score);
                auto sorted = get(Bytes(row.key) + "S" + Bytes(**score) +
                                  Bytes(member.key.substr(members.size())));
                require(sorted && **sorted == uncomponent(member.key.substr(members.size())));
            }
            ++count;
        });
        require(count == meta.count);
        if (meta.kind == 4) {
            uint64_t sorted = 0;
            rows(Bytes(row.key) + "S", [&](const Metadata &) { ++sorted; });
            require(sorted == count);
        }
    });
}
} // namespace fengqudb::paged
