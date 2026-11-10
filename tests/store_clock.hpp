#pragma once
#include "paged/generation.hpp"
// Deterministic TTL coverage; monotonic transaction deadlines remain real.
class StoreTestClock {
    uint64_t previous_;
    bool active_ = true;

  public:
    explicit StoreTestClock(uint64_t now)
        : previous_(fengqudb::paged::test_unix_ms.exchange(now)) {}
    void set(uint64_t now) {
        fengqudb::paged::test_unix_ms.store(now);
    }
    void reset() {
        if (active_) {
            fengqudb::paged::test_unix_ms.store(previous_);
            active_ = false;
        }
    }
    ~StoreTestClock() {
        reset();
    }
};
