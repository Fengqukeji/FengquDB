#pragma once
#include "model.hpp"
#include <cctype>
namespace fengqudb::index::impl {
struct Rune {
    uint32_t cp;
    size_t begin, end;
};
inline std::vector<Rune> runes(std::string_view text) {
    std::vector<Rune> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t b = i;
        uint32_t c = static_cast<uint8_t>(text[i++]);
        unsigned n = 0;
        uint32_t minimum = 0;
        if (c < 128) {
        } else if (c >= 0xc2 && c <= 0xdf) {
            c &= 31;
            n = 1;
            minimum = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            c &= 15;
            n = 2;
            minimum = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            c &= 7;
            n = 3;
            minimum = 65536;
        } else
            throw Error{FQDB_INVALID_ARGUMENT};
        require(n <= text.size() - i);
        for (unsigned j = 0; j < n; ++j) {
            auto a = static_cast<uint8_t>(text[i++]);
            require((a & 0xc0) == 0x80);
            c = (c << 6) | (a & 63);
        }
        require(c >= minimum && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff));
        out.push_back({c, b, i});
    }
    return out;
}
inline void append_utf8(std::string &s, uint32_t c) {
    if (c < 128)
        s.push_back(static_cast<char>(c));
    else if (c < 2048) {
        s.push_back(static_cast<char>(0xc0 | (c >> 6)));
        s.push_back(static_cast<char>(0x80 | (c & 63)));
    } else if (c < 65536) {
        s.push_back(static_cast<char>(0xe0 | (c >> 12)));
        s.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        s.push_back(static_cast<char>(0x80 | (c & 63)));
    } else {
        s.push_back(static_cast<char>(0xf0 | (c >> 18)));
        s.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
        s.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        s.push_back(static_cast<char>(0x80 | (c & 63)));
    }
}
inline uint32_t fold(uint32_t cp) {
    if (cp >= 0xff01 && cp <= 0xff5e)
        cp -= 0xfee0;
    if (cp >= 'A' && cp <= 'Z')
        cp += 32;
    return cp;
}
inline bool cjk(uint32_t c) {
    return (c >= 0x3400 && c <= 0x9fff) || (c >= 0x20000 && c <= 0x323af);
}
inline bool word(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           (c >= 128 && !cjk(c) && !(c >= 0x2000 && c <= 0x206f) && !(c >= 0x3000 && c <= 0x303f));
}
inline std::string exact_term(std::string_view text) {
    if (text.empty() || text.size() > 256)
        return {};
    std::string out(1, '\x01');
    for (auto r : runes(text))
        append_utf8(out, fold(r.cp));
    return out;
}
inline std::vector<std::string> tokens(std::string_view text, size_t limit = 65536) {
    auto rs = runes(text);
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        if (!s.empty() && s.size() <= 128) {
            require(out.size() < limit, FQDB_LIMIT);
            out.push_back(std::move(s));
        }
    };
    for (size_t i = 0; i < rs.size();) {
        auto cp = fold(rs[i].cp);
        if (cjk(cp)) {
            std::string a;
            append_utf8(a, cp);
            add(a);
            if (i + 1 < rs.size() && cjk(fold(rs[i + 1].cp))) {
                append_utf8(a, fold(rs[i + 1].cp));
                add(a);
            }
            ++i;
            continue;
        }
        if (!word(cp)) {
            ++i;
            continue;
        }
        size_t start = i;
        std::string whole;
        while (i < rs.size() && word(fold(rs[i].cp))) {
            append_utf8(whole, fold(rs[i].cp));
            ++i;
        }
        add(whole);
        // Whole identifiers retain error codes; pieces support snake/camel lookup.
        std::string part;
        std::vector<std::string> pieces;
        for (size_t j = start; j < i; ++j) {
            auto c = fold(rs[j].cp);
            bool boundary = j > start && rs[j].cp >= 'A' && rs[j].cp <= 'Z' &&
                            rs[j - 1].cp >= 'a' && rs[j - 1].cp <= 'z';
            if (c == '_' || boundary) {
                if (!part.empty()) {
                    pieces.push_back(std::move(part));
                    part.clear();
                }
                if (c == '_')
                    continue;
            }
            append_utf8(part, c);
        }
        if (!part.empty())
            pieces.push_back(std::move(part));
        if (pieces.size() > 1)
            for (auto &p : pieces)
                add(std::move(p));
    }
    return out;
}
inline void analyze(Document &d) {
    d.terms.clear();
    d.lengths.clear();
    size_t total = 0;
    for (const auto &t : d.texts) {
        (void)runes(t.name);
        require(total + t.text.size() <= 1048576, FQDB_LIMIT);
        total += t.text.size();
        auto ts = tokens(t.text);
        if (t.name == "name" || t.name == "path" || t.name == "id" || t.name == "exact") {
            auto e = exact_term(t.text);
            if (!e.empty())
                ts.push_back(std::move(e));
        }
        d.lengths[t.name] = ts.size();
        for (const auto &term : ts) {
            auto &f = d.terms[term][t.name];
            ++f.frequency;
            f.length = ts.size();
            f.weight = t.weight;
            require(d.terms.size() <= 8192, FQDB_LIMIT);
        }
    }
    for (const auto &[name, v] : d.metadata) {
        (void)runes(name);
        if (v.kind == 1)
            (void)runes(v.data);
    }
}
inline void validate(const Predicate &p) {
    (void)runes(p.field);
    for (const auto &value : p.values)
        if (value.kind == 1)
            (void)runes(value.data);
    for (const auto &child : p.children)
        validate(child);
}
} // namespace fengqudb::index::impl
