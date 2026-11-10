#pragma once
#include "cursor.hpp"
#include <algorithm>
#include <cmath>

namespace fengqudb::paged {
inline Bytes component(std::string_view value) {
    Bytes out;
    auto zero = value.find('\0');
    if (zero == std::string_view::npos) {
        out.reserve(value.size() + 2);
        out.assign(value);
    } else {
        out.reserve(value.size() +
                    static_cast<size_t>(std::count(value.begin(), value.end(), '\0')) + 2);
        out.append(value.substr(0, zero));
        for (size_t i = zero; i < value.size(); ++i) {
            out.push_back(value[i]);
            if (!value[i])
                out.push_back(static_cast<char>(255));
        }
    }
    out.append(2, '\0');
    return out;
}
inline Bytes uncomponent(std::string_view value) {
    Bytes out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i])
            out.push_back(value[i]);
        else {
            require(++i < value.size());
            if (!value[i]) {
                require(i + 1 == value.size());
                return out;
            }
            require(static_cast<unsigned char>(value[i]) == 255);
            out.push_back(0);
        }
    }
    throw Error(FQDB_CORRUPT);
}
inline Bytes big_endian(uint64_t value) {
    Bytes out(8, '\0');
    for (unsigned i = 0; i < 8; ++i)
        out[i] = static_cast<char>(value >> (56 - 8 * i));
    return out;
}
inline uint64_t from_big(std::string_view value) {
    require(value.size() == 8);
    uint64_t out = 0;
    for (unsigned char c : value)
        out = (out << 8) | c;
    return out;
}
inline Bytes score_key(double score) {
    require(std::isfinite(score), FQDB_INVALID_ARGUMENT);
    if (score == 0)
        score = 0; // canonicalize negative zero
    uint64_t bits;
    std::memcpy(&bits, &score, 8);
    return big_endian(bits & (UINT64_C(1) << 63) ? ~bits : bits ^ (UINT64_C(1) << 63));
}
inline double decode_score(std::string_view bytes) {
    auto bits = from_big(bytes);
    bits = bits & (UINT64_C(1) << 63) ? bits ^ (UINT64_C(1) << 63) : ~bits;
    double value;
    std::memcpy(&value, &bits, 8);
    require(std::isfinite(value));
    return value;
}
} // namespace fengqudb::paged
