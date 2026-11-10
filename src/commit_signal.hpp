#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef __linux__
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fengqudb::paged {
inline void commit_pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}
// At most 512 architecture pause instructions, then park in the OS. Only
// performance acknowledgements with one outstanding request use this budget.
constexpr unsigned commit_spins() noexcept {
    return 512;
}
#ifdef FQDB_TESTING
// 1: platform unavailable; 2: native wait fails after admission.
inline std::atomic<int> commit_signal_fault{0};
#endif
struct CommitSignal {
    std::condition_variable changed;
    // 0: producer has not waited, 1: waiting, 2: completed.
    std::atomic<uint32_t> state{0};
    bool busy = false, fallback = false;
    static bool native() noexcept {
#ifdef FQDB_TESTING
        if (commit_signal_fault.load(std::memory_order_relaxed) == 1)
            return false;
#endif
#ifdef __linux__
        static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "futex word size");
        static const bool available = [] {
            alignas(4) uint32_t probe = 0;
            return syscall(SYS_futex, &probe, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) >= 0;
        }();
        return available && std::atomic<uint32_t>::is_always_lock_free;
#elif defined(_WIN32)
        return api().wait && api().wake && std::atomic<uint32_t>::is_always_lock_free;
#else
        return false;
#endif
    }
#ifdef _WIN32
    struct Api {
        using Wait = BOOL(WINAPI *)(volatile VOID *, PVOID, SIZE_T, DWORD);
        using Wake = VOID(WINAPI *)(PVOID);
        Wait wait;
        Wake wake;
    };
    static const Api &api() noexcept {
        static const Api value = [] {
            auto module = GetModuleHandleW(L"kernel32.dll");
            if (!module)
                return Api{};
            return Api{reinterpret_cast<Api::Wait>(GetProcAddress(module, "WaitOnAddress")),
                       reinterpret_cast<Api::Wake>(GetProcAddress(module, "WakeByAddressSingle"))};
        }();
        return value;
    }
#endif
    bool wait_native(bool eager) noexcept {
#ifdef FQDB_TESTING
        if (commit_signal_fault.load(std::memory_order_relaxed) == 2)
            return false;
#endif
        for (unsigned n = 0; n < (eager ? commit_spins() : 0); ++n) {
            if (state.load(std::memory_order_acquire) == 2)
                return true;
            commit_pause();
        }
        uint32_t expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire))
            return expected == 2;
        while (state.load(std::memory_order_acquire) != 2) {
#ifdef __linux__
            if (syscall(SYS_futex, &state, FUTEX_WAIT_PRIVATE, 1, nullptr, nullptr, 0) < 0 &&
                errno != EINTR && errno != EAGAIN)
                return false;
#elif defined(_WIN32)
            uint32_t waiting = 1;
            if (!api().wait(&state, &waiting, sizeof(waiting), INFINITE))
                return false;
#else
            return false;
#endif
        }
        return true;
    }
    void complete_native() noexcept {
        if (state.exchange(2, std::memory_order_release) != 1)
            return;
#ifdef __linux__
        (void)syscall(SYS_futex, &state, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#elif defined(_WIN32)
        api().wake(&state);
#endif
    }
};
} // namespace fengqudb::paged
