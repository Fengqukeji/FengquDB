#pragma once
#include "select_regex.hpp"
namespace fengqudb::paged::select {
inline bool utf8(std::string_view text, size_t &at, uint32_t &point) {
    if (at == text.size())
        return false;
    auto c = static_cast<unsigned char>(text[at++]);
    if (c < 128) {
        point = c;
        return true;
    }
    unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 0;
    if (!count || count > text.size() - at)
        return false;
    point = c & ((1u << (6 - count)) - 1);
    for (unsigned i = 0; i < count; ++i) {
        auto b = static_cast<unsigned char>(text[at++]);
        if ((b & 0xc0) != 0x80)
            return false;
        point = (point << 6) | (b & 63);
    }
    return point >= (count == 1   ? 0x80u
                     : count == 2 ? 0x800u
                                  : 0x10000u) &&
           point <= 0x10ffff && !(point >= 0xd800 && point <= 0xdfff);
}
inline std::vector<uint32_t> fuzzy_pattern(std::string_view text) {
    std::vector<uint32_t> out;
    size_t at = 0;
    while (at < text.size()) {
        uint32_t c = 0;
        require(utf8(text, at, c), FQDB_INVALID_ARGUMENT);
        require(out.size() < 1024, FQDB_LIMIT);
        out.push_back(c);
    }
    return out;
}
inline bool fuzzy(std::string_view input, const std::vector<uint32_t> &pattern, unsigned distance,
                  Work &w, Pattern::Scratch &s) {
    const size_t n = pattern.size();
    if (input.size() > 4 * (n + distance) || input.size() + distance < n)
        return false;
    w.tick(n + 1);
    constexpr int infinity = 100000;
    auto &previous = s.current;
    auto &next = s.next;
    previous.assign(n + 1, infinity);
    next.assign(n + 1, infinity);
    for (size_t j = 0; j <= std::min<size_t>(n, distance); ++j)
        previous[j] = int(j);
    size_t at = 0, i = 0;
    while (at < input.size()) {
        uint32_t c = 0;
        if (!utf8(input, at, c))
            return false;
        if (++i > n + distance)
            return false;
        w.tick();
        size_t low = i > distance ? i - distance : 1, high = std::min(n, i + distance);
        next[0] = i <= distance ? int(i) : infinity;
        if (low > 1)
            next[low - 1] = infinity;
        if (high < n)
            next[high + 1] = infinity;
        int smallest = next[0];
        for (size_t j = low; j <= high; ++j) {
            w.tick();
            next[j] = std::min({previous[j] + 1, next[j - 1] + 1,
                                previous[j - 1] + (pattern[j - 1] == c ? 0 : 1)});
            smallest = std::min(smallest, next[j]);
        }
        if (smallest > int(distance))
            return false;
        previous.swap(next);
    }
    return previous[n] <= int(distance);
}
} // namespace fengqudb::paged::select
