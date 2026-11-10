#pragma once
#include "native_io.hpp"
#include <cstdlib>

namespace fengqudb::paged {
// Process/library-instance budget for speculative unused capacity, not user data.
// A required transaction is never rejected because this budget is exhausted.
struct ReservePool {
    inline static std::atomic<uint64_t> used{0}, peak{0}, live{0}, trims{0}, trim_errors{0};
    static constexpr uint64_t limit() {
        return (UINT64_C(4) << 20) / 16384;
    }
    static uint64_t acquire(uint64_t wanted) {
        auto current = used.load(std::memory_order_relaxed);
        for (;;) {
            const auto maximum = limit();
            const auto granted = std::min(wanted, current < maximum ? maximum - current : 0);
            if (!granted)
                return 0;
            if (used.compare_exchange_weak(current, current + granted, std::memory_order_relaxed)) {
                auto high = peak.load(std::memory_order_relaxed);
                while (high < current + granted &&
                       !peak.compare_exchange_weak(high, current + granted,
                                                   std::memory_order_relaxed)) {
                }
                return granted;
            }
        }
    }
    static constexpr uint64_t file_limit() {
        return (UINT64_C(64) << 20) / 16384;
    }
    static std::array<uint64_t, 6> info() {
        return {limit() * 16384, used.load() * 16384, peak.load() * 16384,
                live.load(),     trims.load(),        trim_errors.load()};
    }
};
} // namespace fengqudb::paged
