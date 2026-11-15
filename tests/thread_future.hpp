#pragma once
#include <future>
#include <thread>
#include <type_traits>
#include <utility>

namespace test_support {
// Barrier fixtures require every participant to have its own runnable thread.
// MSVC std::async may use a bounded scheduler: a task waiting for a not-yet-
// scheduled participant can otherwise starve the test before engine work starts.
// Keep async's joining lifetime and exception propagation, without that scheduler.
template <class T> class ThreadFuture {
    std::future<T> result_;
    std::thread worker_;
    void join() {
        if (worker_.joinable())
            worker_.join();
    }

  public:
    ThreadFuture() = default;
    ThreadFuture(std::future<T> result, std::thread worker)
        : result_(std::move(result)), worker_(std::move(worker)) {}
    ThreadFuture(ThreadFuture &&) noexcept = default;
    ThreadFuture &operator=(ThreadFuture &&other) noexcept {
        if (this != &other) {
            join();
            result_ = std::move(other.result_);
            worker_ = std::move(other.worker_);
        }
        return *this;
    }
    ~ThreadFuture() { join(); }
    T get() {
        join();
        return result_.get();
    }
    template <class R, class P> auto wait_for(std::chrono::duration<R, P> duration) const {
        return result_.wait_for(duration);
    }
};
template <class F> auto async_thread(F &&function) {
    using Result = std::invoke_result_t<F>;
    std::packaged_task<Result()> task(std::forward<F>(function));
    auto result = task.get_future();
    return ThreadFuture<Result>(std::move(result), std::thread(std::move(task)));
}
} // namespace test_support
