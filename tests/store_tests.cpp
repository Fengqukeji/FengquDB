#include "strict_store.hpp"
#include "fengqudb_store.hpp"
#include "paged/core.hpp"
#include "store_clock.hpp"
#include "store_trace.hpp"
#include <future>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace p = fengqudb::paged;
using Store = test_support::StrictStore;
using fengqudb::StoreOptions;
extern "C" void fqdb_store_test_crash(int);
extern "C" void fqdb_store_test_io_fault(int, int);
extern "C" uintptr_t fqdb_store_test_staged_pointer(fqdb_store_txn *, fqdb_slice);
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void ok(fqdb_status status) {
    if (status)
        throw fengqudb::Error(status);
}
static fqdb_slice slice(std::string_view value) {
    return {value.data(), value.size()};
}
struct LogProbe {
    fqdb_store *store;
    std::atomic<unsigned> calls{0};
    std::atomic<bool> bad{false};
};
static void FQDB_CALL log_probe(void *user, const fqdb_log_event *event) {
    auto &probe = *static_cast<LogProbe *>(user);
    fqdb_store_info info{};
    if (!event->database_id || fqdb_store_get_info(probe.store, &info) != FQDB_OK)
        probe.bad.store(true);
    ++probe.calls;
}
static void committed(fengqudb::StoreTransaction &tx) {
    auto result = tx.commit();
    check(bool(result), "transaction did not commit");
}
struct Owned {
    std::string path;
    Owned()
        : path((std::filesystem::temp_directory_path() / ("fqdb-store-" + p::hex(p::new_uuid())))
                   .u8string()) {}
    ~Owned() {
        try {
            // Only known database files in this exclusively created fixture.
            if (std::filesystem::exists(std::filesystem::u8path(p::join(path, "CURRENT")))) {
                auto current = p::read_current(path);
                p::remove_owned(p::join(path, current.filename()));
                p::remove_owned(p::join(path, "CURRENT"));
            }
            p::remove_owned(p::join(path, "LOCK"));
            p::remove_owned(p::join(path, "WRITE"));
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::u8path(path), ignored);
        } catch (...) {
        }
    }
};
static int child(const std::string &executable, const std::string &path, int point) {
#ifdef _WIN32
    auto command = fengqudb::storage::native_path(
        ("\"" + executable + "\" --crash \"" + path + "\" " + std::to_string(point)).c_str());
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &start, &process) != 0,
          "create crash child");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    check(GetExitCodeProcess(process.hProcess, &code) != 0, "read child status");
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    auto pid = fork();
    check(pid >= 0, "fork crash child");
    if (!pid) {
        auto number = std::to_string(point);
        execl(executable.c_str(), executable.c_str(), "--crash", path.c_str(), number.c_str(),
              static_cast<char *>(nullptr));
        std::_Exit(87);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status), "wait crash child");
    return WEXITSTATUS(status);
#endif
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--crash") {
            Store store(argv[2]);
            auto tx = store.transaction();
            tx.put("crash", "complete");
            fqdb_store_test_crash(std::stoi(argv[3]));
            committed(tx);
            return 88;
        }
        store_test_trace("model/start");
        Owned owned;
        StoreOptions options;
        options.cache_bytes = 2 * 1024 * 1024;
        options.delta_bytes = 65536;
        options.writer_bytes = 2 * 1024 * 1024;
        std::map<std::string, std::string> model;
        std::mt19937 random(771234);
        {
            Store store(owned.path, options);
            bool locked = false;
            try {
                Store duplicate(owned.path, options);
            } catch (const fengqudb::Error &e) {
                locked = e.code() == FQDB_LOCKED;
            }
            check(locked, "directory lock");
            std::vector<std::pair<fengqudb::StoreSnapshot, decltype(model)>> snapshots;
            for (unsigned round = 0; round < 100; ++round) {
                auto tx = store.transaction();
                auto candidate = model;
                for (unsigned n = 0; n < 10; ++n) {
                    auto key = "key/" + std::to_string(random() % 400);
                    if (n % 4 == 0) {
                        tx.erase(key);
                        candidate.erase(key);
                    } else {
                        auto value = std::to_string(random());
                        tx.put(key, value);
                        candidate[key] = value;
                    }
                }
                auto mark = tx.savepoint();
                tx.put("discarded", "x");
                tx.rollback_to(mark);
                tx.release(mark);
                committed(tx);
                model = std::move(candidate);
                if (round % 30 == 0)
                    snapshots.emplace_back(store.snapshot(), model);
            }
            store_test_trace("model/reads");
            for (const auto &entry : model)
                check(store.get(entry.first)->view() == entry.second, "store/model point read");
            for (auto &snapshot : snapshots) {
                auto cursor = snapshot.first.scan();
                auto it = snapshot.second.begin();
                while (auto row = cursor.next()) {
                    check(it != snapshot.second.end() && row->key == it->first &&
                              row->value == it->second,
                          "old snapshot changed");
                    ++it;
                }
                check(it == snapshot.second.end(), "snapshot ended early");
            }
            auto snapshot = store.snapshot();
            auto forward = snapshot.scan("key/1", "key/3"),
                 reverse = snapshot.scan("key/1", "key/3", true);
            std::vector<std::string> keys;
            while (auto row = forward.next())
                keys.emplace_back(row->key);
            for (auto it = keys.rbegin(); it != keys.rend(); ++it)
                check(reverse.next()->key == *it, "reverse range mismatch");
            check(!reverse.next(), "reverse range ended late");
            auto first = store.transaction(), second = store.transaction();
            first.put("collision", "a");
            second.put("collision", "b");
            committed(first);
            check(second.commit().status == fengqudb::Status::conflict, "write conflict missed");
            auto phantom = store.transaction();
            {
                auto scan = phantom.scan("phantom/", "phantom0");
                check(!scan.next(), "initial phantom range");
            }
            auto insert = store.transaction();
            insert.put("phantom/new", "x");
            committed(insert);
            phantom.put("outside", "value");
            check(phantom.commit().status == fengqudb::Status::conflict, "phantom insert missed");
            auto deleted = store.transaction();
            {
                auto scan = deleted.scan("phantom/", "phantom0");
                check(scan.next().has_value(), "range existing item");
            }
            auto erase = store.transaction();
            erase.erase("phantom/new");
            committed(erase);
            deleted.put("outside", "value");
            check(deleted.commit().status == fengqudb::Status::conflict, "phantom delete missed");
            auto cancellation = store.transaction();
            cancellation.put("cancelled", "x");
            cancellation.cancel();
            check(cancellation.commit().status == fengqudb::Status::cancelled &&
                      !store.get("cancelled"),
                  "cancelled transaction wrote");
            auto timeout = store.transaction(50);
            timeout.put("timeout", "x");
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            check(timeout.commit().status == fengqudb::Status::timeout && !store.get("timeout"),
                  "timed-out transaction wrote");
            auto txn = store.transaction();
            txn.put("patch", "0123456789");
            auto outer = txn.savepoint();
            txn.patch("patch", 2, 5, "X");
            check(txn.get("patch") == "01X789", "subtraction result");
            auto inner = txn.savepoint();
            txn.patch("patch", 0, 2, "prefix");
            txn.release(inner);
            txn.rollback_to(outer);
            check(txn.get("patch") == "0123456789", "nested savepoint rollback");
            txn.patch("patch", 2, 3, "ABC");
            auto frozen = txn.scan("patch", "patch0");
            check(frozen.next()->value == "01ABC56789", "staged cursor value");
            txn.patch("patch", 0, 2, "XY");
            committed(txn);
            check(store.get("patch")->view() == "XYABC56789", "staged cursor isolation");
            // Ended cursor keeps its immutable write root alive until close.
            check(!frozen.next(), "staged cursor end");
            store_test_trace("ttl");
            auto expiry = store.transaction();
            auto now = p::unix_ms();
            StoreTestClock clock(now);
            auto at = now + 1;
            expiry.put("ttl", "old", at);
            committed(expiry);
            auto ttl_snapshot = store.snapshot();
            auto fixed = ttl_snapshot.get("ttl");
            check(fixed && fixed->view() == "old", "TTL captured before expiry");
            clock.set(at + 1);
            auto retained = ttl_snapshot.get("ttl");
            check(!store.get("ttl") && retained && retained->view() == "old",
                  "snapshot TTL moved with wall time");
            clock.reset();
            auto resurrect = store.transaction();
            resurrect.put("ttl", "new");
            committed(resurrect);
            auto metadata = store.transaction();
            metadata.expire("ttl", 1);
            committed(metadata);
            check(!store.get("ttl"), "TTL metadata write");
            auto seed = store.transaction();
            seed.put("counter", "0");
            committed(seed);
            store_test_trace("concurrency/start");
            std::vector<std::future<void>> workers;
            for (unsigned worker = 0; worker < 6; ++worker)
                workers.push_back(std::async(std::launch::async, [&] {
                    for (unsigned i = 0; i < 50; ++i)
                        for (;;) {
                            auto update = store.transaction();
                            auto value = std::stoi(*update.get("counter"));
                            update.put("counter", std::to_string(value + 1));
                            auto result = update.commit();
                            if (result)
                                break;
                            check(result.status == fengqudb::Status::conflict,
                                  "unexpected concurrent commit error");
                        }
                }));
            for (auto &worker : workers)
                worker.get();
            store_test_trace("concurrency/done");
            check(store.get("counter")->view() == "300", "concurrent lost update");
            store.checkpoint();
            auto info = store.info();
            check(info.delta_bytes == 0 && info.durable_version == info.version,
                  "checkpoint did not fold delta");
        }
        {
            Store store(owned.path, options);
            check(store.get("counter")->view() == "300", "checkpoint restart");
            auto tx = store.transaction();
            tx.put("replayed", "from WAL");
            committed(tx);
        }
        {
            Store store(owned.path, options);
            check(store.get("replayed")->view() == "from WAL" &&
                      store.info().recovered_transactions == 1,
                  "WAL restart");
        }
        store_test_trace("ABI");
        // Public buffer reuse and savepoint copy isolation.
        {
            fqdb_store *store = nullptr;
            ok(test_support::open_strict(owned.path.c_str(), nullptr, &store));
            fqdb_store_txn *tx = nullptr;
            ok(fqdb_store_txn_begin(store, 0, &tx));
            ok(fqdb_store_txn_put(tx, slice("private"), slice(std::string(10000, 'a')), 0));
            auto pointer = fqdb_store_test_staged_pointer(tx, slice("private"));
            ok(fqdb_store_txn_patch(tx, slice("private"), 10, 3, slice("XYZ")));
            check(pointer == fqdb_store_test_staged_pointer(tx, slice("private")),
                  "private patch recopied binary");
            uint64_t mark = 0;
            ok(fqdb_store_txn_savepoint(tx, &mark));
            ok(fqdb_store_txn_patch(tx, slice("private"), 10, 3, slice("ABC")));
            check(pointer != fqdb_store_test_staged_pointer(tx, slice("private")),
                  "savepoint value not isolated");
            auto isolated = fqdb_store_test_staged_pointer(tx, slice("private"));
            ok(fqdb_store_txn_patch(tx, slice("private"), 10, 3, slice("DEF")));
            check(isolated == fqdb_store_test_staged_pointer(tx, slice("private")),
                  "private savepoint edit copied twice");
            fqdb_store_txn_close(tx);
            fqdb_store_close(store);
        }
        store_test_trace("semantic-recovery");
        // A correct frame with an invalid expected base version must fail open
        // without changing even one byte in the original generation.
        {
            auto current = p::read_current(owned.path);
            auto path = p::join(owned.path, current.filename());
            auto generation = std::make_shared<p::Generation>(0);
            generation->open(path, current.uuid, current.generation);
            auto checkpoint = generation->checkpoint();
            p::WalBatch bad(checkpoint.version + 1, 1);
            bad.add(p::OpKind::put, "counter", std::make_shared<const p::Data>("bad"));
            bad.append(*generation);
            generation->file->sync();
            auto length = generation->tail;
            std::string before(static_cast<size_t>(length), '\0');
            generation->file->read(0, before.data(), before.size());
            generation.reset();
            bool corrupt = false;
            try {
                Store store(owned.path, options);
            } catch (const fengqudb::Error &e) {
                corrupt = e.code() == FQDB_CORRUPT;
            }
            check(corrupt, "semantic WAL corruption accepted");
            p::SharedFile file;
            file.open(path, false, true);
            std::string after(before.size(), '\0');
            file.read(0, after.data(), after.size());
            check(before == after, "failed recovery modified original bytes");
            file.truncate(checkpoint.end);
        }
#ifndef __SANITIZE_THREAD__
        for (int point = 1; point <= 4; ++point) {
            Owned crash;
            {
                Store empty(crash.path);
            }
            auto executable =
                std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
            check(child(executable, crash.path, point) == 86, "crash hook not reached");
            Store recovered(crash.path);
            auto value = recovered.get("crash");
            check(point == 1 ? !value
                             : (point == 2 ? !value || value->view() == "complete"
                                           : value && value->view() == "complete"),
                  "crash recovery mismatch");
        }
#else
        std::cout << "SKIP crash subprocesses under TSan; covered by Windows and ASan runs\n";
#endif
        store_test_trace("large-recovery");
        // Recover a value log far larger than its page/value cache, forcing
        // bounded delta metadata to spill before any original-file mutation.
        {
            Owned bulk;
            StoreOptions large = options;
            large.delta_bytes = 4 * 1024 * 1024;
            large.durability = fengqudb::Durability::buffered;
            large.cache_bytes = 65536;
            {
                Store store(bulk.path, large);
                std::string payload(8192, 'b');
                for (unsigned i = 0; i < 1000; ++i) {
                    auto tx = store.transaction();
                    tx.put("bulk/" + std::to_string(1000 + i), payload);
                    committed(tx);
                }
                check(store.info().cache_bytes <= large.cache_bytes, "bulk cache budget");
                store.sync(); // Preserve the WAL without creating a checkpoint.
            }
            auto current = p::read_current(bulk.path);
            auto path = p::join(bulk.path, current.filename());
            uint64_t valid_end;
            std::string before;
            {
                auto generation = std::make_shared<p::Generation>(0);
                generation->open(path, current.uuid, current.generation);
                valid_end = generation->tail;
                p::WalBatch invalid(1001, 1);
                invalid.add(p::OpKind::put, "bulk/1000",
                            std::make_shared<const p::Data>("wrong base"));
                invalid.append(*generation);
                generation->file->sync();
                before.resize(static_cast<size_t>(generation->tail));
                generation->file->read(0, before.data(), before.size());
            }
            bool failed = false;
            try {
                Store recovered(bulk.path, options);
            } catch (const fengqudb::Error &e) {
                failed = e.code() == FQDB_CORRUPT;
            }
            check(failed, "invalid WAL after recovery spill accepted");
            {
                p::SharedFile file;
                file.open(path, false, true);
                std::string after(before.size(), '\0');
                file.read(0, after.data(), after.size());
                check(before == after && file.size() == before.size(),
                      "spilling failed recovery changed original");
                file.truncate(valid_end);
            }
            auto small = options;
            small.cache_bytes = 65536;
            {
                Store recovered(bulk.path, small);
                auto info = recovered.info();
                check(info.recovered_transactions == 1000 && info.stored_keys == 1000 &&
                          info.delta_bytes == 0,
                      "spilled recovery counts");
                check(recovered.get("bulk/1000")->view() == std::string(8192, 'b') &&
                          recovered.get("bulk/1999")->view().size() == 8192,
                      "spilled recovery values");
                check(recovered.info().cache_bytes <= small.cache_bytes,
                      "recovery exceeded cache budget");
            }
            {
                Store quick(bulk.path, small);
                check(quick.info().recovered_transactions == 0,
                      "checkpoint restart replayed historical WAL");
            }
        }
        for (unsigned boundary = 0; boundary < 9; ++boundary) {
            Owned tail;
            {
                Store store(tail.path);
                auto tx = store.transaction();
                tx.put("old", "keep");
                committed(tx);
                store.checkpoint();
            }
            auto current = p::read_current(tail.path);
            auto path = p::join(tail.path, current.filename());
            uint64_t cut;
            {
                auto generation = std::make_shared<p::Generation>(0);
                generation->open(path, current.uuid, current.generation);
                auto checkpoint = generation->checkpoint();
                auto start = generation->tail;
                p::WalBatch partial(checkpoint.version + 1, 1);
                partial.add(p::OpKind::put, "partial",
                            std::make_shared<const p::Data>("discard me"));
                partial.append(*generation);
                auto length = generation->tail - start;
                uint64_t cuts[]{1,          15,          31,          32,        33,
                                length / 2, length - 25, length - 24, length - 1};
                cut = cuts[boundary];
                generation->file->truncate(start + cut);
            }
            {
                Store repaired(tail.path);
                check(repaired.info().repaired_bytes == cut && !repaired.get("partial") &&
                          repaired.get("old")->view() == "keep",
                      "incomplete WAL tail repair");
                auto tx = repaired.transaction();
                tx.put("after", "repair");
                committed(tx);
            }
            {
                Store again(tail.path);
                check(!again.get("partial") && again.get("after")->view() == "repair",
                      "partial old WAL reanimated after later append");
            }
        }
        {
            Owned limited;
            auto limits = options;
            limits.transaction_bytes = 1048576;
            limits.all_transaction_bytes = 2097152;
            limits.pinned_bytes = 65536;
            limits.max_snapshots = 3;
            Store store(limited.path, limits);
            auto tx = store.transaction();
            tx.put("safe", std::string(10000, 's'));
            auto mark = tx.savepoint();
            bool exhausted = false;
            try {
                tx.put("too-large", std::string(2000000, 'x'));
            } catch (const fengqudb::Error &e) {
                exhausted = e.code() == FQDB_LIMIT;
            }
            check(exhausted && tx.commit().status == fengqudb::Status::invalid_argument,
                  "budget failure allowed partial commit");
            tx.rollback_to(mark);
            check(tx.get("safe")->size() == 10000, "budget rollback lost staged data");
            committed(tx);
            auto first = store.snapshot(), second = store.snapshot();
            exhausted = false;
            try {
                auto extra = store.snapshot();
            } catch (const fengqudb::Error &e) {
                exhausted = e.code() == FQDB_LIMIT;
            }
            check(exhausted, "snapshot count budget ignored");
            std::vector<fengqudb::StoreValue> pins;
            exhausted = false;
            for (unsigned n = 0; n < 20; ++n) {
                try {
                    pins.push_back(std::move(*store.get("safe")));
                } catch (const fengqudb::Error &e) {
                    exhausted = e.code() == FQDB_LIMIT;
                    break;
                }
            }
            check(exhausted && store.info().pinned_bytes <= limits.pinned_bytes,
                  "external value pins exceeded budget");
        }
        store_test_trace("I/O-faults");
        // Partial large-WAL writes and flush failures poison the writer; reopen
        // must yield the old state or the complete new transaction, never a prefix.
        for (int failure = 1; failure <= 4; ++failure) {
            Owned fault;
            fqdb_store *store = nullptr;
            ok(test_support::open_strict(fault.path.c_str(), nullptr, &store));
            fqdb_store_txn *tx = nullptr;
            ok(fqdb_store_txn_begin(store, 0, &tx));
            auto large = std::string(40000, 'x');
            ok(fqdb_store_txn_put(tx, slice("atomic"), slice(large), 0));
            fqdb_store_test_io_fault(failure <= 3 ? failure : 0, failure == 4 ? 1 : 0);
            uint64_t version = 0;
            check(fqdb_store_txn_commit(tx, &version) == FQDB_COMMIT_UNKNOWN, "write fault status");
            fqdb_store_txn_close(tx);
            ok(fqdb_store_txn_begin(store, 0, &tx));
            ok(fqdb_store_txn_put(tx, slice("later"), slice("no"), 0));
            check(fqdb_store_txn_commit(tx, &version) == FQDB_RECOVERY_REQUIRED,
                  "poisoned writer continued");
            fqdb_store_txn_close(tx);
            fqdb_store_close(store);
            fqdb_store_test_io_fault(0, 0);
            Store recovered(fault.path);
            auto value = recovered.get("atomic");
            check(failure <= 3 ? !value : value && value->view() == large,
                  "partial transaction published");
            check(!recovered.get("later"), "poisoned later transaction was written");
        }
        {
            store_test_trace("logging");
            Owned logs;
            fqdb_store *store = nullptr;
            ok(test_support::open_strict(logs.path.c_str(), nullptr, &store));
            LogProbe probe{store};
            fqdb_log_handle *sink = nullptr;
            fqdb_log_options log_options{sizeof(log_options), 1,     FQDB_LOG_INFO, 0, 0,
                                         log_probe,           &probe};
            ok(fqdb_log_open(&log_options, &sink));
            fqdb_store_txn *tx = nullptr;
            ok(fqdb_store_txn_begin(store, 0, &tx));
            ok(fqdb_store_txn_put(tx, slice("logged"), slice("yes"), 0));
            uint64_t version = 0;
            ok(fqdb_store_txn_commit(tx, &version));
            fqdb_store_txn_close(tx);
            ok(fqdb_store_checkpoint(store));
            ok(fqdb_store_sync(store));
            ok(fqdb_log_close(sink));
            check(probe.calls.load() >= 3 && !probe.bad.load(),
                  "Store logging callback ran under writer lock or lost identity");
            fqdb_store_close(store);
        }
        std::cout << "PASS Store C/C++, randomized operations, OCC/phantoms, savepoints, TTL, "
                     "cancellation, restart and crash/fault recovery\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
