#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace fengqudb::detail {
inline bool binary_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && (left.empty() || left.data() == right.data() ||
                                           !std::memcmp(left.data(), right.data(), left.size()));
}
// A failed plan means write the complete new value. Sampling never establishes
// equality: every reused byte is covered by an exact comparison.
struct BinaryDiffPlan {
    struct Range {
        uint32_t offset = 0, size = 0;
    };
    std::array<Range, 8> ranges{};
    uint32_t count = 0, bytes = 4;
    bool accepted = false;
};
inline BinaryDiffPlan binary_diff(std::string_view before, std::string_view after) noexcept {
    BinaryDiffPlan plan;
    if (before.size() != after.size() || after.size() < 1024 || after.size() > 65536)
        return plan;
    const auto limit = std::min<size_t>(after.size() / 4, 1024);
    size_t at = 0;
    while (at < after.size()) {
        // libc/compiler can use vector loads. Unaligned word loads below use
        // memcpy, without alignment or strict-aliasing assumptions on ARM.
        if (after.size() - at >= 64 && !std::memcmp(before.data() + at, after.data() + at, 64)) {
            at += 64;
            continue;
        }
        const auto length = std::min<size_t>(8, after.size() - at);
        bool equal;
        if (length == 8) {
            uint64_t left, right;
            std::memcpy(&left, before.data() + at, 8);
            std::memcpy(&right, after.data() + at, 8);
            equal = left == right;
        } else {
            equal = !std::memcmp(before.data() + at, after.data() + at, length);
        }
        if (!equal) {
            if (plan.count &&
                at - (plan.ranges[plan.count - 1].offset + plan.ranges[plan.count - 1].size) <= 8) {
                auto &range = plan.ranges[plan.count - 1];
                const auto size = uint32_t(at + length - range.offset);
                plan.bytes += size - range.size;
                range.size = size;
            } else {
                if (plan.count == plan.ranges.size())
                    return plan;
                plan.ranges[plan.count++] = {uint32_t(at), uint32_t(length)};
                plan.bytes += 8 + uint32_t(length);
            }
            // Reject while discovering dense changes, before finishing the run
            // or allocating/copying any payload. At most one word overshoots.
            if (plan.bytes > limit)
                return plan;
        }
        at += length;
    }
    // Refine only the accepted range edges, at most seven bytes per edge.
    // Interior discovery remains word-sized even for dense modifications.
    for (uint32_t i = 0; i < plan.count; ++i) {
        auto &range = plan.ranges[i];
        while (range.size && before[range.offset] == after[range.offset]) {
            ++range.offset;
            --range.size;
            --plan.bytes;
        }
        while (range.size &&
               before[range.offset + range.size - 1] == after[range.offset + range.size - 1]) {
            --range.size;
            --plan.bytes;
        }
    }
    plan.accepted = true;
    return plan;
}
} // namespace fengqudb::detail
