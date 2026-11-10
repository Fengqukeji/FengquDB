#ifndef FENGQUDB_HPP
#define FENGQUDB_HPP
#include "fengqudb.h"
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <mutex>
#include <limits>

// C++17 convenience API. All C++ objects/exceptions stay in the caller; only the
// versioned C ABI crosses the library boundary. Destructors never commit or sync.
namespace fengqudb {
enum class Status : fqdb_status {
    ok = FQDB_OK,
    not_found = FQDB_NOT_FOUND,
    end = FQDB_END,
    invalid_argument = FQDB_INVALID_ARGUMENT,
    io_error = FQDB_IO_ERROR,
    corrupt = FQDB_CORRUPT,
    locked = FQDB_LOCKED,
    conflict = FQDB_CONFLICT,
    no_memory = FQDB_NO_MEMORY,
    limit = FQDB_LIMIT,
    commit_unknown = FQDB_COMMIT_UNKNOWN,
    recovery_required = FQDB_RECOVERY_REQUIRED,
    unsupported = FQDB_UNSUPPORTED,
    internal_error = FQDB_INTERNAL_ERROR,
    history_lost = FQDB_HISTORY_LOST,
    already_exists = FQDB_ALREADY_EXISTS,
    buffer_too_small = FQDB_BUFFER_TOO_SMALL,
    timeout = FQDB_TIMEOUT,
    constraint = FQDB_CONSTRAINT,
    async_required = FQDB_ASYNC_REQUIRED,
    not_modified = FQDB_NOT_MODIFIED,
    cancelled = FQDB_CANCELLED
};
enum class Durability : uint32_t {
    sync = FQDB_DURABILITY_SYNC,
    buffered = FQDB_DURABILITY_BUFFERED
};
enum class Operation : uint32_t { put = FQDB_PUT, erase = FQDB_DELETE };

class Error : public std::runtime_error {
    Status status_;

  public:
    explicit Error(fqdb_status code)
        : std::runtime_error(fqdb_status_message(code)), status_(static_cast<Status>(code)) {}
    Status status() const noexcept {
        return status_;
    }
    fqdb_status code() const noexcept {
        return static_cast<fqdb_status>(status_);
    }
};
struct CommitResult {
    Status status = Status::ok;
    uint64_t version = 0;
    explicit operator bool() const noexcept {
        return status == Status::ok;
    }
};
struct ReadResult {
    Status status = Status::ok;
    uint64_t required = 0;
    explicit operator bool() const noexcept {
        return status == Status::ok;
    }
};
namespace detail {
inline void check(fqdb_status status) {
    if (status != FQDB_OK)
        throw Error(status);
}
inline void require(const void *handle) {
    if (!handle)
        throw Error(FQDB_INVALID_ARGUMENT);
}
inline fqdb_slice slice(std::string_view bytes) noexcept {
    return {bytes.data(), static_cast<uint64_t>(bytes.size())};
}
inline std::string_view view(fqdb_slice bytes) noexcept {
    return bytes.size ? std::string_view(static_cast<const char *>(bytes.data),
                                         static_cast<size_t>(bytes.size))
                      : std::string_view();
}
inline std::string path(std::string_view utf8) {
    if (utf8.empty() || utf8.find('\0') != std::string_view::npos)
        throw Error(FQDB_INVALID_ARGUMENT);
    return std::string(utf8);
}
template <class T, void(FQDB_CALL *Close)(T *)> struct Deleter {
    void operator()(T *p) const noexcept {
        Close(p);
    }
};
struct Access;
} // namespace detail

// One native allocation/copy. Empty is a valid value; NOT_FOUND is std::nullopt.
class Buffer {
    fqdb_buffer buffer_{};
    friend struct detail::Access;

  public:
    Buffer() noexcept = default;
    ~Buffer() noexcept {
        if (buffer_.data)
            fqdb_buffer_free(&buffer_);
    }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept : buffer_(std::exchange(other.buffer_, fqdb_buffer{})) {}
    Buffer &operator=(Buffer &&other) noexcept {
        if (this != &other) {
            if (buffer_.data)
                fqdb_buffer_free(&buffer_);
            buffer_ = std::exchange(other.buffer_, fqdb_buffer{});
        }
        return *this;
    }
    const char *data() const noexcept {
        return static_cast<const char *>(buffer_.data);
    }
    size_t size() const noexcept {
        return static_cast<size_t>(buffer_.size);
    }
    bool empty() const noexcept {
        return size() == 0;
    }
    std::string_view view() const noexcept {
        return detail::view({buffer_.data, buffer_.size});
    }
    std::string str() const {
        return empty() ? std::string() : std::string(data(), size());
    }
};

namespace detail {
struct Access {
    static Buffer adopt(fqdb_buffer bytes) {
        Buffer result;
        result.buffer_ = bytes;
        return result;
    }
    template <class Handle, class Get>
    static std::optional<Buffer> read(Handle *handle, std::string_view key, Get get) {
        require(handle);
        Buffer result;
        auto status = get(handle, slice(key), &result.buffer_);
        if (status == FQDB_NOT_FOUND)
            return std::nullopt;
        check(status);
        return std::optional<Buffer>(std::move(result));
    }
};
} // namespace detail
} // namespace fengqudb
#endif
