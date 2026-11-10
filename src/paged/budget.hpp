#pragma once
#include "generation.hpp"
#include "../crc_delta.hpp"

namespace fengqudb::paged {
class Budget {
    std::atomic<uint64_t> used_{0};
    uint64_t maximum_;

  public:
    explicit Budget(uint64_t maximum) : maximum_(maximum) {}
    void take(uint64_t bytes) {
        auto used = used_.load(std::memory_order_relaxed);
        do {
            require(bytes <= maximum_ - used, FQDB_LIMIT);
        } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    }
    void release(uint64_t bytes) noexcept {
        used_.fetch_sub(bytes, std::memory_order_relaxed);
    }
    uint64_t used() const noexcept {
        return used_.load(std::memory_order_relaxed);
    }
};
class Charge {
    std::shared_ptr<Budget> local_, global_;
    uint64_t bytes_ = 0;

  public:
    Charge() = default;
    Charge(std::shared_ptr<Budget> local, std::shared_ptr<Budget> global, uint64_t bytes)
        : local_(std::move(local)), global_(std::move(global)) {
        resize(bytes);
    }
    Charge(const Charge &) = delete;
    Charge &operator=(const Charge &) = delete;
    Charge(Charge &&other) noexcept
        : local_(std::move(other.local_)), global_(std::move(other.global_)),
          bytes_(std::exchange(other.bytes_, 0)) {}
    Charge &operator=(Charge &&other) noexcept {
        if (this != &other) {
            reset();
            local_ = std::move(other.local_);
            global_ = std::move(other.global_);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }
    ~Charge() {
        reset();
    }
    void resize(uint64_t bytes) {
        if (bytes == bytes_)
            return;
        if (bytes > bytes_) {
            auto extra = bytes - bytes_;
            if (local_)
                local_->take(extra);
            try {
                if (global_)
                    global_->take(extra);
            } catch (...) {
                if (local_)
                    local_->release(extra);
                throw;
            }
        } else {
            if (local_)
                local_->release(bytes_ - bytes);
            if (global_)
                global_->release(bytes_ - bytes);
        }
        bytes_ = bytes;
    }
    uint64_t bytes() const noexcept {
        return bytes_;
    }
    void reset() noexcept {
        if (local_)
            local_->release(bytes_);
        if (global_)
            global_->release(bytes_);
        bytes_ = 0;
    }
};
struct StagedData : Data {
    Charge charge;
    StagedData(std::shared_ptr<Budget> local, std::shared_ptr<Budget> global)
        : charge(std::move(local), std::move(global), sizeof(StagedData) + 64) {}
    void capacity_for(size_t size) {
        if (size <= capacity())
            return;
        require(size <= FQDB_MAX_VALUE_BYTES, FQDB_LIMIT);
        // Reserve enough for the library's geometric string growth before the
        // allocation; release the unused allowance once its capacity is known.
        auto allowance = 2 * std::max(size, capacity());
        charge.resize(sizeof(StagedData) + 64 + allowance);
        try {
            reserve(size);
        } catch (...) {
            charge.resize(sizeof(StagedData) + 64 + capacity());
            throw;
        }
        charge.resize(sizeof(StagedData) + 64 + capacity());
    }
};
inline std::shared_ptr<StagedData> staged_data(std::string_view value,
                                               const std::shared_ptr<Budget> &local,
                                               const std::shared_ptr<Budget> &global) {
    auto result = std::make_shared<StagedData>(local, global);
    result->capacity_for(value.size());
    if (!value.empty())
        result->assign(value.data(), value.size());
    return result;
}
inline std::shared_ptr<StagedData> staged_data(const Data &value,
                                               const std::shared_ptr<Budget> &local,
                                               const std::shared_ptr<Budget> &global) {
    auto result = staged_data(std::string_view(value), local, global);
    result->cached_crc.store(value.cached_crc.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    return result;
}
#ifdef FQDB_TESTING
inline std::atomic<uint64_t> cache_buffer_transfers{0};
#endif
// A caller-owned transaction may share a tree node without sharing its data
// pointer. The command's explicit ownership grant is required in addition to
// this last reference check. Large WAL payloads may still be borrowed by I/O.
inline std::shared_ptr<Data> cache_data(const std::shared_ptr<StagedData> &value, uint32_t crc,
                                        bool consume) {
    std::shared_ptr<Data> result;
    // Do not retain a large allocation after subtraction shrank the value, or
    // spend ownership checks on tiny strings. These are policy bounds, not
    // assumptions about any STL's short-string layout or capacity rounding.
    if (consume && value->size() >= 256 && value->size() < 16384 &&
        value->capacity() - value->size() <= 64 && value.use_count() == 1) {
        result = std::make_shared<Data>(); // Allocate before consuming input.
        static_cast<Bytes &>(*result) = std::move(static_cast<Bytes &>(*value));
#ifdef FQDB_TESTING
        cache_buffer_transfers.fetch_add(1, std::memory_order_relaxed);
#endif
    } else
        result = std::make_shared<Data>(value->data(), value->size());
    result->cached_crc.store((UINT64_C(1) << 32) | crc, std::memory_order_relaxed);
    return result;
}
inline void patch_buffer(StagedData &bytes, uint64_t offset, uint64_t remove,
                         std::string_view insert) {
    const auto old = bytes.size();
    require(offset <= old && remove <= old - offset, FQDB_INVALID_ARGUMENT);
    require(insert.size() <= FQDB_MAX_VALUE_BYTES - (old - remove), FQDB_LIMIT);
    const auto next = size_t(old - remove + insert.size());
    const auto crc = storage::patched_crc(bytes, bytes.cached_crc.load(std::memory_order_relaxed),
                                          size_t(offset), size_t(remove), insert);
    bytes.capacity_for(next);
    if (next > old)
        bytes.resize(next);
    if (insert.size() != remove)
        std::memmove(bytes.data() + offset + insert.size(), bytes.data() + offset + remove,
                     old - offset - remove);
    if (next < old)
        bytes.resize(next);
    if (!insert.empty())
        std::memcpy(bytes.data() + offset, insert.data(), insert.size());
    bytes.cached_crc.store(crc, std::memory_order_relaxed);
}
} // namespace fengqudb::paged
