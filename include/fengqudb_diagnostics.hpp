#ifndef FENGQUDB_DIAGNOSTICS_HPP
#define FENGQUDB_DIAGNOSTICS_HPP
#include "fengqudb.hpp"
#include "fengqudb_diagnostics.h"
namespace fengqudb {
struct DiagnosticsOptions {
    uint32_t min_level = FQDB_LOG_WARNING;
    uint64_t slow_commit_us = 5000;
    bool reader_only = false;
};
class Diagnostics {
    struct Close {
        void operator()(fqdb_diagnostics *handle) const noexcept {
            fqdb_diagnostics_close(handle);
        }
    };
    std::unique_ptr<fqdb_diagnostics, Close> handle_;

  public:
    explicit Diagnostics(std::string_view directory, DiagnosticsOptions options = {}) {
        auto path = detail::path(directory);
        fqdb_diagnostics_options native{
            sizeof(native), 1, options.min_level,
            options.reader_only ? static_cast<uint32_t>(FQDB_DIAGNOSTICS_READER_ONLY) : 0u,
            options.slow_commit_us};
        fqdb_diagnostics *handle = nullptr;
        detail::check(fqdb_diagnostics_open(path.c_str(), &native, &handle));
        handle_.reset(handle);
    }
    Diagnostics(const Diagnostics &) = delete;
    Diagnostics &operator=(const Diagnostics &) = delete;
    Diagnostics(Diagnostics &&) noexcept = default;
    Diagnostics &operator=(Diagnostics &&) noexcept = default;
    void close() {
        auto status = fqdb_diagnostics_close(handle_.get());
        if (status == FQDB_OK)
            handle_.release();
        detail::check(status);
    }
    Buffer status() const {
        auto value = detail::Access::read(
            handle_.get(), {}, [](fqdb_diagnostics *handle, fqdb_slice, fqdb_buffer *out) {
                return fqdb_diagnostics_status(handle, out);
            });
        return std::move(*value);
    }
    Buffer query(std::string_view json = {}) const {
        auto value = detail::Access::read(handle_.get(), json, fqdb_diagnostics_query);
        return std::move(*value);
    }
    Buffer configure(std::string_view json) const {
        auto value = detail::Access::read(handle_.get(), json, fqdb_diagnostics_configure);
        return std::move(*value);
    }
    Buffer clear() const {
        auto value = detail::Access::read(
            handle_.get(), {}, [](fqdb_diagnostics *handle, fqdb_slice, fqdb_buffer *out) {
                return fqdb_diagnostics_clear(handle, out);
            });
        return std::move(*value);
    }
    bool flush(uint32_t timeout_ms = 2000) const {
        detail::require(handle_.get());
        auto result = fqdb_diagnostics_flush(handle_.get(), timeout_ms);
        if (result == FQDB_TIMEOUT)
            return false;
        detail::check(result);
        return true;
    }
};
} // namespace fengqudb
#endif
