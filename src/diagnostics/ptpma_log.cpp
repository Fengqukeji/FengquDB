// Bounded asynchronous metadata logging: producers never perform file I/O.
#include "ptpma_log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace fengqudb::diagnostics::ptpma {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t queue_limit = 4096, byte_limit = 2 * 1024 * 1024, batch_limit = 256;
constexpr std::uintmax_t rotation_bytes = 8 * 1024 * 1024;
constexpr std::array<std::string_view, 19> field_names{"run_id", "session_id", "operation_id", "status", "error_code", "duration_us", "bytes", "count", "stage", "scope", "source", "kind", "database_id", "version", "base_version", "file_offset", "os_error", "repaired_bytes", "durability"};
std::atomic<std::uint64_t> identifiers{0};
std::string path_text(const std::filesystem::path &path) { const auto s = path.u8string(); return {reinterpret_cast<const char *>(s.data()), s.size()}; }
std::uint64_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
std::int64_t micros() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string next_id() {
    static const std::uint64_t seed = [] {
        std::random_device random;
        return (static_cast<std::uint64_t>(random()) << 32) ^ random() ^
               static_cast<std::uint64_t>(micros()) ^ (process_id() << 16);
    }();
    auto value = seed + identifiers.fetch_add(1, std::memory_order_relaxed);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (unsigned i = 0; i < 16; ++i) result[15-i] = digits[(value >> (4*i)) & 15];
    return result;
}
bool label_character(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }
std::string label(std::string_view value) {
    value = value.substr(0, 100); std::string result; result.reserve(value.size());
    for (unsigned char c : value) result.push_back(label_character(c) ? static_cast<char>(c) : '_');
    return result;
}
std::string component_name(std::string value) {
    if (value.empty() || value.size() > 64 || value == "." || value == ".." || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return label_character(c); })) throw std::invalid_argument("Invalid diagnostic component name");
    for (auto &c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::filesystem::path normalized_root(const std::filesystem::path &value) {
    if (value.empty()) throw std::invalid_argument("Diagnostic directory cannot be empty");
    std::error_code error; auto absolute = std::filesystem::absolute(value, error);
    if (error) absolute = value;
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : canonical;
}
bool same_path(const std::filesystem::path &a, const std::filesystem::path &b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
struct NativeHandle {
#ifdef _WIN32
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
    void close() noexcept { if (*this) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
#else
    int value = -1;
    explicit operator bool() const { return value >= 0; }
    void close() noexcept { if (*this) ::close(value); value = -1; }
#endif
    NativeHandle() = default;
    ~NativeHandle() { close(); }
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    NativeHandle(NativeHandle &&other) noexcept : value(other.value) {
#ifdef _WIN32
        other.value = INVALID_HANDLE_VALUE;
#else
        other.value = -1;
#endif
    }
};
// Serializes batches, rotation, settings and clear across processes; never acquired by producers.
// Its fixed-size epoch also invalidates old work in writers that have not opened a log yet.
struct DirectoryLease {
    NativeHandle handle;
    explicit DirectoryLease(const std::filesystem::path &path, std::chrono::milliseconds timeout) {
#ifdef _WIN32
        handle.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
        handle.value = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
        if (!handle) throw std::runtime_error("Cannot open diagnostic control lock");
        const auto until = Clock::now() + timeout;
        for (;;) {
#ifdef _WIN32
            OVERLAPPED offset{};
            if (LockFileEx(handle.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset)) break;
            if (GetLastError() != ERROR_LOCK_VIOLATION) throw std::runtime_error("Cannot lock diagnostic directory");
#else
            if (::flock(handle.value, LOCK_EX | LOCK_NB) == 0) break;
            if (errno != EWOULDBLOCK && errno != EAGAIN) throw std::runtime_error("Cannot lock diagnostic directory");
#endif
            if (Clock::now() >= until) throw std::runtime_error("Diagnostic directory is busy");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::uint64_t epoch() const {
        std::array<unsigned char, 8> bytes{};
#ifdef _WIN32
        LARGE_INTEGER size{}, zero{}; DWORD count = 0;
        if (!GetFileSizeEx(handle.value, &size)) throw std::runtime_error("Cannot inspect diagnostic epoch");
        if (!size.QuadPart) return 1; // Existing empty control locks predate the shared epoch.
        if (size.QuadPart != static_cast<LONGLONG>(bytes.size()) || !SetFilePointerEx(handle.value, zero, nullptr, FILE_BEGIN) ||
            !ReadFile(handle.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
            throw std::runtime_error("Invalid diagnostic epoch");
#else
        struct stat info{};
        if (::fstat(handle.value, &info) != 0 || !S_ISREG(info.st_mode)) throw std::runtime_error("Cannot inspect diagnostic epoch");
        if (!info.st_size) return 1;
        ssize_t count;
        do { count = ::pread(handle.value, bytes.data(), bytes.size(), 0); } while (count < 0 && errno == EINTR);
        if (info.st_size != static_cast<off_t>(bytes.size()) || count != static_cast<ssize_t>(bytes.size())) throw std::runtime_error("Invalid diagnostic epoch");
#endif
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
        if (!value) throw std::runtime_error("Invalid diagnostic epoch");
        return value;
    }
    void epoch(std::uint64_t value) const {
        std::array<unsigned char, 8> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
#ifdef _WIN32
        LARGE_INTEGER zero{}; DWORD count = 0;
        if (!SetFilePointerEx(handle.value, zero, nullptr, FILE_BEGIN) ||
            !WriteFile(handle.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
            throw std::runtime_error("Cannot save diagnostic epoch");
#else
        ssize_t count;
        do { count = ::pwrite(handle.value, bytes.data(), bytes.size(), 0); } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(bytes.size())) throw std::runtime_error("Cannot save diagnostic epoch");
#endif
    }
};
NativeHandle reserve_deletion(const std::filesystem::path &path) {
    NativeHandle result;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY))) return result;
    result.value = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
    result.value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (result && ::flock(result.value, LOCK_EX | LOCK_NB) != 0) result.close();
#endif
    return result;
}
struct Settings {
    bool enabled = true;
    int days = 7;
    std::uintmax_t bytes = 64 * 1024 * 1024;
    Json json() const { return {{"enabled", enabled}, {"retention_days", days}, {"max_bytes", bytes}}; }
    static std::int64_t bounded(const Json &value, std::int64_t low, std::int64_t high) {
        if (!value.is_number_integer()) throw std::invalid_argument("Diagnostic setting must be an integer");
        if (value.is_number_unsigned()) return static_cast<std::int64_t>(std::clamp(value.get<std::uint64_t>(), static_cast<std::uint64_t>(low), static_cast<std::uint64_t>(high)));
        return std::clamp(value.get<std::int64_t>(), low, high);
    }
    void patch(const Json &args) {
        if (!args.is_object()) throw std::invalid_argument("Diagnostic settings must be an object");
        if (args.contains("enabled")) enabled = args.at("enabled").get<bool>();
        if (args.contains("retention_days")) {
            days = static_cast<int>(bounded(args.at("retention_days"), 1, 30));
        }
        if (args.contains("max_bytes")) {
            bytes = static_cast<std::uintmax_t>(bounded(args.at("max_bytes"), 16 * 1024 * 1024, 256 * 1024 * 1024));
        }
    }
};
Settings read_settings(const std::filesystem::path &path) {
    Settings result; std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) throw std::runtime_error("Cannot inspect diagnostic settings");
    if (exists) {
        if (!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 4096 || error) throw std::runtime_error("Invalid diagnostic settings file");
        std::ifstream input(path, std::ios::binary); std::array<char, 4097> data{}; input.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (input.bad() || input.gcount() > 4096) throw std::runtime_error("Cannot read diagnostic settings");
        result.patch(Json::parse(data.data(), data.data() + input.gcount()));
    }
    return result;
}
} // namespace
struct LocalLog::Impl {
    struct Field { std::uint8_t key; std::string value; };
    struct Entry {
        std::int64_t time = 0;
        std::uint64_t sequence = 0, thread = 0, generation = 0;
        bool priority = false;
        std::string level, event;
        std::vector<Field> fields;
        std::size_t bytes = 0;
    };
    struct File { std::filesystem::path path; std::uintmax_t bytes; std::filesystem::file_time_type modified; };
    std::filesystem::path root, active;
    std::string component, writer;
    mutable std::mutex mutex;
    mutable std::timed_mutex disk;
    std::condition_variable cv;
    std::deque<Entry> normal, critical;
    std::size_t queued_bytes = 0, inflight = 0, inflight_bytes = 0, high_water_bytes = 0, high_water_count = 0, serialization_capacity = 0;
    std::uint64_t sequence = 0, settled = 0, submitted = 0, written = 0, dropped = 0, cleared = 0, io_errors = 0, flush_timeouts = 0;
    // Zero means initialization failed: the first successful handshake drops unclassified work.
    std::uint64_t generation = 0, reported_dropped = 0, file_sequence = 0;
    std::uint64_t observed_files = 0, protected_files = 0, retention_errors = 0;
    std::uintmax_t observed_bytes = 0;
    bool directory_truncated = false, stopping = false, exited = false, reader_only = false, flush_requested = false;
    Settings settings;
    Clock::time_point retry_after{}, maintenance_after{};
    unsigned consecutive_errors = 0;
    NativeHandle output;
    std::uintmax_t active_bytes = 0;
    std::jthread worker;
    std::size_t queued() const { return normal.size() + critical.size(); }
    void root_guard(bool create = false) const {
        std::error_code error;
        if (create) { std::filesystem::create_directories(root, error); if (error) throw std::runtime_error("Cannot create local diagnostic directory"); }
        const auto resolved = std::filesystem::weakly_canonical(root, error);
        if (error || !same_path(resolved, root)) throw std::runtime_error("Diagnostic directory was redirected");
    }
    bool owns(const std::filesystem::path &path) const {
        const auto filename = path.filename().u8string();
        const std::string name(reinterpret_cast<const char *>(filename.data()), filename.size());
        if (!name.ends_with(".jsonl")) return false;
        const auto modern = component + "--", legacy = component + "-";
        const auto prefix = name.starts_with(modern) ? modern : legacy;
        if (!name.starts_with(prefix)) return false;
        const auto suffix = std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - 6);
        // Compact writer IDs use exactly 16 lowercase hex digits followed by
        // one decimal rotation counter. Keep the old four-number names below.
        if (prefix == modern && suffix.size() > 17 && suffix[16] == '-' &&
            std::all_of(suffix.begin(), suffix.begin() + 16, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) && std::all_of(suffix.begin() + 17, suffix.end(), [](char c) {
                return c >= '0' && c <= '9';
            })) return true;
        unsigned groups = 1; bool number = false;
        for (const auto c : suffix) {
            if (c >= '0' && c <= '9') number = true;
            else if (c == '-' && number) { ++groups; number = false; }
            else return false;
        }
        return number && groups == 4;
    }
    std::vector<File> files(bool &limited, Clock::time_point deadline = Clock::time_point::max()) const {
        std::vector<File> result; result.reserve(128); std::error_code error;
        if (!std::filesystem::exists(root, error)) { if (error) limited = true; return result; }
        root_guard();
        auto iterator = std::filesystem::directory_iterator(root, error);
        if (error) { limited = true; return result; }
        std::size_t inspected = 0;
        for (const auto end = std::filesystem::directory_iterator(); iterator != end; iterator.increment(error)) {
            if (error || ++inspected > 4096 || result.size() >= 512 || Clock::now() >= deadline) { limited = true; break; }
            const auto &file = *iterator;
            if (!owns(file.path())) continue;
            const auto status = file.symlink_status(error);
            if (error || !std::filesystem::is_regular_file(status)) { error.clear(); continue; }
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(file.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
#endif
            const auto size = file.file_size(error); if (error) { error.clear(); limited = true; continue; }
            const auto modified = file.last_write_time(error); if (error) { error.clear(); limited = true; continue; }
            result.push_back({file.path(), size, modified});
        }
        if (error) limited = true;
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.modified == b.modified ? a.path < b.path : a.modified < b.modified; });
        return result;
    }
    // Caller owns disk and the interprocess directory lease. Never delete another active writer.
    void retain() {
        bool limited = false; auto logs = files(limited, Clock::now() + std::chrono::milliseconds(100));
        Settings policy; { std::lock_guard lock(mutex); policy = settings; }
        std::uintmax_t total = 0; for (const auto &file : logs) total += file.bytes;
        auto count = logs.size(); std::uint64_t blocked = 0, errors = 0;
        const auto reserved_growth = output && active_bytes < rotation_bytes ? rotation_bytes - active_bytes : 0;
        const auto oldest = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * policy.days);
        for (const auto &file : logs) {
            if (file.path == active || (total + reserved_growth <= policy.bytes && file.modified >= oldest && count <= 128)) continue;
            auto reserved = reserve_deletion(file.path);
            if (!reserved) { ++blocked; continue; }
#ifdef _WIN32
            FILE_DISPOSITION_INFO disposition{TRUE};
            const bool removed = SetFileInformationByHandle(reserved.value, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
#else
            std::error_code error; const bool removed = std::filesystem::remove(file.path, error);
#endif
            if (removed) { total -= file.bytes; --count; } else ++errors;
        }
        { std::lock_guard lock(mutex); observed_bytes = total; observed_files = count; protected_files = blocked; retention_errors += errors; directory_truncated = limited; }
        maintenance_after = Clock::now() + std::chrono::seconds(30);
    }
    void open_output() {
        active = root / (component + "--" + writer + "-" + std::to_string(++file_sequence) + ".jsonl");
#ifdef _WIN32
        output.value = CreateFileW(active.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        output.value = ::open(active.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output && ::flock(output.value, LOCK_EX | LOCK_NB) != 0) output.close();
#endif
        if (!output) throw std::runtime_error("Cannot create diagnostic log");
        active_bytes = 0;
    }
    bool append(std::string_view data, std::uint64_t batch_generation) {
        std::lock_guard lock(disk);
        { std::lock_guard state_lock(mutex); if (generation != batch_generation) return false; }
        root_guard(true);
        DirectoryLease lease(root / (component + "-control.lock"), std::chrono::milliseconds(500));
        const auto shared_generation = lease.epoch();
        bool obsolete = false;
        { std::lock_guard state_lock(mutex);
          if (generation != shared_generation) {
              generation = shared_generation; cleared += queued(); normal.clear(); critical.clear(); queued_bytes = 0;
              flush_requested = inflight != 0; cv.notify_all();
          }
          obsolete = generation != batch_generation; }
        if (obsolete) { output.close(); active.clear(); active_bytes = 0; return false; }
        if (!output || active_bytes + data.size() > rotation_bytes) {
            output.close(); open_output(); retain();
        }
        std::size_t offset = 0;
        try {
            while (offset < data.size()) {
#ifdef _WIN32
                DWORD count = 0;
                if (!WriteFile(output.value, data.data() + offset, static_cast<DWORD>(data.size() - offset), &count, nullptr) || !count) throw std::runtime_error("Diagnostic write failed");
#else
                const auto count = ::write(output.value, data.data() + offset, data.size() - offset);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) throw std::runtime_error("Diagnostic write failed");
#endif
                offset += static_cast<std::size_t>(count);
            }
            active_bytes += data.size();
        } catch (...) { output.close(); throw; }
        if (Clock::now() >= maintenance_after) {
            try { retain(); }
            catch (...) { std::lock_guard state_lock(mutex); ++retention_errors; maintenance_after = Clock::now() + std::chrono::seconds(5); }
        }
        return true;
    }
    void send(std::string_view level, std::string_view event, const Fields &fields) noexcept {
        bool registered = false;
        try {
            { std::lock_guard lock(mutex);
              if (reader_only || stopping || exited || !settings.enabled) return;
              if (Clock::now() < retry_after) { ++submitted; ++dropped; return; } }
            Entry entry; entry.level = label(level); entry.event = label(event);
            entry.priority = level == "error" || event == "process.logger_stopped";
            entry.fields.reserve(std::min(fields.size(), field_names.size()));
            for (std::size_t i = 0; i < field_names.size(); ++i) {
                const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &field) { return field.first == field_names[i]; });
                if (found != fields.end()) entry.fields.push_back({static_cast<std::uint8_t>(i), label(found->second)});
            }
            entry.bytes = sizeof(Entry) + 2 * sizeof(void *) + entry.level.capacity() + entry.event.capacity() + 2 + entry.fields.capacity() * sizeof(Field);
            for (const auto &field : entry.fields) entry.bytes += field.value.capacity() + 1 + 2 * sizeof(void *);
            std::unique_lock lock(mutex);
            if (reader_only || stopping || exited || !settings.enabled) return;
            ++submitted; registered = true;
            if (Clock::now() < retry_after) { ++dropped; return; }
            if (!entry.priority && (queued() >= queue_limit || queued_bytes + entry.bytes > byte_limit)) { ++dropped; return; }
            while (queued() && (queued() >= queue_limit || queued_bytes + entry.bytes > byte_limit)) {
                auto &queue = normal.empty() ? critical : normal;
                queued_bytes -= queue.front().bytes; queue.pop_front(); ++dropped;
            }
            entry.time = micros(); entry.thread = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            entry.sequence = sequence + 1; entry.generation = generation;
            auto &queue = entry.priority ? critical : normal; queue.push_back(std::move(entry)); ++sequence;
            queued_bytes += queue.back().bytes;
            high_water_bytes = std::max(high_water_bytes, queued_bytes); high_water_count = std::max(high_water_count, queued());
            const bool wake = queued() == 1 || queued() >= 64 || !critical.empty(); lock.unlock(); if (wake) cv.notify_one();
        } catch (...) { try { std::lock_guard lock(mutex); if (!registered) ++submitted; ++dropped; } catch (...) {} }
    }
    void run() noexcept {
        try {
            std::vector<Entry> batch; batch.reserve(batch_limit);
            std::string data; data.reserve(256 * 1024);
            for (;;) {
                batch.clear(); data.clear();
                std::uint64_t batch_generation = 0, last = 0, lost = 0;
                { std::unique_lock lock(mutex);
                  cv.wait(lock, [&] { return stopping || queued() || flush_requested; });
                  if (stopping && !queued()) break;
                  if (!stopping && !flush_requested && critical.empty() && queued() < 64)
                      cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return stopping || flush_requested || !critical.empty() || queued() >= 64; });
                  if (!queued()) { settled = sequence; flush_requested = false; cv.notify_all(); continue; }
                  batch_generation = generation; lost = dropped;
                  while (queued() && batch.size() < batch_limit) {
                      auto &queue = critical.empty() || (!normal.empty() && normal.front().sequence < critical.front().sequence) ? normal : critical;
                      queued_bytes -= queue.front().bytes; batch.push_back(std::move(queue.front())); queue.pop_front();
                  }
                  inflight = batch.size(); inflight_bytes = 0; for (const auto &entry : batch) inflight_bytes += entry.bytes;
                  last = batch.back().sequence; }
                try {
                    bool obsolete;
                    { std::lock_guard lock(mutex); obsolete = batch_generation != generation; }
                    bool persisted = false;
                    if (!obsolete) {
                        for (const auto &entry : batch) {
                            Json metadata = Json::object(); for (const auto &field : entry.fields) metadata[std::string(field_names[field.key])] = field.value;
                            data += Json{{"version", 1}, {"time_unix_us", entry.time}, {"process_id", process_id()}, {"thread_id", entry.thread}, {"writer_id", writer}, {"generation", entry.generation}, {"component", component}, {"sequence", entry.sequence}, {"level", entry.level}, {"event", entry.event}, {"fields", std::move(metadata)}}.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
                        }
                        if (lost > reported_dropped) data += Json{{"version", 1}, {"time_unix_us", micros()}, {"process_id", process_id()}, {"writer_id", writer}, {"generation", batch_generation}, {"component", component}, {"level", "warning"}, {"event", "logger.dropped"}, {"fields", {{"count", std::to_string(lost)}}}}.dump() + "\n";
                        { std::lock_guard lock(mutex); serialization_capacity = data.capacity(); }
                        persisted = append(data, batch_generation);
                    }
                    { std::lock_guard lock(mutex);
                      if (persisted) { written += batch.size(); reported_dropped = lost; consecutive_errors = 0; }
                      else cleared += batch.size(); }
                } catch (...) {
                    std::lock_guard lock(mutex); ++io_errors; dropped += batch.size();
                    if (generation == batch_generation) {
                        dropped += queued(); normal.clear(); critical.clear(); queued_bytes = 0;
                        consecutive_errors = std::min(consecutive_errors + 1, 5u);
                        retry_after = Clock::now() + std::chrono::milliseconds(250 * (1u << consecutive_errors));
                    }
                }
                { std::lock_guard lock(mutex);
                  inflight = 0; inflight_bytes = 0; settled = std::max(settled, last);
                  if (!queued()) { settled = sequence; flush_requested = false; }
                  cv.notify_all(); }
            }
        } catch (...) { try { std::lock_guard lock(mutex); ++io_errors; dropped += queued() + inflight; normal.clear(); critical.clear(); queued_bytes = inflight_bytes = inflight = 0; } catch (...) {} }
        { std::lock_guard lock(mutex); exited = true; settled = sequence; cv.notify_all(); }
    }
};
LocalLog::LocalLog(std::filesystem::path directory, std::string component, bool reader_only) : impl_(std::make_unique<Impl>()) {
    impl_->root = normalized_root(directory); impl_->component = component_name(std::move(component)); impl_->writer = next_id(); impl_->reader_only = reader_only;
    try { impl_->settings = read_settings(impl_->root / (impl_->component + "-settings.json")); } catch (...) { ++impl_->io_errors; }
    if (!reader_only) {
        try {
            impl_->root_guard(true); DirectoryLease lease(impl_->root / (impl_->component + "-control.lock"), std::chrono::milliseconds(500));
            impl_->generation = lease.epoch();
        } catch (...) { ++impl_->io_errors; }
        try { impl_->worker = std::jthread([state = impl_.get()] { state->run(); }); write("info", "process.logger_started"); }
        catch (...) { ++impl_->io_errors; impl_->exited = true; }
    }
}
LocalLog::~LocalLog() {
    write("info", "process.logger_stopped");
    { std::lock_guard lock(impl_->mutex); impl_->stopping = true; }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->output.close();
}
void LocalLog::write(std::string_view level, std::string_view event, const Fields &fields) noexcept { impl_->send(level, event, fields); }
Json LocalLog::status() const {
    std::lock_guard lock(impl_->mutex);
    return {{"directory", path_text(impl_->root)}, {"component", impl_->component}, {"writer_id", impl_->writer}, {"reader_only", impl_->reader_only}, {"enabled", impl_->settings.enabled}, {"retention_days", impl_->settings.days}, {"max_bytes", impl_->settings.bytes}, {"queued", impl_->queued()}, {"queued_bytes", impl_->queued_bytes}, {"inflight", impl_->inflight}, {"inflight_bytes", impl_->inflight_bytes}, {"serialization_capacity", impl_->serialization_capacity}, {"queue_limit", queue_limit}, {"queue_byte_limit", byte_limit}, {"high_water_queued", impl_->high_water_count}, {"high_water_bytes", impl_->high_water_bytes}, {"submitted", impl_->submitted}, {"written", impl_->written}, {"dropped", impl_->dropped}, {"cleared", impl_->cleared}, {"io_errors", impl_->io_errors}, {"flush_timeouts", impl_->flush_timeouts}, {"generation", impl_->generation}, {"settled_sequence", impl_->settled}, {"last_sequence", impl_->sequence}, {"worker_running", !impl_->reader_only && !impl_->exited}, {"disk_bytes_at_maintenance", impl_->observed_bytes}, {"files_at_maintenance", impl_->observed_files}, {"protected_files", impl_->protected_files}, {"retention_errors", impl_->retention_errors}, {"directory_truncated", impl_->directory_truncated}, {"sensitive_payloads", false}};
}
bool LocalLog::flush(std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::minutes(1)) throw std::invalid_argument("Diagnostic flush timeout is out of range");
    std::unique_lock lock(impl_->mutex);
    if (impl_->reader_only) return true;
    const auto target = impl_->sequence;
    if (impl_->settled >= target) return true;
    impl_->flush_requested = true; impl_->cv.notify_one();
    const bool complete = impl_->cv.wait_for(lock, timeout, [&] { return impl_->settled >= target || impl_->exited; });
    if (!complete) ++impl_->flush_timeouts;
    return complete && impl_->settled >= target;
}
Json LocalLog::query(const Json &args) const {
    if (!args.is_object()) throw std::invalid_argument("Diagnostic query must be an object");
    const auto limit = static_cast<std::size_t>(std::clamp(args.value("limit", 200), 1, 3000));
    const auto run = args.value("run_id", ""), session = args.value("session_id", ""), event = args.value("event", "");
    if (run.size() > 100 || session.size() > 100 || event.size() > 100) throw std::invalid_argument("Diagnostic filter is too long");
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    std::deque<Json> result; bool truncated = false; std::size_t scanned = 0, bytes = 0, returned_bytes = 0;
    std::unique_lock lock(impl_->disk, std::defer_lock);
    if (!lock.try_lock_until(deadline)) return {{"items", Json::array()}, {"truncated", true}, {"busy", true}, {"scanned", 0}, {"source", "local-operation-log"}};
    auto files = impl_->files(truncated, deadline);
    for (auto file = files.rbegin(); file != files.rend(); ++file) {
        if (bytes >= 8 * 1024 * 1024 || Clock::now() >= deadline || result.size() >= limit || returned_bytes >= 768 * 1024) { truncated = true; break; }
        std::ifstream input(file->path, std::ios::binary | std::ios::ate); const auto end_position = input.tellg();
        if (end_position < 0) { truncated = true; continue; }
        auto cursor = static_cast<std::uintmax_t>(end_position); std::string carry; bool discard_fragment = false;
        auto consume = [&](std::string_view line) {
            if (line.empty()) return;
            ++scanned;
            if (line.size() > 16000) { truncated = true; return; }
            if ((!run.empty() && line.find(run) == std::string_view::npos) || (!session.empty() && line.find(session) == std::string_view::npos) || (!event.empty() && line.find(event) == std::string_view::npos)) return;
            try {
                auto entry = Json::parse(line.begin(), line.end(), [](int depth, Json::parse_event_t, Json &) { if (depth > 8) throw std::runtime_error("Diagnostic nesting limit"); return true; });
                if (!entry.is_object() || entry.value("version", 0) != 1 || entry.value("component", "") != impl_->component || !entry.at("fields").is_object()) { truncated = true; return; }
                const auto &fields = entry.at("fields");
                if ((!run.empty() && fields.value("run_id", "") != run) || (!session.empty() && fields.value("session_id", "") != session) || (!event.empty() && entry.value("event", "").find(event) == std::string::npos)) return;
                Json clean = {{"version", 1}, {"component", impl_->component}, {"fields", Json::object()}};
                for (const auto *key : {"time_unix_us", "process_id", "thread_id", "sequence", "generation"}) if (entry.contains(key) && entry.at(key).is_number_integer()) clean[key] = entry.at(key);
                for (const auto *key : {"event", "level", "writer_id"}) if (entry.contains(key) && entry.at(key).is_string()) clean[key] = label(entry.at(key).get_ref<const std::string &>());
                for (const auto key : field_names) { const auto found = fields.find(std::string(key)); if (found != fields.end() && found->is_string()) clean["fields"][std::string(key)] = label(found->get_ref<const std::string &>()); }
                const auto size = clean.dump().size();
                if (returned_bytes + size > 768 * 1024) { returned_bytes = 768 * 1024; truncated = true; return; }
                returned_bytes += size; result.push_front(std::move(clean));
            } catch (...) { truncated = true; }
        };
        while (cursor && result.size() < limit && returned_bytes < 768 * 1024) {
            if (bytes >= 8 * 1024 * 1024 || Clock::now() >= deadline) { truncated = true; break; }
            const auto count = static_cast<std::size_t>(std::min<std::uintmax_t>({cursor, 65536, 8 * 1024 * 1024 - bytes}));
            cursor -= count; input.seekg(static_cast<std::streamoff>(cursor));
            std::string block(count, '\0'); input.read(block.data(), static_cast<std::streamsize>(count)); if (!input) { truncated = true; break; }
            bytes += count; block += carry; carry.clear(); auto end = block.size();
            if (discard_fragment) { const auto boundary = block.rfind('\n'); if (boundary == std::string::npos) continue; end = boundary; discard_fragment = false; }
            while (end && result.size() < limit && returned_bytes < 768 * 1024) {
                if (Clock::now() >= deadline) { truncated = true; break; }
                const auto boundary = block.rfind('\n', end - 1);
                if (boundary == std::string::npos) {
                    if (!cursor) consume(std::string_view(block).substr(0, end));
                    else if (end <= 16000) carry.assign(block, 0, end);
                    else { discard_fragment = true; truncated = true; }
                    break;
                }
                consume(std::string_view(block).substr(boundary + 1, end - boundary - 1)); end = boundary;
            }
            if ((result.size() >= limit || returned_bytes >= 768 * 1024) && (cursor || end)) truncated = true;
        }
    }
    Json entries = Json::array(); for (auto &entry : result) entries.push_back(std::move(entry));
    return {{"items", std::move(entries)}, {"truncated", truncated}, {"scanned", scanned}, {"scanned_bytes", bytes}, {"returned_bytes", returned_bytes}, {"source", "local-operation-log"}, {"payload_policy", "operation metadata only; no prompts, keys, screenshots or keystrokes"}};
}
Json LocalLog::configure(const Json &args) {
    if (impl_->reader_only) throw std::runtime_error("Read-only diagnostic source cannot change settings");
    std::unique_lock disk_lock(impl_->disk, std::defer_lock);
    if (!disk_lock.try_lock_for(std::chrono::milliseconds(500))) throw std::runtime_error("Diagnostic writer is busy");
    impl_->root_guard(true); DirectoryLease lease(impl_->root / (impl_->component + "-control.lock"), std::chrono::milliseconds(500));
    Settings candidate;
    try { candidate = read_settings(impl_->root / (impl_->component + "-settings.json")); }
    catch (...) { std::lock_guard lock(impl_->mutex); candidate = impl_->settings; }
    candidate.patch(args);
    const auto file = impl_->root / (impl_->component + "-settings.json"), temporary = impl_->root / (impl_->component + "--" + next_id() + "-settings.tmp");
    try {
        { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); const auto text = candidate.json().dump(); output.write(text.data(), static_cast<std::streamsize>(text.size())); output.flush(); if (!output) throw std::runtime_error("Cannot save diagnostic settings"); }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace diagnostic settings");
#else
        std::filesystem::rename(temporary, file);
#endif
    } catch (...) { std::error_code error; std::filesystem::remove(temporary, error); { std::lock_guard lock(impl_->mutex); ++impl_->io_errors; } throw; }
    { std::lock_guard lock(impl_->mutex); impl_->settings = candidate; }
    try { impl_->retain(); } catch (...) { std::lock_guard lock(impl_->mutex); ++impl_->retention_errors; }
    return status();
}
Json LocalLog::clear() {
    if (impl_->reader_only) throw std::runtime_error("Read-only diagnostic source cannot clear logs");
    std::unique_lock disk_lock(impl_->disk, std::defer_lock);
    if (!disk_lock.try_lock_for(std::chrono::milliseconds(500))) throw std::runtime_error("Diagnostic writer is busy");
    impl_->root_guard(true); DirectoryLease lease(impl_->root / (impl_->component + "-control.lock"), std::chrono::milliseconds(500));
    bool limited = false; auto files = impl_->files(limited, Clock::now() + std::chrono::milliseconds(500));
    if (limited) throw std::runtime_error("Diagnostic directory scan incomplete; no clear was performed");
    impl_->output.close();
    std::vector<NativeHandle> reservations; reservations.reserve(files.size());
    for (const auto &file : files) { auto reserved = reserve_deletion(file.path); if (!reserved) throw std::runtime_error("Diagnostic logs are protected or another writer is active; no clear was performed"); reservations.push_back(std::move(reserved)); }
    const auto previous_generation = lease.epoch();
    if (previous_generation == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Diagnostic epoch exhausted; no clear was performed");
    lease.epoch(previous_generation + 1);
    { std::lock_guard lock(impl_->mutex); impl_->generation = previous_generation + 1; impl_->cleared += impl_->queued(); impl_->normal.clear(); impl_->critical.clear(); impl_->queued_bytes = 0;
      if (!impl_->inflight) impl_->settled = impl_->sequence;
      impl_->flush_requested = impl_->inflight != 0; impl_->cv.notify_all(); }
    impl_->active.clear(); impl_->active_bytes = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
#ifdef _WIN32
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(reservations[i].value, FileDispositionInfo, &disposition, sizeof(disposition))) throw std::runtime_error("Could not remove every diagnostic log");
#else
        if (!std::filesystem::remove(files[i].path)) throw std::runtime_error("Could not remove every diagnostic log");
#endif
    }
    { std::lock_guard lock(impl_->mutex); impl_->observed_files = impl_->observed_bytes = impl_->protected_files = 0; }
    return status();
}
} // namespace fengqudb::diagnostics::ptpma
