#pragma once
#include "storage.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include <optional>
#ifdef FQDB_TESTING
#include <thread>
#endif
#ifdef _WIN32
#include <winioctl.h>
#endif
#ifdef __APPLE__
#include <stdio.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#endif

namespace fengqudb::paged {
constexpr uint64_t reclaim_block_bytes = 65536;
#ifdef FQDB_TESTING
inline std::atomic<int> paged_fail_write{0}, paged_fail_sync{0}, paged_fail_punch{0};
inline std::atomic<bool> manifest_uncertain{false}, directory_uncertain{false};
inline std::atomic<bool> paged_fail_created_open{false};
inline std::atomic<uint64_t> paged_allocation_ceiling{UINT64_MAX};
inline std::atomic<bool> paged_hold_data_sync{false}, paged_data_sync_entered{false};
inline bool fault_countdown(std::atomic<int> &counter) {
    auto value = counter.load();
    while (value > 0)
        if (counter.compare_exchange_weak(value, value - 1))
            return value == 1;
    return false;
}
#endif
struct Error : std::exception {
    fqdb_status status;
    explicit Error(fqdb_status s) noexcept : status(s) {}
    const char *what() const noexcept override {
        return fqdb_status_message(status);
    }
};
// Keep exception construction out of the many successful validation branches.
[[noreturn]]
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
inline void fail(fqdb_status status) {
    throw Error(status);
}
inline void require(bool condition, fqdb_status status = FQDB_CORRUPT) {
    if (!condition)
        fail(status);
}
struct IOBudgetStop : Error {
    IOBudgetStop() noexcept : Error(FQDB_LIMIT) {}
};
class IOWritePermit {
    storage::IOWriteReservation reservation_;

  public:
    explicit IOWritePermit(uint64_t bytes) : reservation_(bytes) {
        require(!reservation_.nested_error(), FQDB_INTERNAL_ERROR);
        if (!reservation_)
            throw IOBudgetStop();
    }
};
// Positional I/O never shares a seek cursor with snapshot readers. The data-file
// lease follows the inode/file object across replacement, including hard links.
class NativeFile {
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE reader_ = INVALID_HANDLE_VALUE;
    std::mutex events_mutex_;
    std::vector<HANDLE> events_;
    HANDLE event() {
        std::lock_guard<std::mutex> lock(events_mutex_);
        if (!events_.empty()) {
            auto result = events_.back();
            events_.pop_back();
            ResetEvent(result);
            return result;
        }
        auto result = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(result != nullptr, FQDB_IO_ERROR);
        return result;
    }
    void recycle(HANDLE event) noexcept {
        std::lock_guard<std::mutex> lock(events_mutex_);
        if (events_.size() < 64)
            events_.push_back(event);
        else
            CloseHandle(event);
    }
    struct Event {
        NativeFile &owner;
        HANDLE handle;
        explicit Event(NativeFile &f) : owner(f), handle(f.event()) {}
        ~Event() {
            owner.recycle(handle);
        }
    };
#else
    int file_ = -1;
#endif
    bool leased_ = false;
    bool exclusive_created_ = false;

  public:
    bool exclusively_created() const noexcept {
        return exclusive_created_;
    }
    // The caller owns the file-layer exclusive lock and has published an
    // identical replacement. No reader may still issue I/O on this handle.
    void retire() noexcept {
        unlock();
#ifdef _WIN32
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        if (reader_ != INVALID_HANDLE_VALUE) {
            CloseHandle(reader_);
            reader_ = INVALID_HANDLE_VALUE;
        }
#else
        if (file_ >= 0) {
            ::close(file_);
            file_ = -1;
        }
#endif
    }
    std::atomic<uint64_t> reads{0}, writes{0}, syncs{0}, read_bytes{0}, written_bytes{0};
    std::atomic<uint64_t> punches{0}, punched_bytes{0};
    NativeFile() {
#ifdef _WIN32
        events_.reserve(64);
#endif
    }
    NativeFile(const NativeFile &) = delete;
    NativeFile &operator=(const NativeFile &) = delete;
    ~NativeFile() {
        unlock();
#ifdef _WIN32
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
        if (reader_ != INVALID_HANDLE_VALUE)
            CloseHandle(reader_);
        for (auto event : events_)
            CloseHandle(event);
#else
        if (file_ >= 0)
            ::close(file_);
#endif
    }
    void open(const std::string &path, bool create_only = false, bool existing_only = false,
              bool read_only = false) {
        require(!(create_only && existing_only), FQDB_INVALID_ARGUMENT);
#ifdef _WIN32
        require(file_ == INVALID_HANDLE_VALUE && reader_ == INVALID_HANDLE_VALUE,
                FQDB_INVALID_ARGUMENT);
#else
        require(file_ < 0, FQDB_INVALID_ARGUMENT);
#endif
        auto native = storage::native_path(path.c_str());
#ifdef _WIN32
        file_ =
            CreateFileW(native.c_str(), GENERIC_READ | (read_only ? 0 : GENERIC_WRITE),
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        create_only ? CREATE_NEW : (existing_only ? OPEN_EXISTING : OPEN_ALWAYS),
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            auto error = GetLastError();
            throw Error(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                            ? FQDB_ALREADY_EXISTS
                            : FQDB_IO_ERROR);
        }
        exclusive_created_ = create_only;
#ifdef FQDB_TESTING
        if (create_only && paged_fail_created_open.exchange(false))
            throw Error(FQDB_IO_ERROR);
#endif
        reader_ = CreateFileW(native.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        require(reader_ != INVALID_HANDLE_VALUE, FQDB_IO_ERROR);
        BY_HANDLE_FILE_INFORMATION owner{}, reader{};
        require(GetFileInformationByHandle(file_, &owner) &&
                    GetFileInformationByHandle(reader_, &reader),
                FQDB_IO_ERROR);
        require(owner.dwVolumeSerialNumber == reader.dwVolumeSerialNumber &&
                    owner.nFileIndexHigh == reader.nFileIndexHigh &&
                    owner.nFileIndexLow == reader.nFileIndexLow,
                FQDB_IO_ERROR);
#else
        do {
            file_ = ::open(native.c_str(),
                           (read_only ? O_RDONLY : O_RDWR) | (existing_only ? 0 : O_CREAT) |
                               O_CLOEXEC | (create_only ? O_EXCL : 0),
                           0600);
        } while (file_ < 0 && errno == EINTR);
        if (file_ < 0)
            throw Error(errno == EEXIST ? FQDB_ALREADY_EXISTS : FQDB_IO_ERROR);
        exclusive_created_ = create_only;
#ifdef FQDB_TESTING
        if (create_only && paged_fail_created_open.exchange(false))
            throw Error(FQDB_IO_ERROR);
#endif
#endif
    }
    void lock() {
#ifdef _WIN32
        Event event(*this);
        OVERLAPPED offset{};
        offset.hEvent = event.handle;
        if (!LockFileEx(file_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                        &offset)) {
            auto error = GetLastError();
            DWORD ignored = 0;
            if (error == ERROR_IO_PENDING)
                require(GetOverlappedResult(file_, &offset, &ignored, TRUE) != 0, FQDB_LOCKED);
            else
                throw Error(error == ERROR_LOCK_VIOLATION ? FQDB_LOCKED : FQDB_IO_ERROR);
        }
#else
        int result;
        do {
            result = flock(file_, LOCK_EX | LOCK_NB);
        } while (result && errno == EINTR);
        if (result)
            throw Error(errno == EAGAIN || errno == EWOULDBLOCK ? FQDB_LOCKED : FQDB_IO_ERROR);
#endif
        leased_ = true;
    }
    void unlock() noexcept {
        if (!leased_)
            return;
#ifdef _WIN32
        try {
            Event event(*this);
            OVERLAPPED offset{};
            offset.hEvent = event.handle;
            if (!UnlockFileEx(file_, 0, 1, 0, &offset) && GetLastError() == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                GetOverlappedResult(file_, &offset, &ignored, TRUE);
            }
        } catch (...) {
            return;
        } // The handle retains the lease until final close.
#else
        flock(file_, LOCK_UN);
#endif
        leased_ = false;
    }
    uint64_t size() const {
#ifdef _WIN32
        LARGE_INTEGER n;
        require(GetFileSizeEx(file_, &n) != 0, FQDB_IO_ERROR);
        require(n.QuadPart >= 0, FQDB_CORRUPT);
        return static_cast<uint64_t>(n.QuadPart);
#else
        struct stat n{};
        require(fstat(file_, &n) == 0, FQDB_IO_ERROR);
        require(n.st_size >= 0, FQDB_CORRUPT);
        return static_cast<uint64_t>(n.st_size);
#endif
    }
    void read(uint64_t offset, void *destination, size_t length) {
        require(offset <= INT64_MAX && length <= static_cast<uint64_t>(INT64_MAX) - offset,
                FQDB_LIMIT);
        if (!storage::io_request(false, length))
            throw IOBudgetStop();
        auto cursor = static_cast<unsigned char *>(destination);
        while (length) {
            auto request = std::min<size_t>(length, 1u << 30);
#ifdef _WIN32
            OVERLAPPED operation{};
            operation.Offset = static_cast<DWORD>(offset);
            operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD count = 0;
            if (offset < 64) {
                // The header contains the leased byte; use its owning handle.
                require(ReadFile(file_, cursor, static_cast<DWORD>(request), &count, &operation) !=
                            0,
                        FQDB_IO_ERROR);
            } else {
                Event event(*this);
                operation.hEvent = event.handle;
                bool started =
                    ReadFile(reader_, cursor, static_cast<DWORD>(request), &count, &operation);
                if (!started && GetLastError() != ERROR_IO_PENDING)
                    throw Error(FQDB_IO_ERROR);
                require(GetOverlappedResult(reader_, &operation, &count, TRUE) != 0, FQDB_IO_ERROR);
            }
#else
            ssize_t count;
            do {
                count = pread(file_, cursor, request, static_cast<off_t>(offset));
            } while (count < 0 && errno == EINTR);
            require(count >= 0, FQDB_IO_ERROR);
#endif
            require(count != 0, FQDB_IO_ERROR);
            ++reads;
            read_bytes += static_cast<uint64_t>(count);
            offset += static_cast<uint64_t>(count);
            cursor += count;
            length -= static_cast<size_t>(count);
        }
    }
    void write(uint64_t offset, const void *source, size_t length) {
#ifdef FQDB_TESTING
        if (length && fault_countdown(paged_fail_write)) {
            if (length > 1)
                write(offset, source, length / 2);
            throw Error(FQDB_IO_ERROR);
        }
#endif
        require(offset <= INT64_MAX && length <= static_cast<uint64_t>(INT64_MAX) - offset,
                FQDB_LIMIT);
        require(!storage::io_quota || !storage::io_quota->covered ||
                    length <= storage::io_quota->write_credit,
                FQDB_INTERNAL_ERROR);
        if (!storage::io_request(true, length))
            throw IOBudgetStop();
        auto cursor = static_cast<const unsigned char *>(source);
        while (length) {
            auto request = std::min<size_t>(length, 1u << 30);
#ifdef _WIN32
            OVERLAPPED operation{};
            operation.Offset = static_cast<DWORD>(offset);
            operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD count = 0;
            require(WriteFile(file_, cursor, static_cast<DWORD>(request), &count, &operation) != 0,
                    FQDB_IO_ERROR);
#else
            ssize_t count;
            do {
                count = pwrite(file_, cursor, request, static_cast<off_t>(offset));
            } while (count < 0 && errno == EINTR);
            require(count >= 0, FQDB_IO_ERROR);
#endif
            require(count != 0, FQDB_IO_ERROR);
            ++writes;
            written_bytes += static_cast<uint64_t>(count);
            offset += static_cast<uint64_t>(count);
            cursor += count;
            length -= static_cast<size_t>(count);
        }
    }
    uint64_t allocated() const {
#ifdef _WIN32
        FILE_STANDARD_INFO info{};
        require(GetFileInformationByHandleEx(file_, FileStandardInfo, &info, sizeof(info)) != 0,
                FQDB_IO_ERROR);
        require(info.AllocationSize.QuadPart >= 0, FQDB_IO_ERROR);
        const auto measured = static_cast<uint64_t>(info.AllocationSize.QuadPart);
#else
        struct stat info{};
        require(fstat(file_, &info) == 0 && info.st_blocks >= 0, FQDB_IO_ERROR);
        require(static_cast<uint64_t>(info.st_blocks) <= UINT64_MAX / 512, FQDB_LIMIT);
        const auto measured = static_cast<uint64_t>(info.st_blocks) * 512;
#endif
#ifdef FQDB_TESTING
        // Reproduce delayed sparse-file allocation metadata without relying
        // on the Windows lazy writer's timing. Absent from production builds.
        return std::min(measured, paged_allocation_ceiling.load());
#else
        return measured;
#endif
    }
    // These primitives are used only for proven unreachable, aligned ranges.
    // Querying allocated extents avoids repeatedly spending a slice on old holes.
    std::optional<std::pair<uint64_t, uint64_t>> next_data(uint64_t begin, uint64_t end) {
        require(begin <= end && end <= INT64_MAX, FQDB_LIMIT);
        if (begin == end)
            return {};
#ifdef _WIN32
        FILE_ALLOCATED_RANGE_BUFFER query{}, found{};
        query.FileOffset.QuadPart = static_cast<LONGLONG>(begin);
        query.Length.QuadPart = static_cast<LONGLONG>(end - begin);
        DWORD returned = 0;
        const bool ok = DeviceIoControl(file_, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query),
                                        &found, sizeof(found), &returned, nullptr) != 0;
        if (!ok && GetLastError() != ERROR_MORE_DATA) {
            const auto error = GetLastError();
            throw Error(error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
                                error == ERROR_INVALID_PARAMETER
                            ? FQDB_UNSUPPORTED
                            : FQDB_IO_ERROR);
        }
        if (!returned)
            return {};
        require(returned >= sizeof(found) && found.FileOffset.QuadPart >= 0 &&
                    found.Length.QuadPart > 0,
                FQDB_IO_ERROR);
        auto first = static_cast<uint64_t>(found.FileOffset.QuadPart);
        auto length = static_cast<uint64_t>(found.Length.QuadPart);
        require(length <= UINT64_MAX - first, FQDB_LIMIT);
        auto last = std::min(end, first + length);
        first = std::max(first, begin);
        require(first < last, FQDB_IO_ERROR);
        return std::make_pair(first, last);
#elif defined(__linux__) && defined(SEEK_DATA) && defined(SEEK_HOLE)
        off_t first, last;
        do {
            first = lseek(file_, static_cast<off_t>(begin), SEEK_DATA);
        } while (first < 0 && errno == EINTR);
        if (first < 0 && errno == ENXIO)
            return {};
        if (first < 0)
            throw Error(errno == EINVAL || errno == EOPNOTSUPP ? FQDB_UNSUPPORTED : FQDB_IO_ERROR);
        if (static_cast<uint64_t>(first) >= end)
            return {};
        do {
            last = lseek(file_, first, SEEK_HOLE);
        } while (last < 0 && errno == EINTR);
        require(last > first, FQDB_IO_ERROR);
        return std::make_pair(static_cast<uint64_t>(first),
                              std::min(end, static_cast<uint64_t>(last)));
#else
        throw Error(FQDB_UNSUPPORTED);
#endif
    }
    void punch(uint64_t offset, uint64_t length) {
        require(offset >= reclaim_block_bytes && length && !(offset % reclaim_block_bytes) &&
                    !(length % reclaim_block_bytes) && offset <= INT64_MAX &&
                    length <= static_cast<uint64_t>(INT64_MAX) - offset,
                FQDB_LIMIT);
        IOWritePermit permit(length);
        if (!storage::io_request(true, length))
            throw IOBudgetStop();
#ifdef FQDB_TESTING
        if (fault_countdown(paged_fail_punch))
            throw Error(FQDB_IO_ERROR);
#endif
        punch_native(offset, length);
        ++punches;
        punched_bytes += length;
    }

  private:
    void punch_native(uint64_t offset, uint64_t length) {
#ifdef _WIN32
        FILE_ZERO_DATA_INFORMATION range{};
        range.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
        range.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(offset + length);
        DWORD returned = 0;
        if (!DeviceIoControl(file_, FSCTL_SET_ZERO_DATA, &range, sizeof(range), nullptr, 0,
                             &returned, nullptr)) {
            const auto error = GetLastError();
            throw Error(error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
                                error == ERROR_INVALID_PARAMETER
                            ? FQDB_UNSUPPORTED
                            : FQDB_IO_ERROR);
        }
#elif defined(__linux__)
        int result;
        do {
            result = fallocate(file_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               static_cast<off_t>(offset), static_cast<off_t>(length));
        } while (result && errno == EINTR);
        if (result)
            throw Error(errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL ? FQDB_UNSUPPORTED
                                                                                  : FQDB_IO_ERROR);
#else
        throw Error(FQDB_UNSUPPORTED);
#endif
    }

  public:
    void prepare_sparse() {
#ifdef _WIN32
        DWORD returned = 0;
        if (!DeviceIoControl(file_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
            const auto error = GetLastError();
            throw Error(error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
                                error == ERROR_INVALID_PARAMETER
                            ? FQDB_UNSUPPORTED
                            : FQDB_IO_ERROR);
        }
#elif !defined(__linux__)
        throw Error(FQDB_UNSUPPORTED);
#endif
        const auto end = size();
        require(end <= INT64_MAX - 2 * reclaim_block_bytes, FQDB_LIMIT);
        const auto outside = (end / reclaim_block_bytes + 1) * reclaim_block_bytes;
        // This capability probe is entirely beyond EOF, like a metadata query:
        // KEEP_SIZE changes no file data and consumes no data-byte quota.
        punch_native(outside, reclaim_block_bytes);
        require(size() == end, FQDB_IO_ERROR);
        (void)next_data(reclaim_block_bytes, std::max(end, reclaim_block_bytes));
    }
    void data_sync() {
#ifdef FQDB_TESTING
        if (paged_hold_data_sync.load()) {
            paged_data_sync_entered.store(true);
            while (paged_hold_data_sync.load())
                std::this_thread::yield();
            paged_data_sync_entered.store(false);
        }
#endif
#if defined(_WIN32) || defined(__APPLE__)
        sync();
#else
        storage::io_sync();
#ifdef FQDB_TESTING
        if (fault_countdown(paged_fail_sync))
            throw Error(FQDB_IO_ERROR);
#endif
        int result;
        do {
            result = fdatasync(file_);
        } while (result && errno == EINTR);
        require(result == 0, FQDB_IO_ERROR);
        ++syncs;
#endif
    }
    void sync() {
        storage::io_sync();
#ifdef FQDB_TESTING
        if (fault_countdown(paged_fail_sync))
            throw Error(FQDB_IO_ERROR);
#endif
#ifdef _WIN32
        require(FlushFileBuffers(file_) != 0, FQDB_IO_ERROR);
#else
        int result;
        do {
            result = fsync(file_);
        } while (result && errno == EINTR);
        require(result == 0, FQDB_IO_ERROR);
#endif
        ++syncs;
    }
    void truncate(uint64_t length) {
        require(length <= INT64_MAX, FQDB_LIMIT);
#ifdef _WIN32
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
        require(SetFileInformationByHandle(file_, FileEndOfFileInfo, &end, sizeof(end)) != 0,
                FQDB_IO_ERROR);
#else
        int result;
        do {
            result = ftruncate(file_, static_cast<off_t>(length));
        } while (result && errno == EINTR);
        require(result == 0, FQDB_IO_ERROR);
#endif
    }
};
inline void remove_owned(const std::string &path) noexcept {
    try {
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::u8path(path), ignored);
    } catch (...) {
    }
}
// Only small CLOSED manifest files are replaced. Data generation handles stay
// open under stable names, including while an older snapshot still reads them.
inline fqdb_status publish_manifest(const std::string &temporary, const std::string &target) {
#ifdef _WIN32
    auto source = storage::native_path(temporary.c_str());
    auto destination = storage::native_path(target.c_str());
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return FQDB_COMMIT_UNKNOWN; // Never continue writing a possibly detached generation.
#else
    if (::rename(temporary.c_str(), target.c_str()))
        return FQDB_IO_ERROR;
    try {
        if (!storage::sync_parent(target.c_str()))
            return FQDB_COMMIT_UNKNOWN;
    } catch (...) {
        return FQDB_COMMIT_UNKNOWN;
    }
#endif
#ifdef FQDB_TESTING
    if (manifest_uncertain.exchange(false))
        return FQDB_COMMIT_UNKNOWN;
#endif
    return FQDB_OK;
}
inline fqdb_status publish_directory(const std::string &temporary, const std::string &target,
                                     bool directory_event = true) {
    (void)directory_event;
#ifdef _WIN32
    auto source = storage::native_path(temporary.c_str()),
         destination = storage::native_path(target.c_str());
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            return FQDB_ALREADY_EXISTS;
        return FQDB_COMMIT_UNKNOWN;
    }
#elif defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, target.c_str(),
                  RENAME_NOREPLACE)) {
        if (errno == EEXIST || errno == ENOTEMPTY)
            return FQDB_ALREADY_EXISTS;
        return errno == ENOSYS || errno == EINVAL ? FQDB_UNSUPPORTED : FQDB_IO_ERROR;
    }
    try {
        if (!storage::sync_parent(target.c_str()))
            return FQDB_COMMIT_UNKNOWN;
    } catch (...) {
        return FQDB_COMMIT_UNKNOWN;
    }
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    if (::renamex_np(temporary.c_str(), target.c_str(), RENAME_EXCL)) {
        if (errno == EEXIST || errno == ENOTEMPTY)
            return FQDB_ALREADY_EXISTS;
        return errno == ENOTSUP || errno == EINVAL ? FQDB_UNSUPPORTED : FQDB_IO_ERROR;
    }
    try {
        if (!storage::sync_parent(target.c_str()))
            return FQDB_COMMIT_UNKNOWN;
    } catch (...) {
        return FQDB_COMMIT_UNKNOWN;
    }
#else
    return FQDB_UNSUPPORTED;
#endif
#ifdef FQDB_TESTING
    if (directory_event && directory_uncertain.exchange(false))
        return FQDB_COMMIT_UNKNOWN;
#endif
    return FQDB_OK;
}
} // namespace fengqudb::paged
