#pragma once
#include "commit_signal.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace fengqudb::paged {
// Writer-thread local policy. Learn only from real arrivals; timer/spurious
// wakes cannot repeatedly arm a spin window while the database is idle.
class AdaptiveCommitWait {
  public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto maximum_window = std::chrono::microseconds(64);

  private:
    unsigned budget_ = commit_spins();
    bool armed_ = false, observing_ = false, parallel_;
    Time idle_since_{};

  public:
    explicit AdaptiveCommitWait(bool parallel = true) noexcept : parallel_(parallel) {}
    static bool parallel_execution() noexcept {
#ifdef __linux__
        // Use the calling worker's actual affinity, including Android cpusets.
        // A raw syscall avoids adding an Android API-level symbol dependency.
        unsigned char mask[128]{};
        const auto size = syscall(SYS_sched_getaffinity, 0, sizeof(mask), mask);
        if (size > 0 && static_cast<size_t>(size) <= sizeof(mask)) {
            unsigned count = 0;
            for (long i = 0; i < size; ++i)
                for (unsigned bit = 0; bit < 8; ++bit)
                    if ((mask[i] & (1u << bit)) && ++count == 2)
                        return true;
            return false;
        }
#elif defined(_WIN32)
        DWORD_PTR process = 0, system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
            return (process & (process - 1)) != 0;
#endif
        return std::thread::hardware_concurrency() > 1;
    }
    void completed(bool performance) noexcept {
        armed_ = performance;
        if (!performance)
            budget_ = commit_spins();
    }
    unsigned begin_idle(Time now) noexcept {
        if (observing_)
            return 0;
        observing_ = true;
        idle_since_ = now;
        return std::exchange(armed_, false) ? budget_ : 0;
    }
    void arrived(Time now) noexcept {
        if (!observing_)
            return;
        const auto gap = now - idle_since_;
        if (!parallel_ || gap < Clock::duration::zero() || gap >= std::chrono::milliseconds(1))
            budget_ = commit_spins();
        else if (gap <= std::chrono::microseconds(250))
            budget_ = std::min(4096u, budget_ * 2);
        else
            budget_ = std::max(commit_spins(), budget_ / 2);
        observing_ = false;
    }
    static unsigned spin(const std::atomic<bool> &ready, unsigned budget, Time until) noexcept {
        unsigned pauses = 0;
        while (pauses < budget && !ready.load(std::memory_order_acquire)) {
            if (!(pauses & 127) && Clock::now() >= until)
                break;
            commit_pause();
            ++pauses;
        }
        return pauses;
    }
};
} // namespace fengqudb::paged
