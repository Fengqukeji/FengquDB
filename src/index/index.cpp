// Native retrieval over caller-owned FengquDB transactions and immutable snapshots.
#include "ann.hpp"
#include "postings.hpp"
#include "storage.hpp"
#include "text_query.hpp"
#include "vector_math.hpp"
#include <array>
#include <limits>

namespace fengqudb::index::impl {
struct Mutation {
    std::string base, id;
    uint32_t op = 0;
    uint64_t expected = 0, next = 0, generation = 0;
    Document doc;
};
Mutation mutation(std::string_view s) {
    Mutation m;
    std::string space, partition;
    fields(s, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1:
            space = bytes(r, f);
            break;
        case 2:
            partition = bytes(r, f);
            break;
        case 3: {
            auto n = number(r, f);
            require(n >= 1 && n <= 3);
            m.op = static_cast<uint32_t>(n);
            break;
        }
        case 4:
            m.expected = number(r, f);
            break;
        case 5:
            m.doc = document(bytes(r, f));
            break;
        case 6:
            m.id = bytes(r, f);
            break;
        case 7:
            m.next = number(r, f);
            break;
        case 8:
            m.generation = number(r, f);
            break;
        case 25:
            require(number(r, f) == 1, FQDB_UNSUPPORTED);
            break;
        default:
            return false;
        }
        return true;
    });
    m.base = root(space, partition);
    require(m.op != 0);
    if (m.id.empty())
        m.id = m.doc.id;
    else
        require(m.doc.id.empty() || m.doc.id == m.id);
    require(!m.id.empty() && m.id.size() <= 1024);
    return m;
}
void advance(Stats &s) {
    require(s.sequence < UINT64_MAX, FQDB_LIMIT);
    ++s.sequence;
}
void adjust(Stats &s, const Document &d, int sign) {
    if (d.deleted)
        return;
    if (sign < 0) {
        require(s.count > 0 && (!(d.vector_present || !d.vector.empty()) || s.vectors > 0),
                FQDB_CORRUPT);
        --s.count;
        if (d.vector_present || !d.vector.empty())
            --s.vectors;
    } else {
        require(s.count < UINT64_MAX, FQDB_LIMIT);
        ++s.count;
        if (d.vector_present || !d.vector.empty()) {
            require(s.vectors < UINT64_MAX, FQDB_LIMIT);
            ++s.vectors;
        }
    }
    for (const auto &[name, n] : d.lengths) {
        auto &v = s.lengths[name];
        if (sign < 0) {
            require(v >= n, FQDB_CORRUPT);
            v -= n;
        } else {
            require(v <= UINT64_MAX - n, FQDB_LIMIT);
            v += n;
        }
    }
    for (auto it = s.lengths.begin(); it != s.lengths.end();)
        if (it->second == 0)
            it = s.lengths.erase(it);
        else
            ++it;
    require(s.lengths.size() <= 128, FQDB_LIMIT);
}
std::string patch_bytes(std::string_view original, const Document &next) {
    // Subtract only the replaced fields. Copy unchanged runs (including text,
    // vector and cached terms) directly, then append the small new header/meta.
    w::Reader r(original);
    w::Field f;
    size_t run = 0;
    std::string out;
    out.reserve(original.size() + 512);
    while (!r.done()) {
        size_t begin = r.position();
        require(r.next(f) && r.skip(f), FQDB_CORRUPT);
        if (f.number == 2 || f.number == 3 || f.number == 6 || f.number == 10) {
            out.append(original.substr(run, begin - run));
            run = r.position();
        }
    }
    out.append(original.substr(run));
    w::Writer changed;
    changed.uint_field(2, next.version);
    changed.uint_field(3, next.domain);
    changed.uint_field(10, next.source);
    for (const auto &[name, v] : next.metadata) {
        w::Writer m;
        m.bytes_field(1, name);
        m.bytes_field(2, encode(v));
        changed.bytes_field(6, m.data());
    }
    out.append(changed.data());
    return out;
}
void apply(const Reader &r, const Mutation &m) {
    auto st = stats(r, m.base);
    Document old;
    auto prior = r.get(m.base + "R" + component(m.id));
    bool found = prior.found;
    if (found) {
        try {
            old = document(prior.value, m.op == FQIDX_APPLY_PATCH, m.op != FQIDX_APPLY_PATCH);
        } catch (const Error &) {
            throw Error{FQDB_CORRUPT};
        }
        require(old.id == m.id && old.version, FQDB_CORRUPT);
    }
    Document next = m.doc;
    if (m.op == FQIDX_APPLY_UPSERT) {
        require(next.id == m.id && next.version > 0 && !next.deleted && next.remove.empty() &&
                !next.replace_metadata);
        require(next.version > m.expected, FQDB_CONFLICT);
        require(next.space.empty() == next.vector.empty());
        normalize(next.vector);
        analyze(next);
        if (!next.vector.empty()) {
            auto key = m.base + "V" + component(next.space);
            auto existing = r.get(key);
            uint64_t dimension = 0;
            if (existing.found) {
                fields(existing.value, [&](w::Reader &a, w::Field f) {
                    if (f.number != 1)
                        return false;
                    dimension = number(a, f);
                    return true;
                });
                require(dimension == next.vector.size(), FQDB_INVALID_ARGUMENT);
            } else {
                w::Writer v;
                v.uint_field(1, next.vector.size());
                v.uint_field(2, 0);
                auto rebuilding = r.get(m.base + "J");
                if (rebuilding.found) {
                    std::string scope;
                    fields(rebuilding.value, [&](w::Reader &a, w::Field f) {
                        if (f.number != 1)
                            return false;
                        scope = bytes(a, f);
                        return true;
                    });
                    if (scope.empty() || scope == next.space)
                        v.uint_field(4, 1);
                }
                r.put(key, v.data());
            }
        }
        if (found && old.version == next.version) {
            require(encode(old, true) == encode(next, true), FQDB_CONFLICT);
            return;
        }
        require((found ? old.version : 0) == m.expected && next.version > m.expected,
                FQDB_CONFLICT);
        if (found)
            require(next.generation >= old.generation &&
                        (!old.deleted || next.generation > old.generation),
                    FQDB_CONFLICT);
    } else if (m.op == FQIDX_APPLY_PATCH) {
        require(found && !old.deleted, FQDB_NOT_FOUND);
        require(m.doc.texts.empty() && m.doc.vector.empty() && m.doc.space.empty() &&
                m.doc.payload.empty() && m.doc.locator.empty() && !m.doc.deleted);
        require((!m.doc.version || m.doc.version == m.next) &&
                (!m.doc.generation || m.doc.generation == m.generation));
        require(old.generation == m.generation && m.next > m.expected, FQDB_CONFLICT);
        // Build the prospective value before CAS so exact retries are idempotent.
        next = old;
        next.version = m.next;
        if (m.doc.has_domain)
            next.domain = m.doc.domain;
        if (m.doc.has_source)
            next.source = m.doc.source;
        if (m.doc.replace_metadata)
            next.metadata.clear();
        for (const auto &name : m.doc.remove) {
            (void)runes(name);
            next.metadata.erase(name);
        }
        for (const auto &[name, v] : m.doc.metadata) {
            (void)runes(name);
            if (v.kind == 1)
                (void)runes(v.data);
            next.metadata[name] = v;
        }
        require(next.metadata.size() <= 64, FQDB_LIMIT);
        if (old.version == m.next) {
            require(old.metadata == next.metadata && old.domain == next.domain &&
                        old.source == next.source,
                    FQDB_CONFLICT);
            return;
        }
        require(old.version == m.expected, FQDB_CONFLICT);
        auto encoded = patch_bytes(prior.value, next);
        require(encoded.size() <= FQDB_MAX_VALUE_BYTES, FQDB_LIMIT);
        r.put(m.base + "R" + component(m.id), encoded);
        advance(st);
        r.put(m.base + "S", encode(st));
        return;
    } else {
        require(m.next > m.expected, FQDB_CONFLICT);
        if (found && old.deleted && old.version == m.next && old.generation == m.generation)
            return;
        require((found ? old.version : 0) == m.expected &&
                    (!found || old.generation == m.generation),
                FQDB_CONFLICT);
        next = {};
        next.id = m.id;
        next.version = m.next;
        next.generation = m.generation;
        next.domain = old.domain;
        next.source = old.source;
        next.deleted = true;
    }
    // Reuse unchanged postings, remove old-only terms; do not scan a term's corpus.
    const Terms empty;
    const auto &before = found && !old.deleted ? old.terms : empty;
    const auto &after = next.deleted ? empty : next.terms;
    for (const auto &[term, fs] : before) {
        (void)fs;
        if (!after.contains(term)) {
            r.put(m.base + "P" + component(term) + component(m.id), posting_tombstone(m.id));
            update_df(r, m.base, term, -1);
        }
    }
    for (const auto &[term, fs] : after) {
        auto i = before.find(term);
        if (i == before.end()) {
            update_df(r, m.base, term, 1);
            r.put(m.base + "P" + component(term) + component(m.id), encode_term(m.id, fs));
        } else if (i->second != fs)
            r.put(m.base + "P" + component(term) + component(m.id), encode_term(m.id, fs));
    }
    if (found)
        adjust(st, old, -1);
    adjust(st, next, 1);
    bool changed_vector = !found || old.deleted || next.deleted || old.space != next.space ||
                          old.vector != next.vector || old.generation != next.generation;
    if (changed_vector) {
        if (found && !old.deleted && !old.vector.empty()) {
            vector_count(r, m.base, old.space, -1);
            auto c = vector_contract(r, m.base, old.space);
            if (c.enabled)
                AnnGraph(r, m.base, old.space, c.generation).erase(old.id);
            if (c.building)
                AnnGraph(r, m.base, old.space, c.building).erase(old.id);
        }
        if (!next.deleted && !next.vector.empty()) {
            vector_count(r, m.base, next.space, 1);
            auto c = vector_contract(r, m.base, next.space);
            if (c.enabled)
                AnnGraph(r, m.base, next.space, c.generation).upsert(next);
            if (c.building)
                AnnGraph(r, m.base, next.space, c.building).upsert(next);
        }
    }
    advance(st);
    auto encoded = encode(next, true);
    require(encoded.size() <= FQDB_MAX_VALUE_BYTES, FQDB_LIMIT);
    r.put(m.base + "R" + component(m.id), encoded);
    r.put(m.base + "S", encode(st));
}
struct Query {
    std::string base, text, id, after, space;
    uint32_t op = 1, mode = 0, limit = 8, statistics_policy = 0, ef = 96, snippet = 160,
             term_budget = 128;
    uint64_t postings = 12000, candidates = 1024, scan = 100000, memory = 16ull << 20, timeout = 0,
             expected = 0, sequence = 0;
    bool all = false, partial = false, prefix = false, positions = false, explain = false;
    std::vector<Phrase> phrases;
    std::set<std::string> excluded, scoring_fields;
    std::optional<double> minimum;
    Predicate where;
    std::vector<float> vector;
};
Query query(std::string_view s) {
    Query q;
    std::string space, partition;
    fields(s, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1:
            space = bytes(r, f);
            break;
        case 2:
            partition = bytes(r, f);
            break;
        case 3: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4);
            q.op = static_cast<uint32_t>(n);
            break;
        }
        case 4:
            q.text = bytes(r, f);
            break;
        case 5: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4096);
            q.limit = static_cast<uint32_t>(n);
            break;
        }
        case 6:
            q.where = predicate(bytes(r, f));
            break;
        case 7: {
            auto n = number(r, f);
            require(n <= 2, FQDB_UNSUPPORTED);
            q.mode = static_cast<uint32_t>(n);
            break;
        }
        case 8:
            q.vector = floats(bytes(r, f));
            break;
        case 9:
            q.space = bytes(r, f);
            break;
        case 10:
            q.postings = number(r, f);
            break;
        case 11:
            q.candidates = number(r, f);
            break;
        case 12:
            q.scan = number(r, f);
            break;
        case 13:
            q.memory = number(r, f);
            break;
        case 14:
            q.timeout = number(r, f);
            break;
        case 15: {
            auto n = number(r, f);
            require(n <= 1);
            q.all = n != 0;
            break;
        }
        case 16:
            q.id = bytes(r, f);
            break;
        case 17:
            q.after = bytes(r, f);
            break;
        case 18:
            q.minimum = real(r, f);
            break;
        case 19: {
            auto n = number(r, f);
            require(n <= 1);
            q.partial = n != 0;
            break;
        }
        case 20:
            q.expected = number(r, f);
            break;
        case 21: {
            auto n = number(r, f);
            require(n <= 1, FQDB_UNSUPPORTED);
            q.statistics_policy = uint32_t(n);
        } break;
        case 22:
            q.sequence = number(r, f);
            break;
        case 23:
            require(q.phrases.size() < 8, FQDB_LIMIT);
            q.phrases.push_back(phrase(bytes(r, f)));
            break;
        case 24: {
            auto n = number(r, f);
            require(n <= 1);
            q.prefix = n != 0;
        } break;
        case 25:
            require(number(r, f) == 1, FQDB_UNSUPPORTED);
            break;
        case 26: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4096);
            q.ef = uint32_t(n);
        } break;
        case 27: {
            auto n = number(r, f);
            require(n <= 1);
            q.positions = n != 0;
        } break;
        case 28: {
            auto n = number(r, f);
            require(n <= 4096);
            q.snippet = n ? uint32_t(n) : 160;
        } break;
        case 29: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4096);
            q.term_budget = uint32_t(n);
        } break;
        case 31: {
            auto text = bytes(r, f);
            require(text.size() <= 4096);
            for (auto &t : tokens(text, 64)) {
                q.excluded.insert(std::move(t));
                require(q.excluded.size() <= 32, FQDB_LIMIT);
            }
            break;
        }
        case 32: {
            auto name = bytes(r, f);
            require(!name.empty() && name.size() <= 128 && q.scoring_fields.size() < 32);
            (void)runes(name);
            q.scoring_fields.emplace(name);
            break;
        }
        case 30: {
            auto n = number(r, f);
            require(n <= 1);
            q.explain = n != 0;
        } break;
        default:
            return false;
        }
        return true;
    });
    q.base = root(space, partition);
    (void)runes(q.text);
    validate(q.where);
    require(q.text.size() <= 4096 && q.id.size() <= 1024 && q.after.size() <= 1024 &&
            q.space.size() <= 512);
    require(q.postings && q.candidates && q.scan && q.memory >= 4096 &&
            q.memory <= uint64_t(1) << 32 && q.timeout <= 86400000);
    if (q.minimum)
        require(*q.minimum >= -1 && *q.minimum <= 1);
    if (q.op == FQIDX_QUERY_SEARCH && q.mode != FQIDX_LEXICAL) {
        require(!q.vector.empty() && !q.space.empty() && q.text.empty() && !q.all &&
                q.phrases.empty() && !q.prefix && !q.statistics_policy && q.excluded.empty() &&
                q.scoring_fields.empty());
        normalize(q.vector);
        if (q.mode == FQIDX_DENSE_ANN)
            q.ef = std::max(q.ef, q.limit);
    } else
        require(q.mode == FQIDX_LEXICAL && q.vector.empty() && q.space.empty() && !q.minimum);
    if (q.op == FQIDX_QUERY_GET)
        require(!q.id.empty());
    if (q.op != FQIDX_QUERY_SEARCH)
        require(q.phrases.empty() && !q.prefix && !q.statistics_policy && q.excluded.empty() &&
                q.scoring_fields.empty() && q.text.empty() && !q.all);
    if (!q.scoring_fields.empty())
        require(!q.text.empty() || !q.phrases.empty());
    if (q.prefix)
        require(!q.text.empty());
    return q;
}
bool excluded_match(std::string_view encoded, const Query &q, Work &work) {
    if (q.excluded.empty())
        return false;
    bool found = false;
    unsigned seen = 0;
    fields(encoded, [&](w::Reader &r, w::Field f) {
        if (f.number != 100 || found)
            return false;
        if ((++seen & 127) == 0)
            work.check();
        auto raw = bytes(r, f);
        std::string term;
        fields(raw, [&](w::Reader &a, w::Field t) {
            if (t.number != 1)
                return false;
            term = bytes(a, t);
            return true;
        });
        if (q.excluded.contains(term)) {
            if (q.scoring_fields.empty())
                found = true;
            else
                for (const auto &[name, value] : term_fields(raw)) {
                    (void)value;
                    if (q.scoring_fields.contains(name)) {
                        found = true;
                        break;
                    }
                }
        }
        return true;
    });
    return found;
}
struct Detail {
    std::string term, field;
    uint64_t tf = 0, length = 0;
    double average = 0, idf = 0, weight = 0, contribution = 0;
};
struct Candidate {
    double score = 0;
    uint64_t mask = 0;
    std::set<std::string> fields;
    std::vector<Detail> details;
};
struct Scored {
    std::string id;
    double score = 0;
    std::set<std::string> fields;
    std::vector<Detail> details;
};
inline bool better(const Scored &a, const Scored &b) {
    return a.score != b.score ? a.score > b.score : a.id < b.id;
}
struct Result {
    std::vector<Scored> best;
    std::string cursor, strategy;
    uint32_t stop = FQIDX_FINISHED;
    uint64_t postings = 0, scanned = 0, candidates = 0, total = 0;
    uint64_t terms = 0, statistics_scanned = 0, blocks = 0, graph = 0;
    uint32_t ranking = 0;
    bool counted = true;
    bool restricted_counts = false;
    std::optional<std::pair<uint64_t, uint64_t>> scoped_counts;
};
void offer(Result &out, Scored item, uint32_t limit) {
    if (out.best.size() < limit) {
        out.best.push_back(std::move(item));
        std::push_heap(out.best.begin(), out.best.end(), better);
    } else if (better(item, out.best.front())) {
        std::pop_heap(out.best.begin(), out.best.end(), better);
        out.best.back() = std::move(item);
        std::push_heap(out.best.begin(), out.best.end(), better);
    }
}
void scan_search(const Reader &r, const Query &q, const Stats &st, Work &work, Result &out,
                 bool list) {
    (void)st;
    out.strategy = list ? "ordered_projection_list"
                        : (q.mode ? "dense_exact_partition" : "filtered_projection_list");
    r.scan(
        q.base + "R",
        [&](std::string_view, std::string_view encoded) {
            work.check();
            if (out.scanned >= q.scan)
                throw Stop{FQIDX_SCAN_LIMIT};
            ++out.scanned;
            ScopedCharge charge(work, encoded.size() * 2 + 4096);
            Document d;
            try {
                d = document(encoded, true, false, q.mode != 0);
            } catch (const Error &) {
                throw Error{FQDB_CORRUPT};
            }
            if (d.deleted || !matches(q.where, d.metadata) || excluded_match(encoded, q, work))
                return true;
            double score = 0;
            if (q.mode) {
                if (d.vector.empty() || d.space != q.space)
                    return true;
                require(d.vector.size() == q.vector.size(), FQDB_INVALID_ARGUMENT);
                score = vector_dot(d.vector.data(), q.vector.data(), d.vector.size());
                score = std::clamp(score, -1., 1.);
                if (q.minimum && score < *q.minimum)
                    return true;
            }
            if (out.candidates >= q.candidates)
                throw Stop{FQIDX_CANDIDATE_LIMIT};
            ++out.candidates;
            ++out.total;
            if (list) {
                if (out.best.size() == q.limit) {
                    out.cursor = out.best.back().id;
                    out.counted = false;
                    return false;
                }
                work.take(d.id.size() + 256);
                out.best.push_back({std::move(d.id), 0, {}, {}});
            } else {
                if (out.best.size() < q.limit)
                    work.take(d.id.size() + 256);
                offer(out, {std::move(d.id), score, {}, {}}, q.limit);
            }
            return true;
        },
        list ? q.after : std::string_view{});
}
struct Needle {
    std::string term;
    uint64_t df = 0, mask = 0;
};
std::vector<Needle> plan_terms(const Reader &r, const Query &q, Work &work, Result &out) {
    std::map<std::string, uint64_t> terms;
    auto add = [&](std::string term, uint64_t mask) {
        auto it = terms.find(term);
        if (it != terms.end()) {
            it->second |= mask;
            return;
        }
        if (terms.size() >= q.term_budget)
            throw Stop{FQIDX_TERM_LIMIT};
        work.take(term.size() + 128);
        terms.emplace(std::move(term), mask);
    };
    std::string_view text = q.text;
    PrimaryToken last;
    if (q.prefix) {
        primary_each(text, [&](const PrimaryToken &t) {
            last = t;
            return true;
        });
        require(!last.term.empty());
        text = text.substr(0, last.begin);
    }
    auto ts = tokens(text, 256);
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    require(ts.size() <= 32, FQDB_LIMIT);
    for (size_t i = 0; i < ts.size(); ++i)
        add(ts[i], uint64_t(1) << i);
    if (q.prefix) {
        auto encoded = component(last.term);
        encoded.resize(encoded.size() - 2);
        r.scan(q.base + "F" + encoded, [&](std::string_view key, std::string_view value) {
            work.check();
            if (out.terms >= q.term_budget)
                throw Stop{FQIDX_TERM_LIMIT};
            ++out.terms;
            uint64_t n = 0;
            fields(value, [&](w::Reader &a, w::Field f) {
                if (f.number != 1)
                    return false;
                n = number(a, f);
                return true;
            });
            if (n)
                add(uncomponent(key, q.base.size() + 1), uint64_t(1) << ts.size());
            return true;
        });
    } else {
        auto exact = exact_term(q.text);
        if (!exact.empty())
            add(std::move(exact), 0);
    }
    for (const auto &p : q.phrases)
        for (const auto &t : p.terms)
            add(t, 0);
    std::vector<Needle> result;
    result.reserve(terms.size());
    for (auto &[term, mask] : terms) {
        work.check();
        auto n = df(r, q.base, term);
        result.push_back({term, n, mask});
    }
    if (!q.prefix)
        out.terms = result.size();
    return result;
}
Stats filtered_statistics(const Reader &r, const Query &q, const Stats &partition,
                          std::vector<Needle> &needles, Work &work, Result &out) {
    if (!q.statistics_policy || q.where.op == 0)
        return partition;
    Stats filtered;
    filtered.sequence = partition.sequence;
    filtered.layout = partition.layout;
    std::map<std::string, size_t> lookup;
    for (size_t i = 0; i < needles.size(); ++i) {
        lookup.emplace(needles[i].term, i);
        needles[i].df = 0;
    }
    r.scan(q.base + "R", [&](std::string_view, std::string_view encoded) {
        work.check();
        if (out.scanned >= q.scan)
            throw Stop{FQIDX_SCAN_LIMIT};
        ++out.scanned;
        ++out.statistics_scanned;
        ScopedCharge temporary(work, encoded.size() * 2 + 4096);
        auto d = document(encoded, true, false, false);
        if (d.deleted || !matches(q.where, d.metadata))
            return true;
        adjust(filtered, d, 1);
        unsigned checked = 0;
        fields(encoded, [&](w::Reader &a, w::Field f) {
            if (f.number != 100)
                return false;
            if ((++checked & 127) == 0)
                work.check();
            std::string_view term;
            fields(bytes(a, f), [&](w::Reader &b, w::Field z) {
                if (z.number != 1)
                    return false;
                term = bytes(b, z);
                return true;
            });
            auto at = lookup.find(std::string(term));
            if (at != lookup.end()) {
                if (out.postings >= q.postings)
                    throw Stop{FQIDX_POSTING_LIMIT};
                ++out.postings;
                ++needles[at->second].df;
            }
            return true;
        });
        return true;
    });
    return filtered;
}
void single_term(const Reader &r, const Query &q, const Stats &st, const Needle &n, Work &work,
                 Result &out) {
    double idf = std::log1p((double(st.count - n.df) + .5) / (double(n.df) + .5));
    auto contribution = [&](const std::string &name, const TermField &f) {
        auto total = st.lengths.find(name);
        require(total != st.lengths.end() && total->second, FQDB_CORRUPT);
        double average = double(total->second) / st.count, tf = double(f.frequency);
        return idf * f.weight * (tf * 2.2) / (tf + 1.2 * (.25 + .75 * double(f.length) / average));
    };
    auto skip = [&](std::string_view key, std::string_view encoded) {
        if (out.best.size() < q.limit)
            return false;
        std::optional<TermFields> bounds;
        fields(encoded, [&](w::Reader &a, w::Field f) {
            if (f.number != 4)
                return false;
            bounds = term_fields(bytes(a, f), true);
            return true;
        });
        if (!bounds)
            return false;
        double upper = 0;
        for (const auto &[name, f] : *bounds) {
            if (!q.scoring_fields.empty() && !q.scoring_fields.contains(name))
                continue;
            // A predicate-filtered corpus may have no documents for an otherwise
            // present field. Such a block cannot produce a matching contribution.
            if (!st.lengths.contains(name))
                continue;
            upper += contribution(name, f);
        }
        const auto &worst = out.best.front();
        auto first = uncomponent(key, q.base.size() + 1 + component(n.term).size());
        bool omit = upper < worst.score || (upper == worst.score && first > worst.id);
        if (omit) {
            out.counted = false;
            out.strategy = "native_inverted_block_max_bm25";
        }
        return omit;
    };
    PostingStream stream(r, q.base, n.term, work, out.postings, out.blocks, q.postings, skip);
    Posting row;
    while (stream.next(row)) {
        work.check();
        if (!q.scoring_fields.empty() &&
            std::none_of(row.fields.begin(), row.fields.end(),
                         [&](const auto &p) { return q.scoring_fields.contains(p.first); }))
            continue;
        if (out.scanned >= q.scan)
            throw Stop{FQIDX_SCAN_LIMIT};
        ++out.scanned;
        auto blob = r.get(q.base + "R" + component(row.id));
        require(blob.found, FQDB_CORRUPT);
        ScopedCharge temporary(work, blob.value.size() * 2 + 4096);
        auto d = document(blob.value, q.phrases.empty(), false, false);
        require(!d.deleted && d.id == row.id, FQDB_CORRUPT);
        if (!matches(q.where, d.metadata) || excluded_match(blob.value, q, work) ||
            !phrase_matches(d, q.phrases, work, q.scoring_fields))
            continue;
        if (out.candidates >= q.candidates)
            throw Stop{FQIDX_CANDIDATE_LIMIT};
        ++out.candidates;
        ++out.total;
        Scored hit;
        hit.id = row.id;
        for (const auto &[name, f] : row.fields) {
            if (!q.scoring_fields.empty() && !q.scoring_fields.contains(name))
                continue;
            double score = contribution(name, f);
            hit.score += score;
            hit.fields.insert(name);
            if (q.explain)
                hit.details.push_back({n.term, name, f.frequency, f.length,
                                       double(st.lengths.at(name)) / st.count, idf, f.weight,
                                       score});
        }
        work.take(hit.id.size() + 256 + hit.details.size() * 256);
        offer(out, std::move(hit), q.limit);
    }
}
void lexical(const Reader &r, const Query &q, const Stats &partition, Work &work, Result &out) {
    out.strategy =
        q.statistics_policy ? "predicate_filtered_inverted_bm25" : "native_inverted_bm25";
    auto needles = plan_terms(r, q, work, out);
    auto st = filtered_statistics(r, q, partition, needles, work, out);
    if (q.statistics_policy)
        out.scoped_counts = std::pair{st.count, st.vectors};
    uint64_t required = 0;
    for (const auto &n : needles) {
        require(n.df <= st.count, FQDB_CORRUPT);
        required |= n.mask;
    }
    // Missing one mandatory token proves that an AND query cannot match.
    if (q.all)
        for (const auto &n : needles)
            if (n.mask && !n.df) {
                bool alternate = false;
                for (const auto &other : needles)
                    if (other.df && (other.mask & n.mask))
                        alternate = true;
                if (!alternate)
                    return;
            }
    for (const auto &p : q.phrases)
        for (const auto &t : p.terms)
            if (std::none_of(needles.begin(), needles.end(),
                             [&](const Needle &n) { return n.term == t && n.df; }))
                return;
    std::sort(needles.begin(), needles.end(), [](const Needle &a, const Needle &b) {
        return a.df != b.df ? a.df < b.df : a.term < b.term;
    });
    if (std::count_if(needles.begin(), needles.end(), [](const Needle &n) { return n.df > 0; }) ==
        1) {
        const auto &n =
            *std::find_if(needles.begin(), needles.end(), [](const Needle &n) { return n.df > 0; });
        single_term(r, q, st, n, work, out);
        return;
    }
    std::map<std::string, Candidate> candidates;
    std::unordered_set<std::string> rejected;
    try {
        for (const auto &n : needles) {
            if (!n.df)
                continue;
            double idf = std::log1p((double(st.count - n.df) + .5) / (double(n.df) + .5));
            PostingStream stream(r, q.base, n.term, work, out.postings, out.blocks, q.postings);
            Posting row;
            while (stream.next(row)) {
                work.check();
                if (!q.scoring_fields.empty() &&
                    std::none_of(row.fields.begin(), row.fields.end(),
                                 [&](const auto &p) { return q.scoring_fields.contains(p.first); }))
                    continue;
                const auto &id = row.id;
                if (rejected.contains(id))
                    continue;
                auto at = candidates.find(id);
                if (at == candidates.end()) {
                    if (out.scanned >= q.scan)
                        throw Stop{FQIDX_SCAN_LIMIT};
                    ++out.scanned;
                    auto blob = r.get(q.base + "R" + component(id));
                    require(blob.found, FQDB_CORRUPT);
                    ScopedCharge temporary(work, blob.value.size() * 2 + 4096);
                    Document d;
                    try {
                        d = document(blob.value, q.phrases.empty(), false, false);
                    } catch (const Error &) {
                        throw Error{FQDB_CORRUPT};
                    }
                    require(!d.deleted && d.id == id, FQDB_CORRUPT);
                    if (!matches(q.where, d.metadata) || excluded_match(blob.value, q, work) ||
                        !phrase_matches(d, q.phrases, work, q.scoring_fields)) {
                        work.take(id.size() + 96);
                        rejected.insert(id);
                        continue;
                    }
                    if (candidates.size() >= q.candidates)
                        throw Stop{FQIDX_CANDIDATE_LIMIT};
                    work.take(id.size() + 256);
                    at = candidates.emplace(id, Candidate{}).first;
                    ++out.candidates;
                }
                for (const auto &[name, f] : row.fields) {
                    if (!q.scoring_fields.empty() && !q.scoring_fields.contains(name))
                        continue;
                    auto total = st.lengths.find(name);
                    require(total != st.lengths.end() && total->second > 0, FQDB_CORRUPT);
                    double avg = double(total->second) / std::max(uint64_t(1), st.count),
                           tf = double(f.frequency);
                    double score = idf * f.weight * (tf * 2.2) /
                                   (tf + 1.2 * (.25 + .75 * double(f.length) / avg));
                    at->second.score += score;
                    if (at->second.fields.insert(name).second)
                        work.take(name.size() + 96);
                    if (q.explain) {
                        work.take(n.term.size() + name.size() + 160);
                        at->second.details.push_back(
                            {n.term, name, f.frequency, f.length, avg, idf, f.weight, score});
                    }
                }
                at->second.mask |= n.mask;
            }
        }
    } catch (const Stop &s) {
        out.stop = s.reason;
        out.counted = false;
    }
    for (auto &[id, c] : candidates) {
        if (q.all && (c.mask & required) != required)
            continue;
        ++out.total;
        offer(out, {id, c.score, std::move(c.fields), std::move(c.details)}, q.limit);
    }
}
void approximate_search(const Reader &r, const Query &q, const Stats &st, Work &work, Result &out) {
    AnnGraph graph(r, q.base, q.space);
    auto head = graph.head();
    auto contract = vector_contract(r, q.base, q.space);
    if (!contract.enabled || head.entry.empty() || !contract.counted ||
        contract.count != head.active ||
        head.active <= std::max<uint64_t>(1024, uint64_t(q.ef) * 6)) {
        scan_search(r, q, st, work, out, false);
        out.strategy = "dense_exact_small_partition";
        return;
    }
    out.strategy = "hnsw_int8_navigation_fp32_rerank";
    out.ranking = 1;
    out.counted = false;
    auto candidates = graph.search(q.vector, q.ef, work, out.graph, q.scan);
    out.scanned += out.graph;
    if (candidates.size() < std::min<uint64_t>(q.ef, head.active) ||
        (candidates.size() >= q.ef &&
         candidates[q.limit - 1].score - candidates.back().score < 1e-6)) {
        out.ranking = 0;
        out.counted = true;
        scan_search(r, q, st, work, out, false);
        out.strategy = "dense_exact_ambiguous_navigation";
        return;
    }
    for (const auto &item : candidates) {
        work.check();
        if (out.scanned >= q.scan)
            throw Stop{FQIDX_SCAN_LIMIT};
        ++out.scanned;
        auto d = load(r, q.base, item.id, true);
        if (d.deleted || d.space != q.space || d.vector.empty() || !matches(q.where, d.metadata))
            continue;
        require(d.vector.size() == q.vector.size(), FQDB_CORRUPT);
        if (out.candidates >= q.candidates)
            throw Stop{FQIDX_CANDIDATE_LIMIT};
        ++out.candidates;
        double score = vector_dot(d.vector.data(), q.vector.data(), d.vector.size());
        score = std::clamp(score, -1., 1.);
        if (q.minimum && score < *q.minimum)
            continue;
        if (out.best.size() < q.limit)
            work.take(d.id.size() + 256);
        offer(out, {std::move(d.id), score, {}, {}}, q.limit);
    }
    // A selective filter can remove every graph candidate. Prove an empty set
    // using exact search rather than pretending ANN's empty set is exhaustive.
    if (out.best.empty()) {
        out.ranking = 0;
        out.counted = true;
        out.candidates = 0;
        out.total = 0;
        scan_search(r, q, st, work, out, false);
        out.strategy = "dense_exact_empty_ann_validation";
    }
}
std::string result_bytes(const Reader &r, const Query &q, const Stats &st, Work &work,
                         Result &result) {
    if (q.op != FQIDX_QUERY_LIST)
        std::sort(result.best.begin(), result.best.end(), better);
    w::Writer out;
    out.uint_field(2, r.identity.snapshot_version);
    out.uint_field(3, st.sequence);
    out.uint_field(4, result.ranking);
    out.uint_field(5, result.stop);
    out.uint_field(6, 0);
    out.uint_field(7, result.best.empty()
                          ? (result.stop == FQIDX_FINISHED && !result.ranking ? FQIDX_PROVEN_NONE
                                                                              : FQIDX_MATCH_UNKNOWN)
                          : FQIDX_HAS_HITS);
    out.uint_field(8, result.postings);
    out.uint_field(9, result.candidates);
    out.uint_field(10, result.scanned);
    if (!result.cursor.empty())
        out.bytes_field(11, result.cursor);
    if (result.counted && result.stop == FQIDX_FINISHED)
        out.uint_field(12, result.total);
    if (!result.restricted_counts || result.scoped_counts) {
        out.uint_field(13, result.scoped_counts ? result.scoped_counts->first : st.count);
        out.uint_field(14, result.scoped_counts ? result.scoped_counts->second : st.vectors);
    }
    out.bytes_field(15, result.strategy);
    out.bytes_field(16, "fqidx-utf8-v1");
    out.uint_field(17, result.terms);
    out.uint_field(18, result.statistics_scanned);
    out.uint_field(19, result.blocks);
    out.uint_field(20, result.graph);
    out.uint_field(21, st.layout);
    for (const auto &hit : result.best) {
        work.check();
        auto blob = r.get(q.base + "R" + component(hit.id));
        require(blob.found, FQDB_CORRUPT);
        ScopedCharge temporary(work, blob.value.size() * 2 + 4096);
        Document d;
        try {
            d = document(blob.value);
        } catch (const Error &) {
            throw Error{FQDB_CORRUPT};
        }
        require(!d.deleted && d.id == hit.id, FQDB_CORRUPT);
        auto encoded = encode(d);
        work.take(encoded.size() * 2 + 512);
        w::Writer h;
        h.bytes_field(1, encoded);
        h.double_field(2, hit.score);
        if (q.mode)
            h.double_field(3, hit.score);
        for (const auto &f : hit.fields)
            h.bytes_field(4, f);
        if (q.positions)
            for (const auto &p :
                 positions(d, q.text, q.phrases, q.prefix, q.snippet, work, q.scoring_fields)) {
                work.take(p.snippet.size() + p.field.size() + 128);
                w::Writer a;
                a.bytes_field(1, p.field);
                a.uint_field(2, p.begin);
                a.uint_field(3, p.end);
                a.bytes_field(4, p.snippet);
                a.uint_field(5, p.snippet_begin);
                h.bytes_field(5, a.data());
            }
        for (const auto &detail : hit.details) {
            w::Writer a;
            a.bytes_field(1, detail.term);
            a.bytes_field(2, detail.field);
            a.uint_field(3, detail.tf);
            a.uint_field(4, detail.length);
            a.double_field(5, detail.average);
            a.double_field(6, detail.idf);
            a.double_field(7, detail.weight);
            a.double_field(8, detail.contribution);
            h.bytes_field(6, a.data());
        }
        out.bytes_field(1, h.data());
    }
    return out.take();
}
fqdb_status stop_status(uint32_t stop) {
    return stop == FQIDX_CANCELLED  ? FQDB_CANCELLED
           : stop == FQIDX_DEADLINE ? FQDB_TIMEOUT
                                    : FQDB_LIMIT;
}
std::string execute(const Reader &r, const Query &q, const fqidx_query_control *control) {
    if (control)
        require(control->struct_size == sizeof(*control) && control->abi_version == 1);
    Work work;
    work.memory = q.memory;
    work.control = control;
    if (q.timeout) {
        work.timed = true;
        work.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(q.timeout);
    }
    auto st = stats(r, q.base);
    if (q.sequence)
        require(q.sequence == st.sequence, FQDB_CONFLICT);
    Result result;
    result.restricted_counts = q.statistics_policy != 0;
    try {
        work.check();
        if (q.op == FQIDX_QUERY_GET) {
            auto d = load(r, q.base, q.id, true);
            require(!d.deleted, FQDB_NOT_FOUND);
            if (q.expected)
                require(d.version == q.expected, FQDB_CONFLICT);
            require(matches(q.where, d.metadata), FQDB_NOT_FOUND);
            result.best.push_back({q.id, 0, {}, {}});
            result.total = 1;
            result.strategy = "projection_get";
        } else if (q.op == FQIDX_QUERY_STATS) {
            result.strategy = "partition_stats";
        } else if (q.op == FQIDX_QUERY_LIST || q.mode || (q.text.empty() && q.phrases.empty())) {
            if (q.statistics_policy) {
                std::vector<Needle> empty;
                auto visible = filtered_statistics(r, q, st, empty, work, result);
                result.scoped_counts = std::pair{visible.count, visible.vectors};
            }
            bool no_vectors = false;
            if (q.mode) {
                auto c = vector_contract(r, q.base, q.space);
                if (c.dimension)
                    require(c.dimension == q.vector.size(), FQDB_INVALID_ARGUMENT);
                else
                    require(st.vectors == 0, FQDB_UNSUPPORTED);
                no_vectors = st.vectors == 0 || (c.counted && c.count == 0);
            }
            if (no_vectors)
                result.strategy = "empty_vector_space";
            else if (q.mode == FQIDX_DENSE_ANN)
                approximate_search(r, q, st, work, result);
            else
                scan_search(r, q, st, work, result,
                            q.op == FQIDX_QUERY_LIST ||
                                (!q.mode && q.text.empty() && q.phrases.empty()));
        } else
            lexical(r, q, st, work, result);
    } catch (const Stop &s) {
        result.stop = s.reason;
        result.counted = false;
    }
    if (result.stop != FQIDX_FINISHED && !q.partial)
        throw Error{stop_status(result.stop)};
    try {
        return result_bytes(r, q, st, work, result);
    } catch (const Stop &s) {
        if (!q.partial)
            throw Error{stop_status(s.reason)};
        result.best.clear();
        result.stop = s.reason;
        result.counted = false;
        work.used = 0;
        return result_bytes(r, q, st, work, result);
    }
}
struct Maintenance {
    std::string base, term, id, space, key;
    uint32_t op = 1, terms = 8, records = 128;
    uint64_t postings = 20000, memory = 32ull << 20, timeout = 0, sequence = 0;
};
Maintenance maintenance(std::string_view data) {
    Maintenance m;
    std::string space, partition;
    fields(data, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1:
            space = bytes(r, f);
            break;
        case 2:
            partition = bytes(r, f);
            break;
        case 3: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4);
            m.op = uint32_t(n);
            break;
        }
        case 4: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4096);
            m.terms = uint32_t(n);
            break;
        }
        case 5:
            m.postings = number(r, f);
            break;
        case 6:
            m.memory = number(r, f);
            break;
        case 7:
            m.timeout = number(r, f);
            break;
        case 8:
            m.term = bytes(r, f);
            break;
        case 9:
            m.sequence = number(r, f);
            break;
        case 10:
            m.space = bytes(r, f);
            break;
        case 11:
            m.id = bytes(r, f);
            break;
        case 12: {
            auto n = number(r, f);
            require(n >= 1 && n <= 4096);
            m.records = uint32_t(n);
            break;
        }
        case 13:
            m.key = bytes(r, f);
            break;
        default:
            return false;
        }
        return true;
    });
    m.base = root(space, partition);
    require(m.postings && m.memory >= 4096 && m.memory <= (1ull << 32) && m.timeout <= 86400000 &&
            m.term.size() <= 512 && m.id.size() <= 1024 && m.space.size() <= 512 &&
            m.key.size() <= 4096);
    if (m.op == 1)
        require(m.key.empty() && m.space.empty() && (m.id.empty() || !m.term.empty()));
    if (m.op == 2)
        require(m.term.empty() && m.key.empty());
    if (m.op == 3)
        require(m.term.empty() && m.key.empty() && m.space.empty());
    if (m.op == 4)
        require(m.term.empty() && m.id.empty());
    return m;
}
std::string maintain(const Reader &r, Maintenance m, const fqidx_query_control *control) {
    if (control)
        require(control->struct_size == sizeof(*control) && control->abi_version == 1);
    Work work;
    work.memory = m.memory;
    work.control = control;
    if (m.timeout) {
        work.timed = true;
        work.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m.timeout);
    }
    work.check();
    auto st = stats(r, m.base);
    if (m.sequence)
        require(m.sequence == st.sequence, FQDB_CONFLICT);
    auto checkpoint = m.base + "M" + std::to_string(m.op) + component(m.space);
    std::string saved_term, saved_id, saved_key;
    bool saved_complete = true;
    auto saved = r.get(checkpoint);
    if (saved.found)
        fields(saved.value, [&](w::Reader &a, w::Field f) {
            if (f.number == 1)
                saved_term = bytes(a, f);
            else if (f.number == 2)
                saved_id = bytes(a, f);
            else if (f.number == 7)
                saved_complete = number(a, f) != 0;
            else if (f.number == 9)
                saved_key = bytes(a, f);
            else
                return false;
            return true;
        });
    if (m.term.empty() && m.id.empty() && m.key.empty() && !saved_complete) {
        m.term = saved_term;
        m.id = saved_id;
        m.key = saved_key;
    }
    auto contracts = [&] {
        std::vector<std::pair<std::string, VectorContract>> out;
        r.scan(m.base + "V", [&](std::string_view key, std::string_view) {
            work.check();
            auto space = uncomponent(key, m.base.size() + 1);
            if (!m.space.empty() && m.space != space)
                return true;
            work.take(space.size() + 128);
            out.emplace_back(space, vector_contract(r, m.base, space));
            return true;
        });
        return out;
    };
    if (m.op == 2) {
        auto lock = r.get(m.base + "J");
        if (lock.found) {
            std::string scope;
            fields(lock.value, [&](w::Reader &a, w::Field f) {
                if (f.number != 1)
                    return false;
                scope = bytes(a, f);
                return true;
            });
            require(scope == m.space, FQDB_CONFLICT);
            require(saved.found && !saved_complete && m.id == saved_id, FQDB_CONFLICT);
        } else {
            require(m.id.empty(), FQDB_CONFLICT);
            w::Writer lock_value;
            lock_value.bytes_field(1, m.space);
            r.put(m.base + "J", lock_value.data());
            for (auto &[space, c] : contracts()) {
                require(!c.building, FQDB_CORRUPT);
                require(c.generation < UINT64_MAX, FQDB_LIMIT);
                c.building = c.generation + 1;
                save_contract(r, m.base, space, c);
            }
        }
    }
    uint64_t terms = 0, postings = 0, records = 0, reclaimed = 0;
    bool complete = true, graph_published = false;
    std::string last_term, last_id, last_key;
    if (m.op == 1) {
        std::vector<std::string> selected;
        if (!m.id.empty()) {
            require(!saved_complete && m.term == saved_term && m.id == saved_id, FQDB_CONFLICT);
            selected.push_back(m.term);
        }
        r.scan(
            m.base + "F",
            [&](std::string_view key, std::string_view) {
                work.check();
                if (selected.size() == m.terms) {
                    complete = false;
                    return false;
                }
                auto term = uncomponent(key, m.base.size() + 1);
                work.take(term.size() + 64);
                selected.push_back(std::move(term));
                return true;
            },
            m.term);
        for (const auto &term : selected) {
            if (postings >= m.postings || (terms && work.memory - work.used < (4ull << 20))) {
                complete = false;
                break;
            }
            auto progress =
                compact_term(r, m.base, term, work, postings, m.postings,
                             term == m.term ? std::string_view(m.id) : std::string_view());
            last_term = term;
            ++terms;
            if (!progress.finished) {
                complete = false;
                last_id = progress.after;
                require(!last_id.empty(), FQDB_LIMIT);
                break;
            }
        }
    } else if (m.op == 4) {
        auto prefix = m.base + "H";
        require(m.key.empty() || m.key.starts_with(prefix));
        Range input(r, m.key.empty() ? prefix : m.key + std::string(1, '\0'), upper(prefix));
        fqdb_store_row row{};
        std::vector<std::string> remove;
        while (input.next(row)) {
            work.check();
            if (records == m.records) {
                complete = false;
                break;
            }
            ++records;
            auto key = view(row.key);
            last_key = key;
            auto space = uncomponent(key, m.base.size() + 1);
            if (!m.space.empty() && m.space != space)
                continue;
            size_t at = m.base.size() + 1 + component(space).size();
            require(key.size() >= at + 9 && key[at] == 'G', FQDB_CORRUPT);
            uint64_t generation = 0;
            for (size_t i = 1; i <= 8; ++i)
                generation = (generation << 8) | static_cast<unsigned char>(key[at + i]);
            auto c = vector_contract(r, m.base, space);
            if (!c.dimension || generation < c.generation) {
                work.take(key.size() + 64);
                remove.emplace_back(key);
            }
        }
        for (const auto &key : remove) {
            work.check();
            r.erase(key);
            ++reclaimed;
        }
    } else {
        std::vector<Document> documents;
        r.scan(
            m.base + "R",
            [&](std::string_view, std::string_view encoded) {
                work.check();
                if (documents.size() == m.records) {
                    complete = false;
                    return false;
                }
                auto charge = encoded.size() * 3 + 4096;
                if (!documents.empty() && charge > work.memory - work.used) {
                    complete = false;
                    return false;
                }
                work.take(charge);
                auto d = document(encoded, true, m.op == 3);
                documents.push_back(std::move(d));
                return true;
            },
            m.id);
        for (const auto &d : documents) {
            work.check();
            if (m.op == 3 && records && d.terms.size() > m.postings - postings) {
                complete = false;
                break;
            }
            ++records;
            last_id = d.id;
            if (d.deleted)
                continue;
            if (m.op == 2) {
                if (!d.vector.empty() && (m.space.empty() || d.space == m.space)) {
                    auto c = vector_contract(r, m.base, d.space);
                    if (c.building)
                        AnnGraph(r, m.base, d.space, c.building).upsert(d, &work);
                }
            } else {
                for (const auto &[term, fs] : d.terms) {
                    work.check();
                    if (postings >= m.postings)
                        throw Stop{FQIDX_POSTING_LIMIT};
                    ++postings;
                    auto p = posting_at(r, m.base, term, d.id);
                    require(p && p->fields == fs, FQDB_CORRUPT);
                }
                if (!d.vector.empty()) {
                    auto contract = vector_contract(r, m.base, d.space);
                    if (contract.enabled) {
                        auto node = AnnGraph(r, m.base, d.space, contract.generation).get(d.id);
                        auto expected = ann_node(d);
                        require(node.alive && node.generation == d.generation &&
                                    node.quantized == expected.quantized &&
                                    std::abs(node.inverse_norm - expected.inverse_norm) < 1e-15,
                                FQDB_CORRUPT);
                    }
                }
            }
        }
        if (m.op == 2 && complete) {
            for (auto &[space, c] : contracts())
                if (c.building) {
                    graph_published = true;
                    auto head = AnnGraph(r, m.base, space, c.building).head();
                    require(!c.counted || head.active == c.count, FQDB_CORRUPT);
                    c.counted = true;
                    c.enabled = true;
                    c.count = head.active;
                    c.generation = c.building;
                    c.building = 0;
                    save_contract(r, m.base, space, c);
                }
            r.erase(m.base + "J");
        }
    }
    if ((m.op == 1 && terms) || (m.op == 2 && records) || reclaimed || graph_published) {
        require(st.layout < UINT64_MAX, FQDB_LIMIT);
        ++st.layout;
        r.put(m.base + "S", encode(st));
    }
    w::Writer out;
    if (!complete) {
        if (!last_term.empty())
            out.bytes_field(1, last_term);
        if (!last_id.empty())
            out.bytes_field(2, last_id);
        if (!last_key.empty())
            out.bytes_field(9, last_key);
    }
    out.uint_field(3, st.sequence);
    out.uint_field(4, terms);
    out.uint_field(5, postings);
    out.uint_field(6, records);
    out.uint_field(7, complete);
    out.uint_field(8, st.layout);
    out.uint_field(10, reclaimed);
    r.put(checkpoint, out.data());
    return out.take();
}
template <class F> fqdb_status boundary(F fn) noexcept {
    try {
        fn();
        return FQDB_OK;
    } catch (const Error &e) {
        return e.status;
    } catch (const Stop &s) {
        return stop_status(s.reason);
    } catch (const std::bad_alloc &) {
        return FQDB_NO_MEMORY;
    } catch (const std::length_error &) {
        return FQDB_LIMIT;
    } catch (...) {
        return FQDB_INTERNAL_ERROR;
    }
}
} // namespace fengqudb::index::impl

extern "C" {
uint32_t FQDB_CALL fqidx_abi_version(void) {
    return FQIDX_ABI_VERSION;
}
uintptr_t FQDB_CALL fqidx_core_bridge_address(void) {
    return reinterpret_cast<uintptr_t>(&fqdb_store_bridge_abi_version);
}
fqdb_status FQDB_CALL fqidx_txn_apply(fqdb_store_txn *tx, uint64_t token, fqdb_slice request) {
    auto status = fengqudb::index::impl::boundary([&] {
        auto r = fengqudb::index::impl::reader(tx, token);
        auto m = fengqudb::index::impl::mutation(fengqudb::index::impl::view(request));
        fengqudb::index::impl::apply(r, m);
    });
    if (status != FQDB_OK && tx)
        fqdb_store_txn_cancel(tx);
    return status;
}
fqdb_status FQDB_CALL fqidx_snapshot_query(const fqdb_store_snapshot *s, uint64_t token,
                                           fqdb_slice request, const fqidx_query_control *control,
                                           fqdb_buffer *out) {
    return fengqudb::index::impl::boundary([&] {
        fengqudb::index::impl::require(out && !out->data && !out->size);
        auto r = fengqudb::index::impl::reader(s, token);
        auto q = fengqudb::index::impl::query(fengqudb::index::impl::view(request));
        auto result = fengqudb::index::impl::execute(r, q, control);
        fengqudb::index::impl::check(fqdb_buffer_copy(fengqudb::index::impl::slice(result), out));
    });
}
fqdb_status FQDB_CALL fqidx_txn_query(fqdb_store_txn *tx, uint64_t token, fqdb_slice request,
                                      const fqidx_query_control *control, fqdb_buffer *out) {
    return fengqudb::index::impl::boundary([&] {
        fengqudb::index::impl::require(out && !out->data && !out->size);
        auto r = fengqudb::index::impl::reader(tx, token);
        auto q = fengqudb::index::impl::query(fengqudb::index::impl::view(request));
        auto result = fengqudb::index::impl::execute(r, q, control);
        fengqudb::index::impl::check(fqdb_buffer_copy(fengqudb::index::impl::slice(result), out));
    });
}
fqdb_status FQDB_CALL fqidx_txn_maintain(fqdb_store_txn *tx, uint64_t token, fqdb_slice request,
                                         const fqidx_query_control *control, fqdb_buffer *out) {
    auto status = fengqudb::index::impl::boundary([&] {
        using namespace fengqudb::index::impl;
        require(out && !out->data && !out->size);
        auto r = reader(tx, token);
        auto m = maintenance(view(request));
        auto result = maintain(r, std::move(m), control);
        check(fqdb_buffer_copy(slice(result), out));
    });
    if (status != FQDB_OK && tx)
        fqdb_store_txn_cancel(tx);
    return status;
}
}
