#pragma once
#include <cstdint>

namespace fengqudb::storage {
// Request bytes, not device traffic. One owner thread at a time; metered
// maintenance never delegates its writes into another thread's commit group.
struct IOQuota {
    uint64_t max_read = 0, max_write = 0, read_bytes = 0, write_bytes = 0, sync_calls = 0;
    uint64_t write_credit = 0;
    bool covered = false, rejected = false;
    bool fits(uint64_t used, uint64_t size, uint64_t maximum) noexcept {
        if (size > UINT64_MAX - used || (maximum && (used > maximum || size > maximum - used))) {
            rejected = true;
            return false;
        }
        return true;
    }
    bool request(bool write, uint64_t size) noexcept {
        if (write) {
            if (covered) {
                if (size > write_credit) {
                    rejected = true;
                    return false;
                }
                write_credit -= size;
            } else if (!fits(write_bytes, size, max_write))
                return false;
            write_bytes += size;
        } else {
            if (!fits(read_bytes, size, max_read))
                return false;
            read_bytes += size;
        }
        return true;
    }
};
inline thread_local IOQuota *io_quota = nullptr;
inline bool io_request(bool write, uint64_t size) noexcept {
    return !io_quota || io_quota->request(write, size);
}
inline void io_sync() noexcept {
    if (io_quota && io_quota->sync_calls != UINT64_MAX)
        ++io_quota->sync_calls;
}
// Reserve a complete atomic write unit before its first byte. Nested units
// consume the outer allowance; failures refund unused allowance automatically.
class IOWriteReservation {
    IOQuota *quota_ = io_quota;
    bool owner_ = false, valid_ = true, nested_error_ = false;

  public:
    explicit IOWriteReservation(uint64_t bytes) noexcept {
        if (!quota_)
            return;
        if (quota_->covered) {
            valid_ = bytes <= quota_->write_credit;
            nested_error_ = !valid_;
        } else if ((valid_ = quota_->fits(quota_->write_bytes, bytes, quota_->max_write))) {
            quota_->covered = owner_ = true;
            quota_->write_credit = bytes;
        }
    }
    IOWriteReservation(const IOWriteReservation &) = delete;
    IOWriteReservation &operator=(const IOWriteReservation &) = delete;
    ~IOWriteReservation() {
        if (owner_) {
            quota_->covered = false;
            quota_->write_credit = 0;
        }
    }
    explicit operator bool() const noexcept {
        return valid_;
    }
    bool nested_error() const noexcept {
        return nested_error_;
    }
};
} // namespace fengqudb::storage
