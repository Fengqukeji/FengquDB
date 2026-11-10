#include "logging.hpp"
#include "api_support.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
struct fqdb_log_handle {
    fqdb_log_options options{};
    uint64_t delivered = 0, errors = 0, active = 0;
    bool closed = false;
};
namespace fengqudb {
namespace detail {
std::atomic<bool> log_enabled{false};
namespace {
std::mutex sink_mutex;
std::condition_variable sink_cv;
fqdb_log_handle *current = nullptr;
std::atomic<uint32_t> min_level{FQDB_LOG_WARNING};
std::atomic<uint64_t> slow_us{5000};
thread_local fqdb_log_handle *active_sink = nullptr;
bool qualifies(fqdb_log_event &event, uint32_t minimum, uint64_t slow) noexcept {
    if (event.operation == FQDB_LOG_COMMIT && event.status == FQDB_OK) {
        if (event.duration_us < slow)
            return false;
        event.level = FQDB_LOG_WARNING;
    }
    return event.level >= minimum;
}
} // namespace
void emit_log(fqdb_log_event event) noexcept {
    if (active_sink || !log_enabled.load(std::memory_order_relaxed) ||
        !qualifies(event, min_level.load(std::memory_order_relaxed),
                   slow_us.load(std::memory_order_relaxed)))
        return;
    try {
        fqdb_log_handle *sink;
        {
            std::lock_guard<std::mutex> lock(sink_mutex);
            sink = current;
            if (!sink || sink->closed ||
                !qualifies(event, sink->options.min_level, sink->options.slow_commit_us))
                return;
            ++sink->active;
        }
        bool failed = false;
        active_sink = sink;
        try {
            sink->options.callback(sink->options.user, &event);
        } catch (...) {
            failed = true;
        }
        active_sink = nullptr;
        {
            std::lock_guard<std::mutex> lock(sink_mutex);
            ++sink->delivered;
            if (failed)
                ++sink->errors;
            --sink->active;
            sink_cv.notify_all();
        }
    } catch (...) {
        active_sink = nullptr;
    }
}
} // namespace detail
} // namespace fengqudb
extern "C" {
fqdb_status FQDB_CALL fqdb_log_open(const fqdb_log_options *options, fqdb_log_handle **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!options || options->struct_size != sizeof(*options) || options->abi_version != 1 ||
        options->reserved || options->min_level > FQDB_LOG_ERROR || !options->callback)
        return FQDB_INVALID_ARGUMENT;
    return fengqudb::detail::status_boundary([&]() -> fqdb_status {
        auto sink = std::make_unique<fqdb_log_handle>();
        sink->options = *options;
        std::lock_guard<std::mutex> lock(fengqudb::detail::sink_mutex);
        if (fengqudb::detail::current)
            return FQDB_LOCKED;
        fengqudb::detail::current = sink.get();
        fengqudb::detail::min_level.store(options->min_level, std::memory_order_relaxed);
        fengqudb::detail::slow_us.store(options->slow_commit_us, std::memory_order_relaxed);
        fengqudb::detail::log_enabled.store(true, std::memory_order_release);
        *out = sink.release();
        return FQDB_OK;
    });
}
fqdb_status FQDB_CALL fqdb_log_get_info(fqdb_log_handle *sink, fqdb_log_info *out) {
    if (!sink || !out)
        return FQDB_INVALID_ARGUMENT;
    return fengqudb::detail::status_boundary([&]() -> fqdb_status {
        std::lock_guard<std::mutex> lock(fengqudb::detail::sink_mutex);
        *out = {sink->delivered, sink->errors, sink->active};
        return FQDB_OK;
    });
}
fqdb_status FQDB_CALL fqdb_log_close(fqdb_log_handle *sink) {
    if (!sink)
        return FQDB_OK;
    if (fengqudb::detail::active_sink == sink)
        return FQDB_INVALID_ARGUMENT;
    return fengqudb::detail::status_boundary([&]() -> fqdb_status {
        std::unique_lock<std::mutex> lock(fengqudb::detail::sink_mutex);
        sink->closed = true;
        if (fengqudb::detail::current == sink) {
            fengqudb::detail::current = nullptr;
            fengqudb::detail::log_enabled.store(false, std::memory_order_release);
        }
        fengqudb::detail::sink_cv.wait(lock, [&] { return sink->active == 0; });
        lock.unlock();
        delete sink;
        return FQDB_OK;
    });
}
}
