#pragma once
// Prefix-compressed immutable posting blocks plus one latest delta per ID.
// Compaction is caller-transactional; no separate log or sidecar files.
#include "storage.hpp"
#include <functional>
#include <memory>

namespace fengqudb::index::impl {
struct Posting {
    std::string id;
    TermFields fields;
    bool deleted = false;
};
inline Posting posting(std::string_view data) {
    Posting p;
    fields(data, [&](w::Reader &r, w::Field f) {
        if (f.number == 1)
            p.id = bytes(r, f);
        else if (f.number == 3) {
            auto n = number(r, f);
            require(n <= 1, FQDB_CORRUPT);
            p.deleted = n != 0;
        } else
            return false;
        return true;
    });
    require(!p.id.empty() && p.id.size() <= 1024, FQDB_CORRUPT);
    if (!p.deleted) {
        try {
            p.fields = term_fields(data);
        } catch (const Error &) {
            throw Error{FQDB_CORRUPT};
        }
        require(!p.fields.empty() && p.fields.size() <= 32, FQDB_CORRUPT);
    }
    return p;
}
inline std::string posting_tombstone(std::string_view id) {
    w::Writer w;
    w.bytes_field(1, id);
    w.uint_field(3, 1);
    return w.take();
}
inline std::string encode_block(const std::vector<Posting> &rows) {
    require(!rows.empty() && rows.size() <= 128);
    std::map<std::string, uint64_t> dictionary;
    for (const auto &p : rows)
        for (const auto &[name, f] : p.fields) {
            (void)f;
            dictionary.try_emplace(name, 0);
        }
    require(dictionary.size() <= 128, FQDB_LIMIT);
    w::Writer out;
    out.uint_field(1, 1);
    uint64_t ordinal = 0;
    for (auto &[name, index] : dictionary) {
        index = ordinal++;
        out.bytes_field(2, name);
    }
    TermFields bounds;
    for (const auto &p : rows)
        for (const auto &[name, f] : p.fields) {
            auto [at, created] = bounds.try_emplace(name, TermField{0, UINT64_MAX, 0});
            (void)created;
            at->second.frequency = std::max(at->second.frequency, f.frequency);
            at->second.length = std::min(at->second.length, f.length);
            at->second.weight = std::max(at->second.weight, f.weight);
        }
    out.bytes_field(4, encode_term("", bounds));
    std::string previous;
    for (const auto &p : rows) {
        size_t common = 0;
        while (common < previous.size() && common < p.id.size() && previous[common] == p.id[common])
            ++common;
        w::Writer row;
        row.uint_field(1, common);
        row.bytes_field(2, std::string_view(p.id).substr(common));
        for (const auto &[name, f] : p.fields) {
            w::Writer a;
            a.uint_field(1, dictionary.at(name));
            a.uint_field(2, f.frequency);
            a.uint_field(3, f.length);
            a.double_field(4, f.weight);
            row.bytes_field(3, a.data());
        }
        out.bytes_field(3, row.data());
        previous = p.id;
    }
    return out.take();
}
inline std::vector<Posting> decode_block(std::string_view data) {
    require(data.size() <= 1048576, FQDB_CORRUPT);
    std::vector<Posting> rows;
    std::vector<std::string> names;
    std::string previous;
    fields(data, [&](w::Reader &r, w::Field f) {
        if (f.number == 1)
            require(number(r, f) == 1, FQDB_CORRUPT);
        else if (f.number == 2) {
            auto name = bytes(r, f);
            require(!name.empty() && name.size() <= 128 && names.size() < 128, FQDB_CORRUPT);
            names.emplace_back(name);
        } else if (f.number == 3) {
            require(rows.size() < 128, FQDB_CORRUPT);
            Posting p;
            uint64_t common = 0;
            std::string suffix;
            fields(bytes(r, f), [&](w::Reader &a, w::Field q) {
                if (q.number == 1)
                    common = number(a, q);
                else if (q.number == 2)
                    suffix = bytes(a, q);
                else if (q.number == 3) {
                    uint64_t index = UINT64_MAX;
                    TermField term;
                    fields(bytes(a, q), [&](w::Reader &b, w::Field z) {
                        if (z.number == 1)
                            index = number(b, z);
                        else if (z.number == 2)
                            term.frequency = number(b, z);
                        else if (z.number == 3)
                            term.length = number(b, z);
                        else if (z.number == 4)
                            term.weight = real(b, z);
                        else
                            return false;
                        return true;
                    });
                    require(index < names.size() && term.frequency &&
                                term.frequency <= term.length && term.weight > 0 &&
                                term.weight <= 1000000 && p.fields.size() < 32,
                            FQDB_CORRUPT);
                    require(p.fields.emplace(names[size_t(index)], term).second, FQDB_CORRUPT);
                } else
                    return false;
                return true;
            });
            require(common <= previous.size() && common + suffix.size() <= 1024, FQDB_CORRUPT);
            p.id = previous.substr(0, size_t(common)) + suffix;
            require(!p.id.empty() && (rows.empty() || p.id > previous) && !p.fields.empty(),
                    FQDB_CORRUPT);
            previous = p.id;
            rows.push_back(std::move(p));
        } else
            return false;
        return true;
    });
    require(!rows.empty(), FQDB_CORRUPT);
    return rows;
}
class Range {
    fqdb_store_cursor *cursor_ = nullptr;

  public:
    Range(const Reader &r, std::string_view start, std::string_view end, bool reverse = false) {
        check(
            r.tx ? fqdb_store_txn_cursor_open(r.tx, slice(start), slice(end), reverse, &cursor_)
                 : fqdb_store_cursor_open(r.snapshot, slice(start), slice(end), reverse, &cursor_));
    }
    Range(const Range &) = delete;
    ~Range() {
        fqdb_store_cursor_close(cursor_);
    }
    bool next(fqdb_store_row &row) {
        auto s = fqdb_store_cursor_next(cursor_, &row);
        if (s == FQDB_END)
            return false;
        check(s);
        return true;
    }
};
class PostingStream {
    Work &work_;
    uint64_t &visited_;
    uint64_t &blocks_;
    uint64_t budget_;
    Range deltas_, blocks_range_;
    std::optional<Posting> delta_;
    std::vector<Posting> block_;
    size_t at_ = 0;
    uint64_t charge_ = 0;
    std::function<bool(std::string_view, std::string_view)> skip_;
    std::string last_;
    bool bounded_step_ = false;
    static std::string start(const std::string &prefix, std::string_view after) {
        return after.empty() ? prefix : prefix + component(after) + std::string(1, '\0');
    }
    void delta_next() {
        work_.check();
        fqdb_store_row row{};
        if (!deltas_.next(row)) {
            delta_.reset();
            return;
        }
        delta_ = posting(view(row.value));
    }
    void block_next() {
        work_.check();
        block_.clear();
        at_ = 0;
        work_.release(charge_);
        charge_ = 0;
        fqdb_store_row row{};
        for (;;) {
            if (!blocks_range_.next(row))
                return;
            work_.check();
            ++blocks_;
            if (!skip_ || !skip_(view(row.key), view(row.value)))
                break;
        }
        auto b = view(row.value);
        auto charged = b.size() * 6 + 16384;
        work_.take(charged);
        charge_ = charged;
        block_ = decode_block(b);
    }

  public:
    PostingStream(const Reader &r, const std::string &base, std::string_view term, Work &w,
                  uint64_t &visited, uint64_t &blocks, uint64_t budget,
                  std::function<bool(std::string_view, std::string_view)> skip = {},
                  std::string_view after = {}, bool bounded_step = false)
        : work_(w), visited_(visited), blocks_(blocks), budget_(budget),
          deltas_(r, start(base + "P" + component(term), after),
                  upper(base + "P" + component(term))),
          blocks_range_(r, start(base + "B" + component(term), after),
                        upper(base + "B" + component(term))),
          skip_(std::move(skip)), bounded_step_(bounded_step) {
        try {
            delta_next();
            block_next();
        } catch (...) {
            work_.release(charge_);
            charge_ = 0;
            throw;
        }
    }
    ~PostingStream() {
        work_.release(charge_);
    }
    bool next(Posting &out) {
        for (;;) {
            work_.check();
            if (bounded_step_ && visited_ >= budget_ && (!block_.empty() || delta_))
                throw Stop{FQIDX_POSTING_LIMIT};
            if (at_ == block_.size() && !block_.empty())
                block_next();
            bool b = at_ < block_.size();
            if (!b && !delta_)
                return false;
            if (visited_ >= budget_)
                throw Stop{FQIDX_POSTING_LIMIT};
            ++visited_;
            if (delta_ && (!b || delta_->id <= block_[at_].id)) {
                if (b && delta_->id == block_[at_].id)
                    ++at_;
                out = std::move(*delta_);
                delta_next();
            } else
                out = std::move(block_[at_++]);
            last_ = out.id;
            if (!out.deleted)
                return true;
        }
    }
    const std::string &last_id() const {
        return last_;
    }
};
inline std::optional<Posting> posting_at(const Reader &r, const std::string &base,
                                         std::string_view term, std::string_view id) {
    auto b = r.get(base + "P" + component(term) + component(id));
    if (b.found) {
        auto p = posting(b.value);
        if (p.deleted)
            return {};
        return p;
    }
    auto prefix = base + "B" + component(term);
    Range range(r, prefix, prefix + component(id) + std::string(1, '\0'), true);
    fqdb_store_row row{};
    if (!range.next(row))
        return {};
    auto rows = decode_block(view(row.value));
    auto it = std::lower_bound(rows.begin(), rows.end(), id,
                               [](const Posting &p, std::string_view n) { return p.id < n; });
    if (it == rows.end() || it->id != id)
        return {};
    return std::move(*it);
}
inline std::string uncomponent(std::string_view key, size_t begin) {
    std::string out;
    for (size_t i = begin; i < key.size(); ++i) {
        auto c = static_cast<unsigned char>(key[i]);
        if (c)
            out.push_back(static_cast<char>(c));
        else {
            require(i + 1 < key.size(), FQDB_CORRUPT);
            auto next = static_cast<unsigned char>(key[++i]);
            if (next == 0)
                return out;
            require(next == 255, FQDB_CORRUPT);
            out.push_back('\0');
        }
    }
    throw Error{FQDB_CORRUPT};
}
struct CompactProgress {
    bool finished = true;
    std::string after;
};
inline void drop_empty_term(const Reader &r, const std::string &base, const std::string &term) {
    if (df(r, base, term))
        return;
    bool any = false;
    for (const char *kind : {"P", "B"}) {
        r.scan(base + kind + component(term), [&](std::string_view, std::string_view) {
            any = true;
            return false;
        });
        if (any)
            return;
    }
    r.erase(base + "F" + component(term));
}
inline CompactProgress compact_term(const Reader &r, const std::string &base,
                                    const std::string &term, Work &work, uint64_t &visited,
                                    uint64_t budget, std::string_view after = {}) {
    std::vector<std::pair<std::string, std::string>> blocks;
    std::vector<std::string> old_keys;
    CompactProgress progress;
    uint64_t block_count = 0;
    uint64_t count = std::max<uint64_t>(128, (work.memory - work.used) / 16384);
    uint64_t limit = visited + std::min(count, budget - visited);
    {
        PostingStream input(r, base, term, work, visited, block_count, limit, {}, after, true);
        std::vector<Posting> rows;
        Posting p;
        auto flush = [&] {
            if (rows.empty())
                return;
            auto value = encode_block(rows);
            auto key = base + "B" + component(term) + component(rows.front().id);
            work.take(value.size() * 2 + key.size() + 128);
            blocks.emplace_back(std::move(key), std::move(value));
            rows.clear();
        };
        try {
            while (input.next(p)) {
                rows.push_back(std::move(p));
                if (rows.size() == 128)
                    flush();
            }
        } catch (const Stop &stop) {
            if (stop.reason != FQIDX_POSTING_LIMIT)
                throw;
            progress.finished = false;
        }
        progress.after = input.last_id();
        flush();
    }
    if (progress.after.empty()) {
        require(progress.finished, FQDB_LIMIT);
        drop_empty_term(r, base, term);
        return progress;
    }
    // Remove only the processed key interval. If its boundary cuts an old block,
    // preserve that block's untouched suffix as one compressed tail block.
    std::vector<Posting> tail;
    r.scan(
        base + "B" + component(term),
        [&](std::string_view key, std::string_view value) {
            work.check();
            auto first = uncomponent(key, base.size() + 1 + component(term).size());
            if (first > progress.after)
                return false;
            work.take(key.size() + 64);
            old_keys.emplace_back(key);
            for (auto &p : decode_block(value))
                if (p.id > progress.after) {
                    require(tail.size() < 128, FQDB_CORRUPT);
                    tail.push_back(std::move(p));
                }
            return true;
        },
        after);
    if (!tail.empty()) {
        auto value = encode_block(tail);
        auto key = base + "B" + component(term) + component(tail.front().id);
        work.take(value.size() * 2 + key.size() + 128);
        blocks.emplace_back(std::move(key), std::move(value));
    }
    r.scan(
        base + "P" + component(term),
        [&](std::string_view key, std::string_view value) {
            work.check();
            auto p = posting(value);
            if (p.id > progress.after)
                return false;
            work.take(key.size() + 64);
            old_keys.emplace_back(key);
            return true;
        },
        after);
    for (const auto &key : old_keys) {
        work.check();
        r.erase(key);
    }
    for (const auto &[key, value] : blocks) {
        work.check();
        r.put(key, value);
    }
    if (progress.finished)
        drop_empty_term(r, base, term);
    return progress;
}
} // namespace fengqudb::index::impl
