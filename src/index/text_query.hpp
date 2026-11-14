#pragma once
#include "storage.hpp"

namespace fengqudb::index::impl {
struct PrimaryToken {
    std::string term;
    uint32_t begin = 0, end = 0, position = 0;
};
inline Rune read_rune(std::string_view text, size_t &at) {
    size_t begin = at;
    require(at < text.size());
    uint32_t cp = static_cast<unsigned char>(text[at++]);
    unsigned count = 0;
    uint32_t minimum = 0;
    if (cp < 128) {
    } else if (cp >= 0xc2 && cp <= 0xdf) {
        cp &= 31;
        count = 1;
        minimum = 128;
    } else if (cp >= 0xe0 && cp <= 0xef) {
        cp &= 15;
        count = 2;
        minimum = 2048;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
        cp &= 7;
        count = 3;
        minimum = 65536;
    } else
        throw Error{FQDB_INVALID_ARGUMENT};
    require(count <= text.size() - at);
    for (unsigned i = 0; i < count; ++i) {
        auto c = static_cast<unsigned char>(text[at++]);
        require((c & 0xc0) == 0x80);
        cp = (cp << 6) | (c & 63);
    }
    require(cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff));
    return {cp, begin, at};
}
template <class F> bool primary_each(std::string_view text, F fn) {
    PrimaryToken token;
    bool pending = false, oversized = false;
    uint32_t position = 0;
    auto flush = [&] {
        if (!pending)
            return true;
        token.position = position++;
        pending = false;
        bool keep = oversized || fn(token);
        token.term.clear();
        oversized = false;
        return keep;
    };
    for (size_t at = 0; at < text.size();) {
        auto rune = read_rune(text, at);
        auto cp = fold(rune.cp);
        if (cjk(cp)) {
            if (!flush())
                return false;
            token.begin = uint32_t(rune.begin);
            token.end = uint32_t(rune.end);
            token.position = position++;
            append_utf8(token.term, cp);
            if (!fn(token))
                return false;
            token.term.clear();
        } else if (word(cp)) {
            if (!pending) {
                token.begin = uint32_t(rune.begin);
                pending = true;
            }
            token.end = uint32_t(rune.end);
            if (!oversized) {
                append_utf8(token.term, cp);
                if (token.term.size() > 128) {
                    oversized = true;
                    token.term.clear();
                }
            }
        } else if (!flush())
            return false;
    }
    return flush();
}
struct Phrase {
    std::string text, field;
    uint32_t gap = 0;
    std::vector<std::string> terms;
};
inline Phrase phrase(std::string_view data) {
    Phrase p;
    fields(data, [&](w::Reader &r, w::Field f) {
        if (f.number == 1)
            p.text = bytes(r, f);
        else if (f.number == 2) {
            auto n = number(r, f);
            require(n <= 32);
            p.gap = uint32_t(n);
        } else if (f.number == 3)
            p.field = bytes(r, f);
        else
            return false;
        return true;
    });
    require(!p.text.empty() && p.text.size() <= 1024 && p.field.size() <= 128);
    (void)runes(p.field);
    primary_each(p.text, [&](const PrimaryToken &t) {
        require(p.terms.size() < 32, FQDB_LIMIT);
        p.terms.push_back(t.term);
        return true;
    });
    require(!p.terms.empty());
    return p;
}
struct Position {
    std::string field, snippet;
    uint64_t begin = 0, end = 0, snippet_begin = 0;
};
inline std::optional<std::pair<uint32_t, uint32_t>> phrase_position(std::string_view text,
                                                                    const Phrase &p, Work &work) {
    struct State {
        uint32_t end = UINT32_MAX, begin = 0;
    };
    std::vector<State> states(p.terms.size());
    std::optional<std::pair<uint32_t, uint32_t>> result;
    primary_each(text, [&](const PrimaryToken &t) {
        if ((t.position & 127) == 0)
            work.check();
        for (size_t i = p.terms.size(); i > 0; --i) {
            size_t at = i - 1;
            if (t.term != p.terms[at])
                continue;
            if (!at)
                states[at] = {t.position, t.begin};
            else if (states[at - 1].end != UINT32_MAX && t.position > states[at - 1].end &&
                     t.position - states[at - 1].end <= uint64_t(p.gap) + 1)
                states[at] = {t.position, states[at - 1].begin};
            if (at == p.terms.size() - 1 && states[at].end == t.position) {
                result = std::pair{states[at].begin, t.end};
                return false;
            }
        }
        return true;
    });
    return result;
}
inline bool phrase_matches(const Document &d, const std::vector<Phrase> &phrases, Work &work,
                           const std::set<std::string> &allowed = {}) {
    for (const auto &p : phrases) {
        bool found = false;
        for (const auto &t : d.texts)
            if ((allowed.empty() || allowed.contains(t.name)) &&
                (p.field.empty() || p.field == t.name) && phrase_position(t.text, p, work)) {
                found = true;
                break;
            }
        if (!found)
            return false;
    }
    return true;
}
inline Position position(std::string_view name, std::string_view text, uint64_t begin, uint64_t end,
                         uint32_t budget) {
    Position p;
    p.field = name;
    p.begin = begin;
    p.end = end;
    size_t first = begin > budget / 3 ? size_t(begin - budget / 3) : 0;
    while (first && ((static_cast<unsigned char>(text[first]) & 0xc0) == 0x80))
        --first;
    size_t last = std::min(text.size(), first + budget);
    while (last > first && last < text.size() &&
           ((static_cast<unsigned char>(text[last]) & 0xc0) == 0x80))
        --last;
    p.snippet_begin = first;
    p.snippet = text.substr(first, last - first);
    return p;
}
inline std::vector<Position> positions(const Document &d, std::string_view query,
                                       const std::vector<Phrase> &phrases, bool prefix,
                                       uint32_t snippet, Work &work,
                                       const std::set<std::string> &allowed = {}) {
    std::set<std::string> terms;
    std::string last;
    for (auto &t : tokens(query, 256))
        terms.insert(std::move(t));
    primary_each(query, [&](const PrimaryToken &t) {
        last = t.term;
        return true;
    });
    std::vector<Position> found;
    for (const auto &text : d.texts) {
        if (!allowed.empty() && !allowed.contains(text.name))
            continue;
        for (const auto &p : phrases)
            if (p.field.empty() || p.field == text.name) {
                auto span = phrase_position(text.text, p, work);
                if (span && found.size() < 16)
                    found.push_back(
                        position(text.name, text.text, span->first, span->second, snippet));
            }
        if (found.size() >= 16)
            break;
        primary_each(text.text, [&](const PrimaryToken &t) {
            if ((t.position & 127) == 0)
                work.check();
            bool match =
                terms.contains(t.term) || (prefix && !last.empty() && t.term.starts_with(last));
            if (!match && t.term.size() > 1)
                for (const auto &piece :
                     tokens(std::string_view(text.text).substr(t.begin, t.end - t.begin), 64))
                    if (terms.contains(piece)) {
                        match = true;
                        break;
                    }
            if (match)
                found.push_back(position(text.name, text.text, t.begin, t.end, snippet));
            return found.size() < 16;
        });
    }
    return found;
}
} // namespace fengqudb::index::impl
