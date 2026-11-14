#ifndef FENGQUDB_INDEX_HPP
#define FENGQUDB_INDEX_HPP
// Caller-side C++ convenience layer. Only the C ABI crosses library boundaries.
#include "fengqudb_index.h"
#include "fengqudb_store.hpp"
#include <map>

namespace fengqudb::index {
class Value {
    std::string wire_;
    explicit Value(std::string bytes) : wire_(std::move(bytes)) {}

  public:
    Value() = default; // null
    const std::string &encode() const {
        return wire_;
    }
    static Value text(std::string_view value) {
        return data(1, value);
    }
    static Value bytes(std::string_view value) {
        return data(6, value);
    }
    static Value integer(int64_t value) {
        wire::Writer w;
        w.uint_field(1, 2);
        w.sint_field(3, value);
        return Value(w.take());
    }
    static Value unsigned_integer(uint64_t value) {
        wire::Writer w;
        w.uint_field(1, 3);
        w.uint_field(4, value);
        return Value(w.take());
    }
    static Value real(double value) {
        wire::Writer w;
        w.uint_field(1, 4);
        w.double_field(5, value);
        return Value(w.take());
    }
    static Value boolean(bool value) {
        wire::Writer w;
        w.uint_field(1, 5);
        w.uint_field(6, value);
        return Value(w.take());
    }

  private:
    static Value data(unsigned kind, std::string_view value) {
        wire::Writer w;
        w.uint_field(1, kind);
        w.bytes_field(2, value);
        return Value(w.take());
    }
};
class Filter {
    std::string wire_;
    explicit Filter(std::string bytes) : wire_(std::move(bytes)) {}

  public:
    Filter() = default; // true
    enum Compare {
        Equal = 1,
        NotEqual = 2,
        Less = 3,
        LessEqual = 4,
        Greater = 5,
        GreaterEqual = 6
    };
    const std::string &encode() const {
        return wire_;
    }
    static Filter compare(std::string_view field, Compare operation, const Value &value) {
        wire::Writer w;
        w.uint_field(1, operation);
        w.bytes_field(2, field);
        w.bytes_field(3, value.encode());
        return Filter(w.take());
    }
    static Filter eq(std::string_view field, const Value &value) {
        return compare(field, Equal, value);
    }
    static Filter in(std::string_view field, const std::vector<Value> &values) {
        wire::Writer w;
        w.uint_field(1, 7);
        w.bytes_field(2, field);
        for (const auto &v : values)
            w.bytes_field(3, v.encode());
        return Filter(w.take());
    }
    static Filter exists(std::string_view field) {
        wire::Writer w;
        w.uint_field(1, 8);
        w.bytes_field(2, field);
        return Filter(w.take());
    }
    static Filter all(const std::vector<Filter> &children) {
        return combine(9, children);
    }
    static Filter any(const std::vector<Filter> &children) {
        return combine(10, children);
    }
    static Filter negate(const Filter &child) {
        return combine(11, {child});
    }

  private:
    static Filter combine(unsigned op, const std::vector<Filter> &children) {
        wire::Writer w;
        w.uint_field(1, op);
        for (const auto &c : children)
            w.bytes_field(4, c.encode());
        return Filter(w.take());
    }
};
struct TextField {
    std::string name, text;
    double weight = 1;
};
namespace encoding {
inline std::string vector(const std::vector<float> &values) {
    wire::Writer w;
    for (float v : values) {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        w.fixed(bits, 4);
    }
    return w.take();
}
inline void metadata(wire::Writer &w, const std::map<std::string, Value> &values) {
    for (const auto &[name, value] : values) {
        wire::Writer m;
        m.bytes_field(1, name);
        m.bytes_field(2, value.encode());
        w.bytes_field(6, m.data());
    }
}
} // namespace encoding
struct Document {
    std::string id;
    uint64_t projection_version = 1, domain_revision = 0, lifecycle_generation = 1;
    std::vector<TextField> texts;
    std::map<std::string, Value> metadata;
    std::vector<float> vector;
    std::string vector_space, source_locator, payload;
    uint64_t source_revision = 0;
    std::string encode() const {
        wire::Writer w;
        w.bytes_field(1, id);
        w.uint_field(2, projection_version);
        w.uint_field(3, domain_revision);
        w.uint_field(4, lifecycle_generation);
        for (const auto &text : texts) {
            wire::Writer t;
            t.bytes_field(1, text.name);
            t.bytes_field(2, text.text);
            t.double_field(3, text.weight);
            w.bytes_field(5, t.data());
        }
        encoding::metadata(w, metadata);
        if (!vector.empty())
            w.bytes_field(7, encoding::vector(vector));
        if (!vector_space.empty())
            w.bytes_field(8, vector_space);
        w.bytes_field(9, source_locator);
        w.uint_field(10, source_revision);
        w.bytes_field(11, payload);
        return w.take();
    }
};
struct Patch {
    std::string id;
    uint64_t expected_version = 0, next_version = 0, lifecycle_generation = 1;
    std::map<std::string, Value> metadata;
    std::vector<std::string> remove_metadata;
    bool replace_metadata = false;
    std::optional<uint64_t> domain_revision, source_revision;
};
struct Phrase {
    std::string text;
    uint32_t max_gap = 0;
    std::string field;
};
struct Query {
    enum Operation { Search = 1, Get = 2, Stats = 3, List = 4 };
    Operation operation = Search;
    std::string text, id, after_id;
    uint32_t limit = 8;
    Filter where;
    bool dense = false, match_all_terms = false, allow_partial = false;
    bool approximate = false, prefix_last = false, include_positions = false,
         include_explain = false;
    uint32_t statistics_policy = 0, ef_search = 96, snippet_bytes = 160, term_budget = 128;
    std::vector<Phrase> phrases;
    std::vector<std::string> excluded_terms, scoring_fields;
    std::vector<float> vector;
    std::string vector_space;
    std::optional<double> min_similarity;
    uint64_t posting_budget = 12000, candidate_budget = 1024, scan_budget = 100000;
    uint64_t memory_budget = 16ull << 20, timeout_ms = 0;
    uint64_t expected_projection_version = 0, expected_mutation_sequence = 0;
};
// Views point into Result's owned native buffer. Keep Result alive and unmoved
// while using a Hit reference. Moving Result transfers ownership of its bytes.
struct MatchPosition {
    std::string_view field, snippet;
    uint64_t begin = 0, end = 0, snippet_begin = 0;
};
struct ScoreDetail {
    std::string_view term, field;
    uint64_t tf = 0, length = 0;
    double average_length = 0, idf = 0, weight = 0, contribution = 0;
};
struct Hit {
    std::string_view id, payload, source_locator, document_wire;
    uint64_t projection_version = 0, domain_revision = 0, lifecycle_generation = 0;
    double score = 0, dense_similarity = 0;
    std::vector<std::string_view> matched_fields;
    std::vector<MatchPosition> positions;
    std::vector<ScoreDetail> details;
};
class Result {
    Buffer bytes_;
    static uint64_t number(wire::Reader &r, wire::Field f) {
        uint64_t n = 0;
        detail::check(f.type == wire::Type::Varint && r.varint(n) ? FQDB_OK : FQDB_CORRUPT);
        return n;
    }
    static std::string_view bytes(wire::Reader &r, wire::Field f) {
        std::string_view b;
        detail::check(f.type == wire::Type::Bytes && r.bytes(b) ? FQDB_OK : FQDB_CORRUPT);
        return b;
    }
    static double real(wire::Reader &r, wire::Field f) {
        uint64_t bits = 0;
        detail::check(f.type == wire::Type::Fixed64 && r.fixed(bits, 8) ? FQDB_OK : FQDB_CORRUPT);
        double value = 0;
        std::memcpy(&value, &bits, 8);
        return value;
    }
    template <class F> static void fields(std::string_view data, F fn) {
        wire::Reader r(data);
        wire::Field f;
        while (r.next(f))
            if (!fn(r, f))
                detail::check(r.skip(f) ? FQDB_OK : FQDB_CORRUPT);
        detail::check(r.done() ? FQDB_OK : FQDB_CORRUPT);
    }

  public:
    std::vector<Hit> hits;
    uint64_t snapshot_version = 0, mutation_sequence = 0, record_count = 0, vector_count = 0;
    uint64_t postings_visited = 0, candidates = 0, records_scanned = 0;
    uint64_t terms_visited = 0, statistics_scanned = 0, blocks_visited = 0, graph_visited = 0,
             layout_generation = 0;
    std::optional<uint64_t> total_hits;
    uint32_t termination = 0, ranking_kind = 0, index_coverage = 0, match_evidence = 0;
    std::string_view next_cursor, strategy, analyzer;
    explicit Result(Buffer data) : bytes_(std::move(data)) {
        fields(bytes_.view(), [&](wire::Reader &r, wire::Field f) {
            if (f.number == 1) {
                Hit h;
                fields(bytes(r, f), [&](wire::Reader &a, wire::Field q) {
                    if (q.number == 1) {
                        h.document_wire = bytes(a, q);
                        fields(h.document_wire, [&](wire::Reader &d, wire::Field t) {
                            switch (t.number) {
                            case 1:
                                h.id = bytes(d, t);
                                break;
                            case 2:
                                h.projection_version = number(d, t);
                                break;
                            case 3:
                                h.domain_revision = number(d, t);
                                break;
                            case 4:
                                h.lifecycle_generation = number(d, t);
                                break;
                            case 9:
                                h.source_locator = bytes(d, t);
                                break;
                            case 11:
                                h.payload = bytes(d, t);
                                break;
                            default:
                                return false;
                            }
                            return true;
                        });
                    } else if (q.number == 2 || q.number == 3) {
                        uint64_t bits = 0;
                        double value = 0;
                        detail::check(q.type == wire::Type::Fixed64 && a.fixed(bits, 8)
                                          ? FQDB_OK
                                          : FQDB_CORRUPT);
                        std::memcpy(&value, &bits, 8);
                        (q.number == 2 ? h.score : h.dense_similarity) = value;
                    } else if (q.number == 4)
                        h.matched_fields.push_back(bytes(a, q));
                    else if (q.number == 5) {
                        MatchPosition p;
                        fields(bytes(a, q), [&](wire::Reader &b, wire::Field f) {
                            if (f.number == 1)
                                p.field = bytes(b, f);
                            else if (f.number == 2)
                                p.begin = number(b, f);
                            else if (f.number == 3)
                                p.end = number(b, f);
                            else if (f.number == 4)
                                p.snippet = bytes(b, f);
                            else if (f.number == 5)
                                p.snippet_begin = number(b, f);
                            else
                                return false;
                            return true;
                        });
                        h.positions.push_back(p);
                    } else if (q.number == 6) {
                        ScoreDetail d;
                        fields(bytes(a, q), [&](wire::Reader &b, wire::Field f) {
                            if (f.number == 1)
                                d.term = bytes(b, f);
                            else if (f.number == 2)
                                d.field = bytes(b, f);
                            else if (f.number == 3)
                                d.tf = number(b, f);
                            else if (f.number == 4)
                                d.length = number(b, f);
                            else if (f.number == 5)
                                d.average_length = real(b, f);
                            else if (f.number == 6)
                                d.idf = real(b, f);
                            else if (f.number == 7)
                                d.weight = real(b, f);
                            else if (f.number == 8)
                                d.contribution = real(b, f);
                            else
                                return false;
                            return true;
                        });
                        h.details.push_back(d);
                    } else
                        return false;
                    return true;
                });
                hits.push_back(std::move(h));
            } else
                switch (f.number) {
                case 2:
                    snapshot_version = number(r, f);
                    break;
                case 3:
                    mutation_sequence = number(r, f);
                    break;
                case 4:
                    ranking_kind = static_cast<uint32_t>(number(r, f));
                    break;
                case 5:
                    termination = static_cast<uint32_t>(number(r, f));
                    break;
                case 6:
                    index_coverage = static_cast<uint32_t>(number(r, f));
                    break;
                case 7:
                    match_evidence = static_cast<uint32_t>(number(r, f));
                    break;
                case 8:
                    postings_visited = number(r, f);
                    break;
                case 9:
                    candidates = number(r, f);
                    break;
                case 10:
                    records_scanned = number(r, f);
                    break;
                case 11:
                    next_cursor = bytes(r, f);
                    break;
                case 12:
                    total_hits = number(r, f);
                    break;
                case 13:
                    record_count = number(r, f);
                    break;
                case 14:
                    vector_count = number(r, f);
                    break;
                case 15:
                    strategy = bytes(r, f);
                    break;
                case 16:
                    analyzer = bytes(r, f);
                    break;
                case 17:
                    terms_visited = number(r, f);
                    break;
                case 18:
                    statistics_scanned = number(r, f);
                    break;
                case 19:
                    blocks_visited = number(r, f);
                    break;
                case 20:
                    graph_visited = number(r, f);
                    break;
                case 21:
                    layout_generation = number(r, f);
                    break;
                default:
                    return false;
                }
            return true;
        });
    }
    Result(Result &&) noexcept = default;
    Result &operator=(Result &&) noexcept = default;
    bool complete() const {
        return termination == FQIDX_FINISHED && index_coverage == 0;
    }
};
struct MaintenanceOptions {
    enum Kind { Compact = 1, RebuildGraph = 2, Verify = 3, CollectGraphs = 4 };
    Kind operation = Compact;
    uint32_t max_terms = 8, max_records = 128;
    uint64_t max_postings = 20000, memory_budget = 32ull << 20, timeout_ms = 0,
             expected_mutation_sequence = 0;
    std::string after_term, after_id, vector_space, after_key;
};
struct MaintenanceResult {
    std::string next_term, next_id, next_key;
    uint64_t mutation_sequence = 0, terms = 0, postings = 0, records = 0, layout_generation = 0,
             reclaimed = 0;
    bool finished = false;
};
class Client {
    uint64_t token_;
    std::string space_, partition_;
    template <class Function>
    decltype(auto) change(StoreTransaction &tx, Function &&function) const {
        try {
            return function();
        } catch (...) {
            // Encoding/decoding can fail before or after the C ABI call. Keep
            // the same all-or-nothing contract as native mutation failures.
            fqdb_store_txn_cancel(tx.handle_.get());
            throw;
        }
    }
    wire::Writer request(unsigned operation) const {
        wire::Writer w;
        w.bytes_field(1, space_);
        w.bytes_field(2, partition_);
        w.uint_field(3, operation);
        return w;
    }
    std::string encode(const Query &q) const {
        auto w = request(q.operation);
        if (!q.text.empty())
            w.bytes_field(4, q.text);
        w.uint_field(5, q.limit);
        w.bytes_field(6, q.where.encode());
        if (q.dense || q.approximate)
            w.uint_field(7, q.approximate ? 2 : 1);
        if (!q.vector.empty())
            w.bytes_field(8, encoding::vector(q.vector));
        if (!q.vector_space.empty())
            w.bytes_field(9, q.vector_space);
        w.uint_field(10, q.posting_budget);
        w.uint_field(11, q.candidate_budget);
        w.uint_field(12, q.scan_budget);
        w.uint_field(13, q.memory_budget);
        w.uint_field(14, q.timeout_ms);
        w.uint_field(15, q.match_all_terms);
        if (!q.id.empty())
            w.bytes_field(16, q.id);
        if (!q.after_id.empty())
            w.bytes_field(17, q.after_id);
        if (q.min_similarity)
            w.double_field(18, *q.min_similarity);
        w.uint_field(19, q.allow_partial);
        w.uint_field(20, q.expected_projection_version);
        w.uint_field(22, q.expected_mutation_sequence);
        w.uint_field(21, q.statistics_policy);
        for (const auto &p : q.phrases) {
            wire::Writer a;
            a.bytes_field(1, p.text);
            a.uint_field(2, p.max_gap);
            a.bytes_field(3, p.field);
            w.bytes_field(23, a.data());
        }
        w.uint_field(24, q.prefix_last);
        w.uint_field(26, q.ef_search);
        w.uint_field(27, q.include_positions);
        w.uint_field(28, q.snippet_bytes);
        w.uint_field(29, q.term_budget);
        w.uint_field(30, q.include_explain);
        for (const auto &t : q.excluded_terms)
            w.bytes_field(31, t);
        for (const auto &field : q.scoring_fields)
            w.bytes_field(32, field);
        return w.take();
    }

  public:
    Client(const Store &store, std::string index_space, std::string partition)
        : token_(0), space_(std::move(index_space)), partition_(std::move(partition)) {
        detail::check(fqidx_abi_version() == 1 &&
                              fqidx_core_bridge_address() ==
                                  reinterpret_cast<uintptr_t>(&fqdb_store_bridge_abi_version)
                          ? FQDB_OK
                          : FQDB_UNSUPPORTED);
        fqdb_store_bridge_info info{};
        info.struct_size = sizeof(info);
        info.abi_version = 1;
        detail::check(fqdb_store_bridge_identity(store.handle_.get(), &info));
        token_ = info.store_token;
    }
    void upsert(StoreTransaction &tx, const Document &document,
                uint64_t expected_version = 0) const {
        change(tx, [&] {
            auto w = request(FQIDX_APPLY_UPSERT);
            w.uint_field(4, expected_version);
            w.bytes_field(5, document.encode());
            detail::check(fqidx_txn_apply(tx.handle_.get(), token_, detail::slice(w.data())));
        });
    }
    void patch(StoreTransaction &tx, const Patch &patch) const {
        change(tx, [&] {
            auto w = request(FQIDX_APPLY_PATCH);
            wire::Writer d;
            encoding::metadata(d, patch.metadata);
            if (patch.domain_revision)
                d.uint_field(3, *patch.domain_revision);
            if (patch.source_revision)
                d.uint_field(10, *patch.source_revision);
            for (const auto &name : patch.remove_metadata)
                d.bytes_field(13, name);
            d.uint_field(14, patch.replace_metadata);
            w.uint_field(4, patch.expected_version);
            w.bytes_field(5, d.data());
            w.bytes_field(6, patch.id);
            w.uint_field(7, patch.next_version);
            w.uint_field(8, patch.lifecycle_generation);
            detail::check(fqidx_txn_apply(tx.handle_.get(), token_, detail::slice(w.data())));
        });
    }
    void erase(StoreTransaction &tx, std::string_view id, uint64_t expected_version,
               uint64_t next_version, uint64_t lifecycle_generation) const {
        change(tx, [&] {
            auto w = request(FQIDX_APPLY_ERASE);
            w.uint_field(4, expected_version);
            w.bytes_field(6, id);
            w.uint_field(7, next_version);
            w.uint_field(8, lifecycle_generation);
            detail::check(fqidx_txn_apply(tx.handle_.get(), token_, detail::slice(w.data())));
        });
    }
    Result query(const StoreSnapshot &view, const Query &q,
                 const fqidx_query_control *control = nullptr) const {
        auto request_bytes = encode(q);
        fqdb_buffer out{};
        detail::check(fqidx_snapshot_query(view.handle_.get(), token_, detail::slice(request_bytes),
                                           control, &out));
        return Result(detail::Access::adopt(out));
    }
    Result query(StoreTransaction &tx, const Query &q,
                 const fqidx_query_control *control = nullptr) const {
        auto request_bytes = encode(q);
        fqdb_buffer out{};
        detail::check(
            fqidx_txn_query(tx.handle_.get(), token_, detail::slice(request_bytes), control, &out));
        return Result(detail::Access::adopt(out));
    }
    MaintenanceResult maintain(StoreTransaction &tx, const MaintenanceOptions &options = {},
                               const fqidx_query_control *control = nullptr) const {
        return change(tx, [&] {
            auto w = request(options.operation);
            w.uint_field(4, options.max_terms);
            w.uint_field(5, options.max_postings);
            w.uint_field(6, options.memory_budget);
            w.uint_field(7, options.timeout_ms);
            w.bytes_field(8, options.after_term);
            w.uint_field(9, options.expected_mutation_sequence);
            w.bytes_field(10, options.vector_space);
            w.bytes_field(11, options.after_id);
            w.uint_field(12, options.max_records);
            w.bytes_field(13, options.after_key);
            fqdb_buffer out{};
            detail::check(fqidx_txn_maintain(tx.handle_.get(), token_, detail::slice(w.data()),
                                             control, &out));
            auto buffer = detail::Access::adopt(out);
            wire::Reader r(buffer.view());
            wire::Field f;
            MaintenanceResult result;
            while (r.next(f)) {
                if (f.number == 1 || f.number == 2 || f.number == 9) {
                    std::string_view value;
                    detail::check(f.type == wire::Type::Bytes && r.bytes(value) ? FQDB_OK
                                                                                : FQDB_CORRUPT);
                    if (f.number == 1)
                        result.next_term = value;
                    else if (f.number == 2)
                        result.next_id = value;
                    else
                        result.next_key = value;
                } else if ((f.number >= 3 && f.number <= 8) || f.number == 10) {
                    uint64_t n = 0;
                    detail::check(f.type == wire::Type::Varint && r.varint(n) ? FQDB_OK
                                                                              : FQDB_CORRUPT);
                    if (f.number == 3)
                        result.mutation_sequence = n;
                    else if (f.number == 4)
                        result.terms = n;
                    else if (f.number == 5)
                        result.postings = n;
                    else if (f.number == 6)
                        result.records = n;
                    else if (f.number == 7)
                        result.finished = n != 0;
                    else if (f.number == 10)
                        result.reclaimed = n;
                    else
                        result.layout_generation = n;
                } else
                    detail::check(r.skip(f) ? FQDB_OK : FQDB_CORRUPT);
            }
            detail::check(r.done() ? FQDB_OK : FQDB_CORRUPT);
            return result;
        });
    }
};
} // namespace fengqudb::index
#endif
