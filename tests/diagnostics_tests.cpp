#include "strict_store.hpp"
#include "fengqudb_diagnostics.hpp"
#include "fengqudb_store.hpp"
#include <json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace fengqudb;
using Json = nlohmann::json;
namespace fs = std::filesystem;
static void check_at(bool value, int line) {
    if (!value)
        throw std::runtime_error("diagnostics check failed at line " + std::to_string(line));
}
#define check(value) check_at((value), __LINE__)
static Json parse(Buffer value) {
    return Json::parse(value.view());
}
int main() {
    try {
        auto root = fs::temp_directory_path() /
                    ("fqdb-diagnostics-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        auto logs = root / "logs", file = root / "PRIVATE-FILENAME.fdb";
        DiagnosticsOptions options;
        options.min_level = FQDB_LOG_INFO;
        options.slow_commit_us = 0;
        Diagnostics log(logs.u8string(), options);
        StoreOptions db_options;
        db_options.durability = Durability::buffered;
        {
        test_support::StrictStore db(file.u8string(), db_options);
        db.put_if_changed("PRIVATE-KEY-1234", "PRIVATE-VALUE-4321");
        {
        auto a = db.transaction(), b = db.transaction();
        a.put("same", "a");
        b.put("same", "b");
        a.commit();
        check(b.commit().status == Status::conflict);
        }
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                try {
                    for (int i = 0; i < 200; ++i)
                        db.put_if_changed("thread/" + std::to_string(t) + "/" + std::to_string(i), "data");
                } catch (...) {
                    failed = true;
                }
            });
        for (auto &thread : threads)
            thread.join();
        check(!failed);
        db.sync();
        check(log.flush(10000));
        for (const auto &entry : fs::directory_iterator(logs))
            check(entry.is_regular_file() && entry.path().filename().u8string().size() <= 53);
        auto status = parse(log.status());
        check(status["high_water_queued"] <= 4096 && status["high_water_bytes"] <= 2097152 &&
              status["callback_errors"] == 0);
        check(status["submitted"].get<uint64_t>() == status["written"].get<uint64_t>() +
                                                         status["dropped"].get<uint64_t>() +
                                                         status["cleared"].get<uint64_t>());
        auto output = log.query(R"({"limit":3000})").str();
        check(output.find("PRIVATE-KEY-1234") == std::string::npos &&
              output.find("PRIVATE-VALUE-4321") == std::string::npos &&
              output.find("PRIVATE-FILENAME") == std::string::npos);
        }
        fs::path data_file;
        for (const auto &entry : fs::directory_iterator(file))
            if (entry.path().extension() == ".fdb") data_file = entry.path();
        check(!data_file.empty());
        {
            std::ofstream append(data_file, std::ios::binary | std::ios::app);
            append.write("bad", 3);
        }
        test_support::StrictStore repaired(file.u8string());
        check(repaired.get("PRIVATE-KEY-1234")->string() == "PRIVATE-VALUE-4321");
        try {
            StoreOptions missing;
            missing.create = false;
            test_support::StrictStore invalid((root / "absent" / "db.fdb").u8string(), missing);
            check(false);
        } catch (const Error &e) {
            check(e.status() == Status::not_found);
        }
        check(log.flush(10000));
        auto repairs = parse(log.query(R"({"event":"recovery.truncated_tail"})"));
        check(!repairs["items"].empty() &&
              repairs["items"].back()["fields"]["repaired_bytes"] == "3");
        auto errors = parse(log.query(R"({"event":"open.failed"})"));
        check(!errors["items"].empty());
        try {
            log.query("[");
            check(false);
        } catch (const Error &e) {
            check(e.status() == Status::invalid_argument);
        }
        {
            std::ofstream foreign(logs / "foreign--1-2-3-4.jsonl");
            foreign << "foreign";
            std::ofstream data(logs / "business.fdb");
            data << "business";
            std::ofstream old_modern(logs / "fengqudb--100-2-3-4.jsonl");
            old_modern << "legacy modern";
            std::ofstream old_legacy(logs / "fengqudb-100-2-3-4.jsonl");
            old_legacy << "legacy";
        }
        log.configure(R"({"enabled":true,"retention_days":1,"max_bytes":16777216})");
        log.clear();
        check(fs::exists(logs / "foreign--1-2-3-4.jsonl") && fs::exists(logs / "business.fdb"));
        check(!fs::exists(logs / "fengqudb--100-2-3-4.jsonl") &&
              !fs::exists(logs / "fengqudb-100-2-3-4.jsonl"));
        check(repaired.get("PRIVATE-KEY-1234")->string() == "PRIVATE-VALUE-4321");
        log.close();
        DiagnosticsOptions reader_options;
        reader_options.reader_only = true;
        Diagnostics reader(logs.u8string(), reader_options);
        check(parse(reader.status())["reader_only"] == true);
        try {
            reader.clear();
            check(false);
        } catch (const Error &e) {
            check(e.status() == Status::unsupported);
        }
        reader.close();
        fqdb_diagnostics *too_long = nullptr;
        std::string long_directory = (root / std::string(1000, 'x')).u8string();
        check(fqdb_diagnostics_open(long_directory.c_str(), nullptr, &too_long) == FQDB_LIMIT &&
              !too_long);
        auto absent_parent = root / "absent-log-parent";
        auto reject_path = [&](const fs::path &path) {
            fqdb_diagnostics *handle = nullptr;
            auto text = path.u8string();
            auto result = fqdb_diagnostics_open(text.c_str(), nullptr, &handle);
            bool opened = handle != nullptr;
            if (handle)
                fqdb_diagnostics_close(handle);
            if (result != FQDB_LIMIT || opened || fs::exists(absent_parent))
                throw std::runtime_error("diagnostic path was not rejected before directory creation");
        };
        reject_path(absent_parent / std::string(256, 'x'));
#ifndef _WIN32
        // POSIX NAME_MAX counts bytes: 86 CJK characters require 258 bytes.
        std::string unicode_component;
        for (unsigned i = 0; i < 86; ++i)
            unicode_component += u8"\u5b58";
        reject_path(absent_parent / fs::u8path(unicode_component));
#endif
        {
            auto unicode_logs = root / fs::u8path(u8"\u65e5\u5fd7");
            Diagnostics unicode(unicode_logs.u8string(), options);
            repaired.put_if_changed("unicode-log-path", "saved");
            check(unicode.flush(10000));
            auto info = parse(unicode.status());
            check(info["io_errors"] == 0 && info["written"].get<uint64_t>() > 0);
            for (const auto &entry : fs::directory_iterator(unicode_logs))
                check(entry.is_regular_file() && entry.path().filename().u8string().size() <= 53);
        }
        auto blocked = root / "not-a-directory";
        {
            std::ofstream output(blocked);
            output << "file";
        }
        {
            Diagnostics failing(blocked.u8string(), options);
            repaired.put_if_changed("log-io-failure", "database-still-writable");
            check(failing.flush(10000));
            check(parse(failing.status())["io_errors"] > 0);
        }
        std::cout << "PASS PTpma diagnostics: async accounting, privacy, recovery/I/O events, "
                     "clear isolation, reader, disk failure; "
                  << root << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
