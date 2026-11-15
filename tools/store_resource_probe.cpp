// Linux-only probe for real OS resource failures, using the production C/C++ API.
// No FQDB_TESTING hooks. Run only inside the isolated resource-test container.
#include "fengqudb_store.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;
using fengqudb::Store;
constexpr size_t mib = 1024 * 1024;
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void ok(fqdb_status status) {
    if (status != FQDB_OK)
        throw fengqudb::Error(status);
}
static uint64_t number(std::string_view text) {
    uint64_t result = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "invalid number");
    return result;
}
static void event(const char *name, const std::string &fields = {}) {
    std::cout << "{\"event\":\"" << name << '"' << fields << "}\n" << std::flush;
    check(bool(std::cout), "stdout acknowledgement failed");
}
static fengqudb::StoreOptions settings(bool create = true) {
    fengqudb::StoreOptions options;
    options.durability = fengqudb::Durability::sync;
    options.create = create;
    options.cache_bytes = mib;
    options.delta_bytes = 262144;
    options.writer_bytes = 4 * mib;
    options.transaction_bytes = 16 * mib;
    options.all_transaction_bytes = 32 * mib;
    options.pinned_bytes = 8 * mib;
    options.max_snapshots = 64;
    return options;
}
static Store open(const std::string &root, bool create = true) {
    auto db = Store::open_at(root + "/db", "main", settings(create));
    db.configure_write({fengqudb::StoreWriteMode::Reliable, 50, mib});
    return db;
}
static std::string row_key(uint64_t n, char part) {
    auto digits = std::to_string(n);
    return "r/" + std::string(10 - digits.size(), '0') + digits + '/' + part;
}
static std::string payload(uint64_t n, size_t bytes) {
    fengqudb::wire::Writer writer;
    writer.uint_field(1, n);
    writer.bytes_field(2, std::string(bytes, static_cast<char>('a' + n % 26)));
    return writer.take();
}
static uint64_t head(Store &db) {
    auto value = db.get("head");
    return value ? number(value->view()) : 0;
}
static uint64_t append(Store &db, uint64_t n, size_t bytes, bool announce = false) {
    auto data = payload(n, bytes);
    auto tx = db.transaction();
    tx.put(row_key(n, 'a'), data);
    tx.put(row_key(n, 'b'), "mirror/" + std::to_string(n));
    tx.put("head", std::to_string(n));
    if (announce)
        event("begin", ",\"sequence\":" + std::to_string(n));
    auto result = tx.commit(true);
    if (!result)
        throw fengqudb::Error(static_cast<fqdb_status>(result.status));
    check(db.info().durable_version == result.version, "acknowledged transaction was not SYNC");
    return result.version;
}
static void verify_prefix(Store &db, uint64_t acknowledged, uint64_t maximum, size_t bytes) {
    auto highest = head(db);
    check(highest >= acknowledged && highest <= maximum, "recovery lost acknowledged prefix");
    for (uint64_t n = 1; n <= highest; ++n) {
        auto a = db.get(row_key(n, 'a'));
        auto b = db.get(row_key(n, 'b'));
        check(a && a->view() == payload(n, bytes), "missing or damaged acknowledged payload");
        check(b && b->view() == "mirror/" + std::to_string(n), "partial multi-key transaction");
    }
    check(!db.get(row_key(highest + 1, 'a')) && !db.get(row_key(highest + 1, 'b')),
          "unpublished transaction leaked individual rows");
    check(db.info().stored_keys == highest * 2 + (highest ? 1 : 0), "unexpected recovered rows");
    db.verify();
}
static uint64_t available(const std::string &root) {
    struct statvfs stat{};
    check(statvfs(root.c_str(), &stat) == 0, "statvfs failed");
    return static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
}
class ReserveFile {
    std::string path_;
    int descriptor_ = -1;

  public:
    explicit ReserveFile(const std::string &root) : path_(root + "/reserve.bin") {
        descriptor_ = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        check(descriptor_ >= 0, "cannot create owned reserve file");
    }
    ReserveFile(const ReserveFile &) = delete;
    ~ReserveFile() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
        if (!path_.empty())
            ::unlink(path_.c_str());
    }
    void write(uint64_t bytes) {
        std::array<char, 65536> data{};
        while (bytes) {
            auto size = static_cast<size_t>(std::min<uint64_t>(bytes, data.size()));
            auto count = ::write(descriptor_, data.data(), size);
            if (count < 0 && errno == EINTR)
                continue;
            check(count > 0, "cannot allocate reserve file");
            bytes -= static_cast<uint64_t>(count);
        }
        check(fsync(descriptor_) == 0, "cannot sync reserve file");
    }
    void release() {
        check(descriptor_ >= 0 && ::close(descriptor_) == 0, "cannot close reserve file");
        descriptor_ = -1;
        check(::unlink(path_.c_str()) == 0, "cannot release reserved space");
        path_.clear();
    }
};
static int confirm_enospc(const std::string &root) {
    auto path = root + "/full-check.bin";
    auto descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return errno;
    char block[4096]{};
    auto result = ::write(descriptor, block, sizeof(block));
    auto error = result < 0 ? errno : 0;
    ::close(descriptor);
    ::unlink(path.c_str());
    return error;
}
static void disk_full(const std::string &root) {
    const size_t bytes = 131072;
    const auto capacity = available(root);
    check(capacity >= 8 * mib && capacity <= 32 * mib, "requires an isolated 8..32 MiB filesystem");
    ReserveFile reserve(root);
    reserve.write(2 * mib); // Leave enough recovery headroom after the failure.
    uint64_t acknowledged = 0, attempted = 0;
    fqdb_status failure = FQDB_OK;
    {
        auto db = open(root);
        for (uint64_t n = 1; n <= 512; ++n) {
            attempted = n;
            try {
                append(db, n, bytes);
                acknowledged = n;
            } catch (const fengqudb::Error &error) {
                failure = error.code();
                break;
            }
        }
        check(acknowledged > 0 && (failure == FQDB_IO_ERROR || failure == FQDB_COMMIT_UNKNOWN),
              "filesystem limit did not fail a real write");
        check(head(db) == acknowledged, "failed append published a partial view");
        check(confirm_enospc(root) == ENOSPC, "filesystem did not report actual ENOSPC");
        fqdb_status continued = FQDB_OK;
        try {
            append(db, attempted + 1, 64);
        } catch (const fengqudb::Error &error) {
            continued = error.code();
        }
        check(continued == FQDB_RECOVERY_REQUIRED, "writer continued after real write failure");
        event("disk_full", ",\"acknowledged\":" + std::to_string(acknowledged) + ",\"status\":" +
                               std::to_string(failure) + ",\"errno\":" + std::to_string(ENOSPC) +
                               ",\"available_bytes\":" + std::to_string(available(root)));
    }
    reserve.release();
    auto db = open(root, false);
    verify_prefix(db, acknowledged, attempted, bytes);
    auto recovered = head(db);
    auto repaired = db.info().repaired_bytes;
    append(db, recovered + 1, bytes);
    verify_prefix(db, recovered + 1, recovered + 1, bytes);
    db.checkpoint();
    event("pass", ",\"case\":\"disk_full\",\"acknowledged\":" + std::to_string(acknowledged) +
                      ",\"recovered\":" + std::to_string(recovered) +
                      ",\"repaired_bytes\":" + std::to_string(repaired));
}
static std::string current_bytes(const std::string &root) {
    std::ifstream file(root + "/db/main/CURRENT", std::ios::binary);
    check(bool(file), "cannot read CURRENT");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void disk_compact(const std::string &root) {
    check(available(root) >= 8 * mib && available(root) <= 32 * mib,
          "requires an isolated 8..32 MiB filesystem");
    auto db = open(root);
    for (uint64_t n = 1; n <= 16; ++n)
        append(db, n, 131072);
    for (unsigned i = 0; i < 12; ++i)
        db.put_if_changed("obsolete", std::string(262144, static_cast<char>('a' + i)));
    auto snapshot = db.snapshot();
    auto current = current_bytes(root);
    auto version = db.info().version;
    const auto files_before = [&] {
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(root + "/db/main"))
            files.push_back(entry.path().filename());
        std::sort(files.begin(), files.end());
        return files;
    }();
    ReserveFile reserve(root);
    auto free = available(root);
    check(free > 131072, "insufficient room for compaction failure setup");
    reserve.write(free - 65536);
    fqdb_status failure = FQDB_OK;
    try {
        db.compact();
    } catch (const fengqudb::Error &error) {
        failure = error.code();
    }
    check(failure == FQDB_IO_ERROR, "candidate ENOSPC was not reported as an I/O failure");
    check(current_bytes(root) == current && db.info().version == version,
          "failed compaction published a candidate");
    check(snapshot.get(row_key(1, 'a'))->view() == payload(1, 131072),
          "failed compaction invalidated a snapshot");
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(root + "/db/main")) {
        check(entry.is_regular_file(), "compaction created nested output");
        files.push_back(entry.path().filename());
    }
    std::sort(files.begin(), files.end());
    check(files == files_before, "failed compaction leaked a candidate");
    reserve.release();
    db.put_if_changed("after-failure", "writer remains usable");
    db.compact();
    db.verify();
    check(snapshot.get("obsolete")->view() == std::string(262144, 'l'),
          "retry compaction invalidated a snapshot");
    fs::create_directory(root + "/bak");
    db.backup(root + "/bak/base");
    event("pass", ",\"case\":\"disk_compact\",\"status\":" + std::to_string(failure) +
                      ",\"version\":" + std::to_string(db.info().version));
}
static uint64_t virtual_bytes() {
    std::ifstream file("/proc/self/statm");
    uint64_t pages = 0;
    check(bool(file >> pages), "cannot read process memory map");
    return pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}
class AddressLimit {
    struct rlimit old_{};

  public:
    explicit AddressLimit(uint64_t maximum) {
        check(getrlimit(RLIMIT_AS, &old_) == 0, "cannot read address-space limit");
        auto next = old_;
        next.rlim_cur = maximum;
        check(setrlimit(RLIMIT_AS, &next) == 0, "cannot set address-space limit");
    }
    ~AddressLimit() {
        setrlimit(RLIMIT_AS, &old_);
    }
};
static void allocation_failure(const std::string &root) {
    auto db = open(root);
    append(db, 1, 65536);
    const std::string input(mib, 'x'); // The caller-owned input exists before pressure.
    fqdb_store *raw = nullptr;
    // Use a second independent Store to exercise the public C error boundary.
    auto options = settings().native();
    ok(fqdb_store_open_at((root + "/db").c_str(), "alloc", &options, &raw));
    std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> owner(raw, fqdb_store_close);
    fqdb_store_txn *tx = nullptr;
    ok(fqdb_store_txn_begin(raw, 0, &tx));
    std::unique_ptr<fqdb_store_txn, decltype(&fqdb_store_txn_close)> transaction(
        tx, fqdb_store_txn_close);
    ok(fqdb_store_txn_put(tx, {"guard", 5}, {"uncommitted", 11}, 0));
    std::vector<void *> pressure;
    pressure.reserve(1024);
    const auto maximum = virtual_bytes() + 32 * mib;
    fqdb_status result = FQDB_OK;
    {
        AddressLimit limit(maximum);
        while (pressure.size() < pressure.capacity()) {
            auto block = std::malloc(262144);
            if (!block)
                break;
            pressure.push_back(block);
            auto pages = static_cast<volatile char *>(block);
            for (size_t i = 0; i < 262144; i += 4096)
                pages[i] = 1;
        }
        result = fqdb_store_txn_put(tx, {"large", 5}, {input.data(), input.size()}, 0);
        for (auto block : pressure)
            std::free(block);
    }
    check(result == FQDB_NO_MEMORY, "actual address-space pressure did not reach native NO_MEMORY");
    uint64_t version = 999;
    check(fqdb_store_txn_commit(tx, &version) == FQDB_INVALID_ARGUMENT && version == 0,
          "allocation failure committed a partial transaction");
    transaction.reset();
    fqdb_buffer value{};
    check(fqdb_store_get(raw, {"guard", 5}, &value) == FQDB_NOT_FOUND && !value.data,
          "allocation failure published staged data");
    verify_prefix(db, 1, 1, 65536);
    append(db, 2, 65536);
    verify_prefix(db, 2, 2, 65536);
    // Also verify that the failed Store can accept a fresh complete transaction.
    ok(fqdb_store_txn_begin(raw, 0, &tx));
    transaction.reset(tx);
    ok(fqdb_store_txn_put(tx, {"healthy", 7}, {"yes", 3}, 0));
    ok(fqdb_store_txn_commit(tx, &version));
    event("pass", ",\"case\":\"allocation_failure\",\"status\":" + std::to_string(result) +
                      ",\"rlimit_as\":" + std::to_string(maximum) +
                      ",\"pressure_blocks\":" + std::to_string(pressure.size()));
}
static void stream(const std::string &root, bool oom, size_t bytes) {
    auto db = open(root);
    std::vector<std::unique_ptr<char[]>> pressure;
    pressure.reserve(256);
    event("ready", ",\"mode\":\"" + std::string(oom ? "oom" : "kill") +
                       "\",\"blob_bytes\":" + std::to_string(bytes));
    for (uint64_t n = 1; n <= 128; ++n) {
        auto version = append(db, n, bytes, true);
        event("ack",
              ",\"sequence\":" + std::to_string(n) + ",\"version\":" + std::to_string(version));
        if (oom) {
            auto block = std::make_unique<char[]>(mib);
            auto pages = static_cast<volatile char *>(block.get());
            for (size_t i = 0; i < mib; i += 4096)
                pages[i] = static_cast<char>(n);
            pressure.push_back(std::move(block));
        }
    }
    throw std::runtime_error(
        "writer reached its finite ceiling without the expected external kill");
}
static void recover(const std::string &root, uint64_t acknowledged, size_t bytes) {
    uint64_t recovered = 0, repaired = 0, replayed = 0;
    {
        auto db = open(root, false);
        verify_prefix(db, acknowledged, 128, bytes);
        recovered = head(db);
        repaired = db.info().repaired_bytes;
        replayed = db.info().recovered_transactions;
        append(db, recovered + 1, bytes);
        db.checkpoint();
    }
    {
        auto db = open(root, false);
        verify_prefix(db, recovered + 1, recovered + 1, bytes);
    }
    event("pass", ",\"case\":\"recover\",\"acknowledged\":" + std::to_string(acknowledged) +
                      ",\"recovered\":" + std::to_string(recovered) +
                      ",\"repaired_bytes\":" + std::to_string(repaired) +
                      ",\"replayed_transactions\":" + std::to_string(replayed));
}
int main(int argc, char **argv) {
    try {
        check(argc >= 3, "usage: probe CASE ROOT [ACKNOWLEDGED BLOB_BYTES]");
        const std::string mode(argv[1]), root(argv[2]);
        check(root == "/data", "probe must use its isolated /data mount");
        check(mode == "recover" || !fs::exists(root + "/db"),
              "probe requires a fresh isolated root");
        if (mode == "disk-full")
            disk_full(root);
        else if (mode == "disk-compact")
            disk_compact(root);
        else if (mode == "allocation")
            allocation_failure(root);
        else if (mode == "oom" || mode == "kill") {
            check(argc == 4, "writer requires blob size");
            auto bytes = number(argv[3]);
            check(bytes >= 65536 && bytes <= 4 * mib, "invalid blob size");
            stream(root, mode == "oom", static_cast<size_t>(bytes));
        } else if (mode == "recover") {
            check(argc == 5, "recovery requires ack and blob size");
            recover(root, number(argv[3]), static_cast<size_t>(number(argv[4])));
        } else
            throw std::invalid_argument("unknown probe case");
        return 0;
    } catch (const fengqudb::Error &error) {
        std::cerr << "database status=" << error.code() << ": " << error.what() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
