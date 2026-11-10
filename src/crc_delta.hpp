#pragma once
#include "storage.hpp"

namespace fengqudb::storage {
inline uint32_t crc_matrix_times(const uint32_t *matrix, uint32_t value) noexcept {
    uint32_t sum = 0;
    for (unsigned bit = 0; value; ++bit, value >>= 1)
        if (value & 1)
            sum ^= matrix[bit];
    return sum;
}
inline uint32_t crc32_unshift(uint32_t value, uint64_t length) noexcept {
    static const auto powers = [] {
        std::array<std::array<uint32_t, 32>, 64> result{};
        for (unsigned n = 0; n < 32; ++n) {
            uint32_t v = uint32_t(1) << n;
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto low = v >> 31;
                v = ((v ^ (low ? 0xedb88320u : 0u)) << 1) | low;
            }
            result[0][n] = v;
        }
        for (size_t i = 1; i < result.size(); ++i)
            for (unsigned n = 0; n < 32; ++n)
                result[i][n] = crc_matrix_times(result[i - 1].data(), result[i - 1][n]);
        return result;
    }();
    for (unsigned bit = 0; length; ++bit, length >>= 1)
        if (length & 1)
            value = crc_matrix_times(powers[bit].data(), value);
    return value;
}
// Build an operator only after its length repeats. Random patch lengths keep
// the logarithmic path; recurring small-field edits reuse one bounded matrix.
inline uint32_t crc32_shift(uint32_t value, uint64_t length, bool inverse = false) noexcept {
    if (!inverse)
        return crc32_combine(value, 0, length);
    if (!length || !value)
        return value;
    struct Entry {
        uint64_t length = 0;
        std::array<uint32_t, 32> matrix{};
        bool inverse = false, built = false;
    };
    thread_local std::array<Entry, 4> entries{};
    thread_local unsigned replace = 0;
    auto apply = [&](uint32_t v) {
        return inverse ? crc32_unshift(v, length) : crc32_combine(v, 0, length);
    };
    for (auto &e : entries)
        if (e.length == length && e.inverse == inverse) {
            if (!e.built) {
                for (unsigned n = 0; n < 32; ++n)
                    e.matrix[n] = apply(uint32_t(1) << n);
                e.built = true;
            }
            return crc_matrix_times(e.matrix.data(), value);
        }
    auto &e = entries[replace++ % entries.size()];
    e.length = length;
    e.inverse = inverse;
    e.built = false;
    return apply(value);
}
// Called before moving/cropping bytes. Zero means defer one full scan; the
// high bit marker distinguishes a known CRC of zero from an unknown CRC.
inline uint64_t patched_crc(std::string_view old, uint64_t cached, size_t offset, size_t remove,
                            std::string_view insert) noexcept {
    constexpr uint64_t known = UINT64_C(1) << 32;
    const auto next = old.size() - remove + insert.size();
    if (!next)
        return known;
    if (!cached)
        return 0;
    const auto tail = old.size() - offset - remove;
    auto crc = static_cast<uint32_t>(cached);
    if (!remove && !tail)
        return known | crc32(insert.data(), insert.size(), crc);
    // Tiny values are cheaper to scan than to apply GF(2) operators.
    if (next < 512)
        return 0;
    const auto work = remove + insert.size();
    if (remove == insert.size() && work + 128 < next) {
        auto delta = crc32(old.data() + offset, remove) ^ crc32(insert.data(), insert.size());
        return known | (crc ^ crc32_shift(delta, tail));
    }
    if (std::min(offset, tail) + work + 256 >= next)
        return 0; // Large deletions are cheaper to checksum after cropping.
    if (offset <= tail) {
        auto prefix = crc32(old.data(), offset);
        auto before = crc32(old.data() + offset, remove, prefix);
        auto after = crc32(insert.data(), insert.size(), prefix);
        return known | (crc ^ crc32_shift(before ^ after, tail));
    }
    auto suffix = crc32(old.data() + offset + remove, tail);
    auto before = crc32_shift(crc ^ suffix, tail, true);
    auto prefix = crc32_shift(before ^ crc32(old.data() + offset, remove), remove, true);
    auto after = crc32(insert.data(), insert.size(), prefix);
    return known | (crc32_shift(after, tail) ^ suffix);
}
} // namespace fengqudb::storage
