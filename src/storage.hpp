#ifndef FENGQUDB_STORAGE_HPP
#define FENGQUDB_STORAGE_HPP
#include "fengqudb.h"
#include "io_budget.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace fengqudb {
namespace storage {
constexpr size_t file_header_size = 32, frame_header_size = 32, trailer_size = 24;
constexpr char file_magic[] = "FQDBLOG1", frame_magic[] = "FQTXN001", end_magic[] = "FQEND001";
constexpr char checkpoint_magic[] = "FQCHK001", baseline_magic[] = "FQBASE01";
#ifdef FQDB_TESTING
inline std::atomic<uint64_t> write_calls{0}, seek_calls{0};
// Per-thread observer for a selected payload length; absent from release builds.
inline thread_local size_t crc32_watch_size = size_t(-1);
inline thread_local uint64_t crc32_watch_calls = 0;
#endif
inline const auto &crc32_table() noexcept {
    static const auto table = [] {
        std::array<std::array<uint32_t, 256>, 8> result{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t value = n;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
            result[0][n] = value;
        }
        for (size_t k = 1; k < result.size(); ++k)
            for (size_t n = 0; n < 256; ++n) {
                auto value = result[k - 1][n];
                result[k][n] = (value >> 8) ^ result[0][value & 255];
            }
        return result;
    }();
    return table;
}
inline uint32_t crc32_step(uint32_t value, const unsigned char *bytes) noexcept {
    const auto &table = crc32_table();
    uint32_t low = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
                   (uint32_t(bytes[3]) << 24);
    uint32_t high = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) | (uint32_t(bytes[6]) << 16) |
                    (uint32_t(bytes[7]) << 24);
    low ^= value;
    return table[7][low & 255] ^ table[6][(low >> 8) & 255] ^ table[5][(low >> 16) & 255] ^
           table[4][low >> 24] ^ table[3][high & 255] ^ table[2][(high >> 8) & 255] ^
           table[1][(high >> 16) & 255] ^ table[0][high >> 24];
}
inline uint32_t crc32_scalar(const void *data, size_t size, uint32_t initial = 0) noexcept {
    const auto &table = crc32_table();
    uint32_t value = initial ^ 0xffffffffu;
    auto bytes = static_cast<const unsigned char *>(data);
    while (size >= 8) {
        value = crc32_step(value, bytes);
        bytes += 8;
        size -= 8;
    }
    while (size--) {
        value = table[0][(value ^ *bytes++) & 255] ^ (value >> 8);
    }
    return value ^ 0xffffffffu;
}
// IEEE CRC32(A || B) from cached CRCs, without visiting B again.
// Apply the byte-shift operator for arbitrary lengths without caching.
inline uint32_t crc32_combine_raw(uint32_t a, uint32_t b, uint64_t length) noexcept {
    if (!length)
        return a;
    auto times = [](const uint32_t *matrix, uint32_t vector) {
        uint32_t sum = 0;
        unsigned n = 0;
        while (vector) {
            if (vector & 1)
                sum ^= matrix[n];
            vector >>= 1;
            ++n;
        }
        return sum;
    };
    static const auto powers = [&] {
        std::array<std::array<uint32_t, 32>, 64> result{};
        auto square = [&](uint32_t *out, const uint32_t *matrix) {
            for (unsigned n = 0; n < 32; ++n)
                out[n] = times(matrix, matrix[n]);
        };
        std::array<uint32_t, 32> bit{}, two{}, four{};
        bit[0] = 0xedb88320u;
        uint32_t row = 1;
        for (unsigned n = 1; n < 32; ++n) {
            bit[n] = row;
            row <<= 1;
        }
        square(two.data(), bit.data());
        square(four.data(), two.data());
        square(result[0].data(), four.data());
        for (size_t i = 1; i < result.size(); ++i)
            square(result[i].data(), result[i - 1].data());
        return result;
    }();
    size_t bit = 0;
    while (length) {
        if (length & 1)
            a = times(powers[bit].data(), a);
        length >>= 1;
        ++bit;
    }
    return a ^ b;
}
inline uint32_t crc32_transform(const uint32_t *matrix, uint32_t value) noexcept {
    uint32_t sum = 0;
    for (unsigned bit = 0; value; ++bit, value >>= 1)
        if (value & 1)
            sum ^= matrix[bit];
    return sum;
}
// Build only after repeated use within a bounded working set. Variable one-off
// lengths keep the scalar/logarithmic paths instead of paying matrix setup.
inline const uint32_t *crc32_operator(uint64_t length) noexcept {
    struct Entry {
        uint64_t length = 0;
        std::array<uint32_t, 32> matrix{};
        unsigned hits = 0;
    };
    thread_local std::array<Entry, 8> entries{};
    thread_local unsigned replace = 0;
    for (auto &entry : entries)
        if (entry.length == length) {
            if (entry.hits < 8) {
                ++entry.hits;
                return nullptr;
            }
            if (entry.hits == 8) {
                for (unsigned i = 0; i < 32; ++i)
                    entry.matrix[i] = crc32_combine_raw(uint32_t(1) << i, 0, length);
                ++entry.hits;
            }
            return entry.matrix.data();
        }
    auto &entry = entries[replace++ % entries.size()];
    entry.length = length;
    entry.hits = 1;
    return nullptr;
}
inline uint32_t crc32_combine(uint32_t a, uint32_t b, uint64_t length) noexcept {
    if (!length)
        return a;
    if (!a)
        return b;
    auto matrix = crc32_operator(length);
    return matrix ? crc32_transform(matrix, a) ^ b : crc32_combine_raw(a, b, length);
}
// Independent contiguous quarters hide table-lookup dependencies. This is
// the same IEEE polynomial and initial/final XOR as the scalar path.
inline uint32_t crc32(const void *data, size_t size, uint32_t initial = 0) noexcept {
#ifdef FQDB_TESTING
    if (size == crc32_watch_size)
        ++crc32_watch_calls;
#endif
    if (size < 2048)
        return crc32_scalar(data, size, initial);
    const auto bytes = static_cast<const unsigned char *>(data);
    const auto quarter = (size / 32) * 8;
    const auto matrix = crc32_operator(quarter);
    if (!matrix)
        return crc32_scalar(data, size, initial);
    uint32_t a = initial ^ 0xffffffffu, b = 0xffffffffu, c = 0xffffffffu, d = 0xffffffffu;
    for (size_t at = 0; at < quarter; at += 8) {
        a = crc32_step(a, bytes + at);
        b = crc32_step(b, bytes + quarter + at);
        c = crc32_step(c, bytes + quarter * 2 + at);
        d = crc32_step(d, bytes + quarter * 3 + at);
    }
    auto sum = crc32_transform(matrix, a ^ 0xffffffffu) ^ (b ^ 0xffffffffu);
    sum = crc32_transform(matrix, sum) ^ (c ^ 0xffffffffu);
    sum = crc32_transform(matrix, sum) ^ (d ^ 0xffffffffu);
    return crc32_scalar(bytes + quarter * 4, size - quarter * 4, sum);
}
inline void put32(unsigned char *p, uint32_t v) noexcept {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<unsigned char>(v >> (8 * i));
}
inline void put64(unsigned char *p, uint64_t v) noexcept {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = static_cast<unsigned char>(v >> (8 * i));
}
inline uint32_t get32(const unsigned char *p) noexcept {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= uint32_t(p[i]) << (8 * i);
    return v;
}
inline uint64_t get64(const unsigned char *p) noexcept {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
#ifdef _WIN32
using NativePath = std::wstring;
inline NativePath native_path(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (!count)
        throw std::invalid_argument("invalid UTF-8 path");
    NativePath value(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, value.data(), count))
        throw std::invalid_argument("invalid UTF-8 path");
    value.resize(static_cast<size_t>(count - 1));
    return value;
}
#else
using NativePath = std::string;
inline NativePath native_path(const char *path) {
    return path;
}
#endif

class File {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
    uint64_t position_ = 0;
    uint32_t error_ = 0;
    bool position_known_ = true;
    uint64_t writes_ = 0, syncs_ = 0;
    bool failed() noexcept {
#ifdef _WIN32
        error_ = GetLastError();
#else
        error_ = static_cast<uint32_t>(errno);
#endif
        return false;
    }

  public:
    File() = default;
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() {
        close();
    }
    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (handle_ != -1)
            ::close(handle_);
        handle_ = -1;
#endif
    }
    uint32_t error() const noexcept {
        return error_;
    }
    uint64_t position() const noexcept {
        return position_;
    }
    uint64_t write_count() const noexcept {
        return writes_;
    }
    uint64_t sync_count() const noexcept {
        return syncs_;
    }
    fqdb_status open(const char *path, bool create_only = false) {
        return open_native(native_path(path), create_only);
    }
    fqdb_status open_native(const NativePath &path, bool create_only = false) noexcept {
        error_ = 0;
#ifdef _WIN32
        handle_ =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        create_only ? CREATE_NEW : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            failed();
            if (error_ == ERROR_FILE_EXISTS || error_ == ERROR_ALREADY_EXISTS)
                return FQDB_ALREADY_EXISTS;
            return error_ == ERROR_SHARING_VIOLATION ? FQDB_LOCKED : FQDB_IO_ERROR;
        }
#else
        do {
            handle_ = ::open(path.c_str(),
                             O_RDWR | O_CREAT | O_CLOEXEC | (create_only ? O_EXCL : 0), 0600);
        } while (handle_ == -1 && errno == EINTR);
        if (handle_ == -1) {
            failed();
            return error_ == EEXIST ? FQDB_ALREADY_EXISTS : FQDB_IO_ERROR;
        }
        int locked;
        do {
            locked = flock(handle_, LOCK_EX | LOCK_NB);
        } while (locked && errno == EINTR);
        if (locked) {
            failed();
            return error_ == EWOULDBLOCK || error_ == EAGAIN ? FQDB_LOCKED : FQDB_IO_ERROR;
        }
#endif
        position_ = 0;
        position_known_ = true;
        return FQDB_OK;
    }
    bool size(uint64_t &out) noexcept {
        error_ = 0;
#ifdef _WIN32
        LARGE_INTEGER n;
        if (!GetFileSizeEx(handle_, &n))
            return failed();
        out = static_cast<uint64_t>(n.QuadPart);
#else
        struct stat n{};
        if (fstat(handle_, &n))
            return failed();
        out = static_cast<uint64_t>(n.st_size);
#endif
        return true;
    }
    bool seek(uint64_t offset) noexcept {
        error_ = 0;
        if (offset > static_cast<uint64_t>(INT64_MAX)) {
            error_ = EINVAL;
            return false;
        }
        if (position_known_ && offset == position_)
            return true;
#ifdef FQDB_TESTING
        ++seek_calls;
#endif
#ifdef _WIN32
        LARGE_INTEGER n;
        n.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, n, nullptr, FILE_BEGIN)) {
            position_known_ = false;
            return failed();
        }
#else
        off_t result;
        do {
            result = lseek(handle_, static_cast<off_t>(offset), SEEK_SET);
        } while (result == -1 && errno == EINTR);
        if (result == -1) {
            position_known_ = false;
            return failed();
        }
#endif
        position_ = offset;
        position_known_ = true;
        return true;
    }
    bool read(void *data, size_t size) noexcept {
        error_ = 0;
        if (!io_request(false, size)) {
            error_ = EFBIG;
            return false;
        }
        auto cursor = static_cast<unsigned char *>(data);
        while (size) {
#ifdef _WIN32
            DWORD count = 0;
            if (!ReadFile(handle_, cursor, static_cast<DWORD>(std::min<size_t>(size, 1u << 30)),
                          &count, nullptr))
                return failed();
#else
            ssize_t count = ::read(handle_, cursor, std::min<size_t>(size, 1u << 30));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return failed();
#endif
            if (!count) {
                error_ = EIO;
                return false;
            }
            cursor += count;
            size -= static_cast<size_t>(count);
            position_ += static_cast<uint64_t>(count);
        }
        return true;
    }
    bool write(const void *data, size_t size) noexcept {
        error_ = 0;
        if (!io_request(true, size)) {
            error_ = EFBIG;
            return false;
        }
        auto cursor = static_cast<const unsigned char *>(data);
        while (size) {
#ifdef FQDB_TESTING
            ++write_calls;
#endif
            ++writes_;
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(handle_, cursor, static_cast<DWORD>(std::min<size_t>(size, 1u << 30)),
                           &count, nullptr))
                return failed();
#else
            ssize_t count = ::write(handle_, cursor, std::min<size_t>(size, 1u << 30));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return failed();
#endif
            if (!count) {
                error_ = EIO;
                return false;
            }
            cursor += count;
            size -= static_cast<size_t>(count);
            position_ += static_cast<uint64_t>(count);
        }
        return true;
    }
    bool segments(std::string_view header, std::string_view payload,
                  std::string_view trailer) noexcept {
        const size_t total_bytes = header.size() + payload.size() + trailer.size();
        IOWriteReservation reservation(total_bytes);
        if (!reservation) {
            error_ = EFBIG;
            return false;
        }
#ifdef _WIN32
        const size_t total = header.size() + payload.size() + trailer.size();
        if (total <= 1024) {
            std::array<unsigned char, 1024> buffer;
            std::memcpy(buffer.data(), header.data(), header.size());
            if (!payload.empty())
                std::memcpy(buffer.data() + header.size(), payload.data(), payload.size());
            std::memcpy(buffer.data() + header.size() + payload.size(), trailer.data(),
                        trailer.size());
            return write(buffer.data(), total);
        }
        return write(header.data(), header.size()) && write(payload.data(), payload.size()) &&
               write(trailer.data(), trailer.size());
#else
        error_ = 0;
        if (!io_request(true, total_bytes)) {
            error_ = EFBIG;
            return false;
        }
        iovec vectors[3] = {{const_cast<char *>(header.data()), header.size()},
                            {const_cast<char *>(payload.data()), payload.size()},
                            {const_cast<char *>(trailer.data()), trailer.size()}};
        size_t first = 0;
        while (first < 3) {
#ifdef FQDB_TESTING
            ++write_calls;
#endif
            ++writes_;
            ssize_t count = writev(handle_, vectors + first, static_cast<int>(3 - first));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return failed();
            if (!count) {
                error_ = EIO;
                return false;
            }
            position_ += static_cast<uint64_t>(count);
            size_t consumed = static_cast<size_t>(count);
            while (first < 3 && consumed >= vectors[first].iov_len) {
                consumed -= vectors[first].iov_len;
                ++first;
            }
            if (first < 3) {
                vectors[first].iov_base = static_cast<char *>(vectors[first].iov_base) + consumed;
                vectors[first].iov_len -= consumed;
            }
        }
        return true;
#endif
    }
    bool sync() noexcept {
        io_sync();
        error_ = 0;
        ++syncs_;
#ifdef _WIN32
        return FlushFileBuffers(handle_) != 0 || failed();
#else
        int result;
        do {
            result = fsync(handle_);
        } while (result && errno == EINTR);
        return !result || failed();
#endif
    }
    bool truncate(uint64_t size) noexcept {
        if (!seek(size))
            return false;
#ifdef _WIN32
        return SetEndOfFile(handle_) != 0 || failed();
#else
        int result;
        do {
            result = ftruncate(handle_, static_cast<off_t>(size));
        } while (result && errno == EINTR);
        return !result || failed();
#endif
    }
};

inline bool sync_parent(const char *path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    std::string name(path);
    auto slash = name.find_last_of('/');
    auto parent = slash == std::string::npos ? std::string(".")
                  : slash == 0               ? std::string("/")
                                             : name.substr(0, slash);
    int fd;
    do {
        fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd == -1 && errno == EINTR);
    if (fd == -1)
        return false;
    int result;
    do {
        result = fsync(fd);
    } while (result && errno == EINTR);
    int saved = errno;
    ::close(fd);
    errno = saved;
    return result == 0;
#endif
}
inline std::array<unsigned char, file_header_size> file_header(uint32_t format,
                                                               uint64_t baseline = 0) noexcept {
    std::array<unsigned char, file_header_size> header{};
    std::memcpy(header.data(), file_magic, 8);
    put32(header.data() + 8, format);
    put32(header.data() + 12, file_header_size);
    put64(header.data() + 16, baseline);
    put32(header.data() + 24, format == 2 ? 1 : (format == 3 ? 2 : 0));
    put32(header.data() + 28, crc32(header.data(), 28));
    return header;
}
struct RecordFrame {
    std::array<unsigned char, frame_header_size> header{};
    std::array<unsigned char, trailer_size> trailer{};
    RecordFrame(const char *magic, uint64_t version, std::string_view payload) noexcept {
        initialize(magic, version, payload.size(), crc32(payload.data(), payload.size()));
    }
    RecordFrame(const char *magic, uint64_t version, size_t length, uint32_t checksum) noexcept {
        initialize(magic, version, length, checksum);
    }
    void initialize(const char *magic, uint64_t version, size_t length,
                    uint32_t checksum) noexcept {
        std::memcpy(header.data(), magic, 8);
        put64(header.data() + 8, version);
        put32(header.data() + 16, static_cast<uint32_t>(length));
        put32(header.data() + 20, checksum);
        put32(header.data() + 24, crc32(header.data(), 24));
        std::memcpy(trailer.data(), end_magic, 8);
        put64(trailer.data() + 8, version);
        put32(trailer.data() + 16, checksum);
        put32(trailer.data() + 20, crc32(trailer.data(), 20));
    }
    bool write(File &file, std::string_view payload) const noexcept {
        return file.segments({reinterpret_cast<const char *>(header.data()), header.size()},
                             payload,
                             {reinterpret_cast<const char *>(trailer.data()), trailer.size()});
    }
};
inline bool write_record(File &file, const char *magic, uint64_t version,
                         std::string_view payload) noexcept {
    return RecordFrame(magic, version, payload).write(file, payload);
}

// Only the private temporary is removed on failure. The destination is published
// without replacement, after its complete contents have been synced.
class CheckpointFile {
    NativePath target_, temporary_;
    bool created_ = false;

  public:
    File file;
    explicit CheckpointFile(const char *target) : target_(native_path(target)) {
        static std::atomic<uint64_t> counter{0};
#ifdef _WIN32
        auto suffix = std::to_string(GetCurrentProcessId());
#else
        auto suffix = std::to_string(static_cast<uint64_t>(getpid()));
#endif
        suffix += "." + std::to_string(
                            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        suffix += "." + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        temporary_ = native_path((std::string(target) + ".tmp." + suffix).c_str());
    }
    ~CheckpointFile() {
        file.close();
        if (created_) {
#ifdef _WIN32
            DeleteFileW(temporary_.c_str());
#else
            ::unlink(temporary_.c_str());
#endif
        }
    }
    fqdb_status create() noexcept {
#ifdef _WIN32
        if (GetFileAttributesW(target_.c_str()) != INVALID_FILE_ATTRIBUTES)
            return FQDB_ALREADY_EXISTS;
#else
        struct stat info{};
        if (lstat(target_.c_str(), &info) == 0)
            return FQDB_ALREADY_EXISTS;
        if (errno != ENOENT)
            return FQDB_IO_ERROR;
#endif
        auto result = file.open_native(temporary_, true);
        created_ = result == FQDB_OK;
        return result;
    }
    fqdb_status publish(const char *target, uint32_t &error) {
        file.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary_.c_str(), target_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError();
            return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ||
                           GetFileAttributesW(target_.c_str()) != INVALID_FILE_ATTRIBUTES
                       ? FQDB_ALREADY_EXISTS
                       : FQDB_IO_ERROR;
        }
        created_ = false;
        (void)target;
        return FQDB_OK;
#else
        if (::link(temporary_.c_str(), target_.c_str())) {
            error = static_cast<uint32_t>(errno);
            return errno == EEXIST ? FQDB_ALREADY_EXISTS : FQDB_IO_ERROR;
        }
        if (::unlink(temporary_.c_str()) == 0)
            created_ = false;
        if (!sync_parent(target)) {
            error = static_cast<uint32_t>(errno);
            return FQDB_COMMIT_UNKNOWN;
        }
        return FQDB_OK;
#endif
    }
};
} // namespace storage
} // namespace fengqudb
#endif
