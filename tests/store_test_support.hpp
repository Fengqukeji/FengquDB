#pragma once
#include "store_trace.hpp"
#include "fengqudb_store.hpp"
#include "strict_store.hpp"
#include "thread_future.hpp"
#include "paged/maintenance.hpp"
#include <future>
#include <iostream>
#ifndef _WIN32
#include <sys/wait.h>
#endif
namespace p = fengqudb::paged;
using Store = test_support::StrictStore;
extern "C" void fqdb_store_test_maintenance_hold(int);
extern "C" bool fqdb_store_test_maintenance_entered();
extern "C" void fqdb_store_test_crash(int);
extern "C" void fqdb_store_test_io_fault(int, int);
extern "C" void fqdb_store_test_publication_fault(bool, bool);
extern "C" void fqdb_store_test_open_fault(bool);
inline void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
inline void commit(fengqudb::StoreTransaction &tx) {
    check(bool(tx.commit()), "commit failed");
}
inline void put(Store &store, std::string_view key, std::string_view value) {
    auto tx = store.transaction();
    tx.put(key, value);
    commit(tx);
}
template <class F> inline fqdb_status status(F &&function) {
    try {
        function();
        return FQDB_OK;
    } catch (const fengqudb::Error &error) {
        return error.code();
    }
}
inline void reached() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!fqdb_store_test_maintenance_entered()) {
        check(std::chrono::steady_clock::now() < deadline, "maintenance barrier not reached");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Held {
    explicit Held(int point) {
        fqdb_store_test_maintenance_hold(point);
    }
    ~Held() {
        release();
    }
    void release() {
        fqdb_store_test_maintenance_hold(0);
    }
};
struct Fixture {
    std::string root;
    std::vector<std::string> directories;
    Fixture() {
        store_test_trace("fixture/start");
        root =
            (std::filesystem::temp_directory_path() / ("fqdb-maintenance-" + p::hex(p::new_uuid())))
                .u8string();
        check(std::filesystem::create_directory(std::filesystem::u8path(root)),
              "owned fixture root");
    }
    std::string name(const std::string &name) {
        auto path = p::join(root, name);
        directories.push_back(path);
        return path;
    }
    inline void clean(const std::string &path) noexcept {
        try {
            auto native = std::filesystem::u8path(path);
            if (std::filesystem::is_symlink(native))
                return;
            if (!std::filesystem::exists(native))
                return;
            std::vector<std::filesystem::path> files;
            for (const auto &entry : std::filesystem::directory_iterator(native)) {
                auto name = entry.path().filename().u8string();
                if (!entry.is_regular_file() ||
                    !(name == "LOCK" || name == "CURRENT" || name == "WRITE" || p::generation_filename(name) ||
                      (entry.path().extension() == ".blk" &&
                       p::generation_filename(name.substr(0, name.size() - 4) + ".fdb"))))
                    return;
                files.push_back(entry.path());
            }
            for (const auto &file : files)
                std::filesystem::remove(file);
            std::filesystem::remove(native);
        } catch (...) {
        }
    }
    ~Fixture() {
        for (const auto &path : directories)
            clean(path);
        // Crash-only private backup directories belong to this exclusive
        // fixture and are still cleaned one known file at a time.
        try {
            for (const auto &entry :
                 std::filesystem::directory_iterator(std::filesystem::u8path(root))) {
                auto name = entry.path().filename().u8string();
                if (entry.is_symlink() || !entry.is_directory())
                    continue;
                if (name.size() == 21 && name[0] == 't' && name.substr(17) == ".tmp") {
                    bool canonical = true;
                    for (size_t i = 1; i < 17; ++i)
                        if (!((name[i] >= '0' && name[i] <= '9') ||
                              (name[i] >= 'a' && name[i] <= 'f')))
                            canonical = false;
                    if (canonical)
                        clean(entry.path().u8string());
                    continue;
                }
                for (const auto &directory : directories) {
                    auto prefix =
                        std::filesystem::u8path(directory).filename().u8string() + ".tmp-";
                    if (name.size() != prefix.size() + 32 || name.compare(0, prefix.size(), prefix))
                        continue;
                    bool canonical = true;
                    for (size_t i = prefix.size(); i < name.size(); ++i)
                        if (!((name[i] >= '0' && name[i] <= '9') ||
                              (name[i] >= 'a' && name[i] <= 'f')))
                            canonical = false;
                    if (canonical)
                        clean(entry.path().u8string());
                }
            }
            std::filesystem::remove(std::filesystem::u8path(root));
        } catch (...) {
        }
    }
};
inline int child(const std::string &executable, const std::string &mode, const std::string &source,
                 const std::string &destination, int point) {
#ifdef _WIN32
    auto command =
        fengqudb::storage::native_path(("\"" + executable + "\" " + mode + " \"" + source +
                                        "\" \"" + destination + "\" " + std::to_string(point))
                                           .c_str());
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &start, &process) != 0,
          "crash child create");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    check(GetExitCodeProcess(process.hProcess, &code) != 0, "crash child status");
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    auto pid = fork();
    check(pid >= 0, "fork maintenance child");
    if (!pid) {
        auto number = std::to_string(point);
        execl(executable.c_str(), executable.c_str(), mode.c_str(), source.c_str(),
              destination.c_str(), number.c_str(), static_cast<char *>(nullptr));
        std::_Exit(87);
    }
    int result = 0;
    check(waitpid(pid, &result, 0) == pid && WIFEXITED(result), "wait maintenance child");
    return WEXITSTATUS(result);
#endif
}
