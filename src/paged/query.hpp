#pragma once
#include "features.hpp"
#include "projection.hpp"
#include <tuple>

namespace fengqudb::paged {
// One immutable entry per Core. It neither retains a generation nor extends
// transaction budgets, and its complete allocation is charged to pin_budget.
struct QueryRuleCache {
    std::weak_ptr<const Data> source;
    IndexRule rule;
    Charge charge;
    QueryRuleCache(const Value &schema, IndexRule &&parsed, Charge &&allocation) noexcept
        : source(schema), rule(std::move(parsed)), charge(std::move(allocation)) {}
    static uint64_t cost(const IndexRule &rule) noexcept {
        return sizeof(QueryRuleCache) + 128 + rule.name.capacity() + rule.prefix.capacity() +
               rule.parts.capacity() * sizeof(IndexPart);
    }
};
static_assert(std::is_nothrow_move_constructible<IndexRule>::value,
              "Optional rule caching must not consume a rule on allocation failure");
class QueryRule {
    std::shared_ptr<const QueryRuleCache> cached_;
    std::optional<IndexRule> local_;

  public:
    QueryRule(Transaction &tx, const Bytes &key, const Value &schema) {
        auto &core = *tx.base->core;
        // A private schema can be edited in place. Only committed immutable
        // values qualify; even a cache hit still records the schema dependency.
        const bool eligible =
            core.options.cache_bytes && schema->size() <= 4096 && !write_find(tx.writes, key);
        std::shared_ptr<const QueryRuleCache> expected;
        if (eligible) {
            expected = std::atomic_load_explicit(&core.query_rule_cache, std::memory_order_acquire);
            if (expected && expected->source.lock() == schema) {
                cached_ = std::move(expected);
                return;
            }
        }
        local_.emplace(*schema);
        if (!eligible)
            return;
        try {
            // Both reservation and object allocation finish before the
            // noexcept move. Rejection leaves the already-parsed local rule.
            Charge allocation(core.pin_budget, {}, QueryRuleCache::cost(*local_));
            cached_ =
                std::make_shared<QueryRuleCache>(schema, std::move(*local_), std::move(allocation));
        } catch (...) {
            return;
        }
        // A losing candidate is still useful for this query and releases its
        // charge on return. Never race on the writer-only RulesCache.
        std::atomic_compare_exchange_strong_explicit(&core.query_rule_cache, &expected, cached_,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
    }
    const IndexRule &get() const noexcept {
        return cached_ ? cached_->rule : *local_;
    }
};
// Query value envelope: 1 equality prefix protobuf, 2 lower-bound protobuf,
// 3 upper-bound protobuf, 4 flags, repeated 5 projection field numbers.
struct IndexQuery {
    std::string_view equal;
    std::optional<std::string_view> lower, upper;
    uint64_t flags = 0;
    std::vector<uint32_t> projection;
    explicit IndexQuery(std::string_view bytes) {
        wire::Reader reader(bytes);
        wire::Field field;
        unsigned seen = 0;
        while (reader.next(field)) {
            require(field.number >= 1 && field.number <= 5, FQDB_INVALID_ARGUMENT);
            if (field.number <= 4) {
                require(!(seen & (1u << field.number)), FQDB_INVALID_ARGUMENT);
                seen |= 1u << field.number;
            }
            if (field.number <= 3) {
                std::string_view value;
                require(field.type == wire::Type::Bytes && reader.bytes(value),
                        FQDB_INVALID_ARGUMENT);
                if (field.number == 1)
                    equal = value;
                else
                    (field.number == 2 ? lower : upper) = value;
            } else if (field.number == 5 && field.type == wire::Type::Bytes) {
                std::string_view values;
                require(reader.bytes(values), FQDB_INVALID_ARGUMENT);
                wire::Reader packed(values);
                while (!packed.done()) {
                    uint64_t value = 0;
                    require(packed.varint(value) && value && value <= 0x1fffffff &&
                                projection.size() < 256,
                            FQDB_INVALID_ARGUMENT);
                    projection.push_back(static_cast<uint32_t>(value));
                }
            } else {
                uint64_t value = 0;
                require(field.type == wire::Type::Varint && reader.varint(value),
                        FQDB_INVALID_ARGUMENT);
                if (field.number == 4) {
                    require(value <= 15, FQDB_INVALID_ARGUMENT);
                    flags = value;
                } else {
                    require(value && value <= 0x1fffffff && projection.size() < 256,
                            FQDB_INVALID_ARGUMENT);
                    projection.push_back(static_cast<uint32_t>(value));
                }
            }
        }
        require(reader.done(), FQDB_INVALID_ARGUMENT);
        std::sort(projection.begin(), projection.end());
        projection.erase(std::unique(projection.begin(), projection.end()), projection.end());
    }
};
inline std::pair<Bytes, Bytes> query_bounds(const IndexRule &rule, const IndexQuery &query) {
    auto values = indexed_values(rule, query.equal, false);
    Bytes prefix = index_root(rule);
    size_t equal = 0;
    while (equal < rule.count() && values[equal])
        prefix += component(*values[equal++]);
    for (size_t i = equal; i < rule.count(); ++i)
        require(!values[i], FQDB_INVALID_ARGUMENT);
    require(prefix.size() <= max_internal_key_bytes, FQDB_LIMIT);
    auto low = prefix, high = prefix_end(prefix);
    if (query.lower || query.upper)
        require(equal < rule.count(), FQDB_INVALID_ARGUMENT);
    auto bound = [&](std::string_view encoded) {
        auto parts = indexed_values(rule, encoded, false);
        require(bool(parts[equal]), FQDB_INVALID_ARGUMENT);
        for (size_t i = 0; i < rule.count(); ++i)
            require(i == equal || !parts[i], FQDB_INVALID_ARGUMENT);
        return prefix + component(*parts[equal]);
    };
    std::optional<Bytes> lower, upper;
    if (query.lower)
        lower = bound(*query.lower);
    if (query.upper)
        upper = bound(*query.upper);
    if (lower && upper)
        require(compare(*lower, *upper) <= 0, FQDB_INVALID_ARGUMENT);
    if (query.lower) {
        low = *lower;
        if (query.flags & 1)
            low = prefix_end(low);
    }
    if (query.upper) {
        high = *upper;
        if (query.flags & 2)
            high = prefix_end(high);
    }
    if (lower && upper && *lower == *upper && compare(low, high) > 0)
        high = low;
    require(compare(low, high) <= 0, FQDB_INVALID_ARGUMENT);
    return {std::move(low), std::move(high)};
}
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline FeatureResult
query_index(Transaction &tx, const FeatureRequest &request, Charge &charge) {
    tx.ready();
    enable_features(tx);
    FeatureResult result;
    charge = Charge(tx.budget, tx.base->core->transaction_budget, 0);
    uint64_t retained = 0;
    IndexQuery query(request.operation == FQDB_INDEX_LOOKUP ? std::string_view{} : request.value);
    if (request.operation == FQDB_RECORD_PROJECT) {
        require(request.scope.empty() && !query.lower && !query.upper && query.equal.empty() &&
                    !query.flags,
                FQDB_INVALID_ARGUMENT);
        require(!system_key(request.key), FQDB_INVALID_ARGUMENT);
        auto value = tx.get(Bytes(request.key));
        result.value = project_fields(*value, query.projection, charge, retained);
        return result;
    }
    auto schema_key = catalog_prefix() + component(request.scope);
    auto schema = maybe_get(tx, schema_key);
    require(bool(schema), FQDB_NOT_FOUND);
    QueryRule selected(tx, schema_key, *schema);
    const auto &rule = selected.get();
    require(rule.ready, FQDB_LOCKED);
    Bytes low, high, legacy_prefix;
    if (request.operation == FQDB_INDEX_LOOKUP) {
        auto field = indexed_field(rule, request.value);
        require(bool(field), FQDB_INVALID_ARGUMENT);
        low = legacy_prefix = index_prefix(rule, *field);
        high = prefix_end(low);
    } else
        std::tie(low, high) = query_bounds(rule, query);
    const bool reverse = (query.flags & 4) != 0;
    Bytes after = request.key.empty()                      ? Bytes{}
                  : request.operation == FQDB_INDEX_LOOKUP ? legacy_prefix + Bytes(request.key)
                                                           : Bytes(request.key);
    if (!after.empty())
        require(compare(after, low) >= 0 && compare(after, high) < 0, FQDB_INVALID_ARGUMENT);

    // Build an ephemeral overlay only when this transaction changed a source
    // record. It shares persistent roots/budgets and never alters caller writes
    // or the writer-only schema cache. A second query starts from fresh writes.
    std::unique_ptr<Transaction> overlay;
    WriteCursor changed(tx.writes);
    while (auto item = changed.next()) {
        if (!system_key(item->key.view()) &&
            item->key.view().substr(0, rule.prefix.size()) == rule.prefix) {
            overlay = std::make_unique<Transaction>(tx.base, 0);
            overlay->budget = tx.budget;
            overlay->writes = tx.writes;
            overlay->advanced = true;
            overlay->has_deadline = tx.has_deadline;
            overlay->deadline = tx.deadline;
            prepare_index_rule(*overlay, rule, tx.writes);
            break;
        }
    }
    auto &read = overlay ? *overlay : tx;
    auto dependencies = [&] {
        if (!overlay)
            return;
        require(tx.ranges.size() + overlay->ranges.size() <= 1024, FQDB_LIMIT);
        tx.ranges.reserve(tx.ranges.size() + overlay->ranges.size());
        tx.reads.merge(overlay->reads);
        for (auto &range : overlay->ranges)
            tx.ranges.push_back(std::move(range));
    };
    // Preserve the single point-read path for the existing unique equality API.
    if (request.operation == FQDB_INDEX_LOOKUP && (rule.flags & 1)) {
        auto owner = maybe_get(read, low);
        if (owner) {
            charge.resize((*owner)->size() + sizeof(FeatureRow) + 192);
            result.rows.push_back({Bytes(**owner), {}, 0});
            result.count = 1;
        }
        dependencies();
        return result;
    }
    read.range(low, high);
    auto begin = !reverse && !after.empty() ? after : low;
    auto end = reverse && !after.empty() ? after : high;
    ReadCursor cursor(read.base, begin, end, reverse, read.writes, true);
    retained = 128;
    charge.resize(retained);
    // A request-local page path reuses immutable source metadata while every
    // source read still records its normal conflict dependency.
    Charge path_charge(tx.base->core->pin_budget, {}, 0);
    std::optional<Table::ReadPath> source_path;
    if (request.limit >= 16 && tx.base->core->options.cache_bytes &&
        ((query.flags & 8) || !query.projection.empty())) {
        try {
            constexpr uint64_t allowance = 65536;
            // Leave room for the next index owner even if its business key
            // is at the public maximum. Near the pin limit use plain lookups.
            const auto available =
                tx.base->core->options.pinned_bytes - tx.base->core->pin_budget->used();
            if (available >= allowance + FQDB_MAX_KEY_BYTES + sizeof(Data)) {
                path_charge.resize(allowance);
                source_path.emplace(allowance);
            }
        } catch (const Error &error) {
            if (error.status != FQDB_LIMIT)
                throw;
        }
    }
    Bytes last;
    while (auto row = cursor.next()) {
        tx.ready();
        std::string_view internal(static_cast<const char *>(row->key.data),
                                  static_cast<size_t>(row->key.size));
        if (!reverse && !after.empty() && compare(internal, after) <= 0)
            continue;
        if (result.rows.size() == result.rows.capacity()) {
            auto capacity = std::min(static_cast<size_t>(request.limit),
                                     std::max<size_t>(4, result.rows.capacity() * 2));
            auto previous = result.rows.capacity();
            charge.resize(retained + (capacity - previous) * sizeof(FeatureRow));
            result.rows.reserve(capacity);
            retained += (result.rows.capacity() - previous) * sizeof(FeatureRow);
            charge.resize(retained);
        }
        retained += row->value.size + 128;
        require(retained <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
        charge.resize(retained);
        FeatureRow item{
            Bytes(static_cast<const char *>(row->value.data), static_cast<size_t>(row->value.size)),
            {},
            0};
        if ((query.flags & 8) || !query.projection.empty()) {
            auto value = read.try_get(item.key, nullptr, source_path ? &*source_path : nullptr);
            require(bool(value), FQDB_NOT_FOUND);
            if (!query.projection.empty())
                item.value = project_fields(*value, query.projection, charge, retained);
            else {
                retained += value->size() + 64;
                require(retained <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
                charge.resize(retained);
                item.value = std::move(value);
            }
        }
        last.assign(internal);
        result.rows.push_back(std::move(item));
        if (result.rows.size() == request.limit)
            break;
    }
    if (result.rows.size() == request.limit &&
        !(request.operation == FQDB_INDEX_LOOKUP && (rule.flags & 1)))
        result.cursor =
            request.operation == FQDB_INDEX_LOOKUP ? last.substr(legacy_prefix.size()) : last;
    result.count = result.rows.size();
    dependencies();
    return result;
}
} // namespace fengqudb::paged
