#ifndef FENGQUDB_LOGGING_HPP
#define FENGQUDB_LOGGING_HPP
#include "fengqudb_log.h"
#include <atomic>
#include <chrono>
namespace fengqudb {
namespace detail {
extern std::atomic<bool> log_enabled;
void emit_log(fqdb_log_event event) noexcept;
class LogSpan {
    bool enabled_;
    std::chrono::steady_clock::time_point start_;
    fqdb_log_event event_;

  public:
    explicit LogSpan(uint32_t operation) noexcept
        : enabled_(log_enabled.load(std::memory_order_relaxed)) {
        if (enabled_) {
            event_ = {};
            start_ = std::chrono::steady_clock::now();
            event_.struct_size = sizeof(event_);
            event_.abi_version = 1;
            event_.operation = operation;
        }
    }
    fqdb_log_event *event() noexcept {
        return enabled_ ? &event_ : nullptr;
    }
    fqdb_status finish(fqdb_status status) noexcept {
        if (enabled_) {
            event_.status = status;
            event_.level =
                status == FQDB_CONFLICT || status == FQDB_HISTORY_LOST || status == FQDB_CONSTRAINT
                    ? FQDB_LOG_WARNING
                : status                ? FQDB_LOG_ERROR
                : event_.repaired_bytes ? FQDB_LOG_WARNING
                                        : FQDB_LOG_INFO;
            if (status == FQDB_OK)
                event_.stage = FQDB_STAGE_COMPLETE;
            event_.duration_us =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start_)
                                          .count());
            emit_log(event_);
        }
        return status;
    }
};
inline void stage(fqdb_log_event *event, uint32_t value) noexcept {
    if (event)
        event->stage = value;
}
} // namespace detail
} // namespace fengqudb
#endif
