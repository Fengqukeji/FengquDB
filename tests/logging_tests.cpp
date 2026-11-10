#include "strict_store.hpp"
#include "fengqudb_store.hpp"
#include "fengqudb_log.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
extern "C" void fqdb_store_test_io_fault(int write, int sync);
using namespace fengqudb;
using namespace std::chrono_literals;
static void check(bool value) {
    if (!value)
        throw std::runtime_error("logging check failed");
}
struct Sink {
    std::mutex mutex;
    std::vector<fqdb_log_event> events;
    const test_support::StrictStore *db = nullptr;
    fqdb_log_handle *handle = nullptr;
    std::atomic<bool> block{false}, entered{false}, release{false}, throw_now{false};
    bool reentered = false;
    fqdb_status self_close = FQDB_OK;
};
static void FQDB_CALL record(void *user, const fqdb_log_event *event) {
    auto &sink = *static_cast<Sink *>(user);
    if (sink.throw_now)
        throw std::runtime_error("callback fault");
    if (sink.db) {
        (void)sink.db->info();
        sink.reentered = true;
        sink.self_close = fqdb_log_close(sink.handle);
    }
    if (sink.block) {
        sink.entered = true;
        while (!sink.release)
            std::this_thread::yield();
    }
    std::lock_guard<std::mutex> lock(sink.mutex);
    sink.events.push_back(*event);
}
template <class F> static void until(F predicate) {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("logging timeout");
        std::this_thread::yield();
    }
}
int main() {
    try {
        auto root = std::filesystem::temp_directory_path() /
                    ("fqdb-log-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        Sink sink;
        fqdb_log_options options{sizeof(options), 1, FQDB_LOG_INFO, 0, 0, record, &sink};
        check(fqdb_log_open(&options, &sink.handle) == FQDB_OK);
        fqdb_log_handle *duplicate = nullptr;
        check(fqdb_log_open(&options, &duplicate) == FQDB_LOCKED && !duplicate);
        {
        test_support::StrictStore db((root / "db.fdb").u8string());
        sink.db = &db;
        db.put_if_changed("private-key", "private-value");
        check(sink.reentered && sink.self_close == FQDB_INVALID_ARGUMENT);
        auto a = db.transaction(), b = db.transaction();
        a.put("same", "a");
        b.put("same", "b");
        a.commit();
        check(b.commit().status == Status::conflict);
        auto failing = db.transaction();
        failing.put("unknown", "test");
        fqdb_store_test_io_fault(0, 1);
        check(failing.commit().status == Status::commit_unknown);
        fqdb_store_test_io_fault(0, 0);
        bool conflict = false, uncertain = false;
        for (const auto &event : sink.events) {
            check(event.struct_size == sizeof(event) && event.database_id &&
                  event.abi_version == 1);
            conflict |= event.status == FQDB_CONFLICT;
            uncertain |= event.status == FQDB_COMMIT_UNKNOWN && event.stage == FQDB_STAGE_FLUSH;
        }
        check(conflict && uncertain);
        sink.db = nullptr;
        }
        test_support::StrictStore fresh((root / "fresh.fdb").u8string());
        sink.throw_now = true;
        check(fresh.put_if_changed("ok", "despite-callback-error").version == 1);
        sink.throw_now = false;
        fqdb_log_info info{};
        check(fqdb_log_get_info(sink.handle, &info) == FQDB_OK && info.callback_errors > 0);
        sink.block = true;
        auto writer =
            std::async(std::launch::async, [&] { return fresh.put_if_changed("concurrent", "commit").version; });
        until([&] { return sink.entered.load(); });
        std::atomic<bool> closing{false};
        auto closer = std::async(std::launch::async, [&] {
            closing = true;
            return fqdb_log_close(sink.handle);
        });
        until([&] { return closing.load(); });
        bool waited = closer.wait_for(20ms) == std::future_status::timeout;
        auto reader =
            std::async(std::launch::async, [&] { return fresh.get("concurrent")->string(); });
        bool unlocked = reader.wait_for(1s) == std::future_status::ready;
        sink.release = true;
        check(writer.get() == 2 && closer.get() == FQDB_OK && reader.get() == "commit");
        sink.handle = nullptr;
        check(waited && unlocked);
        size_t delivered = sink.events.size();
        fresh.put_if_changed("later", "no-callback");
        check(sink.events.size() == delivered);
        std::cout << "PASS logging: metadata, reentrant reads, callback errors, conflict/flush "
                     "errors, quiescent close\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
