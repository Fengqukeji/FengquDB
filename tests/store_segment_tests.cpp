#include "strict_store.hpp"
#include "store_test_support.hpp"
#include "paged/reclaim.hpp"
#include <map>
#include <fstream>

namespace fs = std::filesystem;
using Model = std::map<std::string, std::string>;
static auto settings(bool segmented = true) {
    auto options = p::defaults();
    options.flags = FQDB_STORE_CREATE | (segmented ? FQDB_STORE_SEGMENTED : 0);
    options.cache_bytes = 0;
    options.delta_bytes = 65536;
    options.durability = FQDB_DURABILITY_BUFFERED;
    return options;
}
static auto open_core(const std::string &path, bool segmented = true) {
    auto core = std::make_shared<p::Core>(path, settings(segmented));
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    return core;
}
static void verify(const std::shared_ptr<p::Core> &core, const Model &model) {
    auto view = core->current();
    for (const auto &item : model) {
        auto ref = view->find(item.first);
        check(ref && *view->generation->value(*ref, view->limit, false, false) == item.second,
              "segmented Store cold value mismatch");
    }
    core->verify(p::maintenance_defaults());
}
static unsigned data_files(const std::string &path) {
    unsigned count = 0;
    for (const auto &entry : fs::directory_iterator(fs::u8path(path)))
        if (entry.path().extension() == ".fdb") {
            ++count;
            check(entry.file_size() <= 131072, "native segment exceeded test capacity");
            check(entry.path().filename().u8string().size() <= 30, "segment filename too long");
        }
    return count;
}
static void seed(const std::shared_ptr<p::Core> &core, Model &model, unsigned rounds = 10) {
    for (unsigned round = 0; round < rounds; ++round) {
        p::Transaction tx(core, 0);
        for (unsigned key = 0; key < 20; ++key) {
            auto name = "key/" + std::to_string(key);
            auto value = std::string(32769, static_cast<char>('a' + round));
            tx.put(name, value, 0);
            model[name] = std::move(value);
        }
        core->commit(tx);
    }
}
static void real_reclaim() {
    Fixture fixture;
    const auto path = fixture.name("segments");
    Model model;
    unsigned before = 0, after = 0;
    {
        auto core = open_core(path);
        seed(core, model, 1);
        auto snapshot = std::make_shared<p::Capture>(core);
        seed(core, model);
        {
            p::Transaction tx(core, 0);
            tx.put("patch", std::string(524288, 'p'), 0);
            core->commit(tx);
            model["patch"] = std::string(524288, 'p');
        }
        {
            p::Transaction tx(core, 0);
            tx.patch("patch", 17, 490000, "cut");
            core->commit(tx);
            model["patch"].replace(17, 490000, "cut");
        }
        core->checkpoint();
        before = data_files(path);
        {
            p::ReclaimSession session(*core);
            auto policy = p::maintenance_defaults();
            check(!session.step(policy), "segmented reclaim start");
            const auto protected_files = data_files(path);
            check(!session.step(policy) && session.waiting_for_readers() && !session.punched,
                  "segmented reclaim ignored an old reader");
            check(data_files(path) == protected_files, "old reader lost physical segments");
            auto ref = snapshot->view->find("key/0");
            check(ref && *snapshot->view->generation->value(*ref, snapshot->view->limit, false,
                                                            false) == std::string(32769, 'a'),
                  "old segmented snapshot changed");
            snapshot.reset();
            policy.max_write_bytes = 262144;
            unsigned slices = 0;
            for (;;) {
                try {
                    if (session.step(policy))
                        break;
                } catch (const p::IOBudgetStop &) {
                }
                check(++slices < 256, "segmented reclaim did not resume");
            }
            after = data_files(path);
            check(after + 16 < before, "reclaim did not remove whole segments");
        }
        verify(core, model);
        check(core->current()->generation->file->segment_bytes() == 65536,
              "segment geometry not active");
        p::SharedFile inspector;
        inspector.open(core->current()->generation->path, false, true, true);
        char magic[8];
        inspector.read(0, magic, 8);
        check(!std::memcmp(magic, "FQDATA04", 8), "logical file facade bypassed");
    }
    {
        // A caller does not need the creation flag to read an existing container.
        auto core = open_core(path, false);
        verify(core, model);
        core->compact(p::maintenance_defaults());
        verify(core, model);
        check(core->current()->generation->file->segment_bytes() == 65536,
              "compaction lost geometry");
        const auto backup = fixture.name("backup");
        core->backup(backup, p::maintenance_defaults());
        auto copy = open_core(backup, false);
        verify(copy, model);
    }
    std::cout << "PASS Store transaction/patch/checkpoint, old readers, sliced reclaim " << before
              << " -> " << after << " segments, reopen, compaction and full backup\n";
}
static void public_api() {
    Fixture fixture;
    const auto path = fixture.name("public");
    fengqudb::StoreOptions options;
    options.segmented = true;
    options.durability = fengqudb::Durability::sync;
    options.cache_bytes = 0;
    {
        Store db(path, options);
        auto tx = db.transaction();
        for (unsigned i = 0; i < 16; ++i)
            tx.put("row/" + std::to_string(i), std::string(10000, 'v'));
        commit(tx);
        db.checkpoint();
        db.verify();
        check(db.get("row/7")->view() == std::string(10000, 'v'), "C++ segmented SDK read");
    }
    {
        fqdb_store *db = nullptr;
        auto native_options = settings(false);
        check(test_support::open_strict(path.c_str(), &native_options, &db) == FQDB_OK,
              "C ABI reopen of segmented data");
        fqdb_buffer bytes{};
        fqdb_slice key{"row/7", 5};
        check(fqdb_store_get(db, key, &bytes) == FQDB_OK && bytes.size == 10000,
              "C ABI segmented read");
        fqdb_buffer_free(&bytes);
        fqdb_store_close(db);
    }
    const auto legacy = fixture.name("legacy");
    {
        auto core = open_core(legacy, false);
        Model model;
        seed(core, model, 1);
        core->checkpoint();
    }
    {
        auto core = open_core(legacy, false);
        check(!core->current()->generation->file->segment_bytes(), "legacy format changed");
    }
    std::cout << "PASS C/C++ creation and auto-detection, SYNC and legacy format\n";
}
static void quota_and_fault() {
    Fixture fixture;
    auto core = open_core(fixture.name("quota"));
    Model model;
    seed(core, model, 1);
    core->checkpoint();
    const auto original = core->current()->generation->tail;
    fengqudb::storage::IOQuota quota{};
    quota.max_write = 200000;
    fengqudb::storage::io_quota = &quota;
    fqdb_status failed = FQDB_OK;
    try {
        p::Transaction tx(core, 0);
        tx.put("denied", std::string(300000, 'n'), 0);
        core->commit(tx);
    } catch (const p::Error &error) {
        failed = error.status;
    }
    fengqudb::storage::io_quota = nullptr;
    check(failed == FQDB_LIMIT && core->current()->generation->tail == original && !core->poisoned,
          "whole segmented transaction budget changed the acknowledged prefix");
    verify(core, model);
    p::paged_fail_write.store(2);
    try {
        p::Transaction tx(core, 0);
        tx.put("torn", std::string(300000, 't'), 0);
        core->commit(tx);
    } catch (const p::Error &) {
    }
    p::paged_fail_write.store(0);
    check(core->poisoned, "partial segmented WAL did not stop writes");
    const auto path = core->directory;
    core.reset();
    core = open_core(path, false);
    verify(core, model);
    p::Transaction tx(core, 0);
    tx.put("continued", "yes", 0);
    core->commit(tx);
    core->sync();
    std::cout << "PASS physical header quota preflight, torn cross-segment WAL recovery and "
                 "continued write\n";
}
static void concurrent_reclaim() {
    Fixture fixture;
    auto core = open_core(fixture.name("concurrent"));
    Model unused;
    seed(core, unused, 3);
    std::atomic<unsigned> start{0};
    std::vector<std::future<void>> writers;
    for (unsigned worker = 0; worker < 4; ++worker)
        writers.push_back(std::async(std::launch::async, [&, worker] {
            start.fetch_add(1);
            while (start.load() < 4)
                std::this_thread::yield();
            for (unsigned round = 0; round < 800; ++round) {
                std::string value(1024, static_cast<char>('A' + worker));
                fengqudb::storage::put64(reinterpret_cast<unsigned char *>(value.data()), round);
                p::Transaction tx(core, 0);
                tx.put("writer/" + std::to_string(worker), value, 0);
                core->commit(tx);
            }
        }));
    unsigned completions = 0;
    for (unsigned pass = 0; pass < 8; ++pass) {
        p::ReclaimSession session(*core);
        auto policy = p::maintenance_defaults();
        policy.max_write_bytes = 262144;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            try {
                if (session.step(policy)) {
                    ++completions;
                    break;
                }
            } catch (const p::IOBudgetStop &) {
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "concurrent reclaim stalled\n";
                std::_Exit(2);
            }
        }
    }
    for (auto &writer : writers) {
        if (writer.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
            std::_Exit(2);
        writer.get();
    }
    Model expected;
    for (unsigned worker = 0; worker < 4; ++worker) {
        std::string value(1024, static_cast<char>('A' + worker));
        fengqudb::storage::put64(reinterpret_cast<unsigned char *>(value.data()), 799);
        expected["writer/" + std::to_string(worker)] = std::move(value);
    }
    verify(core, expected);
    core->sync();
    const auto path = core->directory;
    core.reset();
    core = open_core(path, false);
    verify(core, expected);
    std::cout << "PASS four writers with " << completions
              << " reclaim rounds, cold reads and reopen\n";
}
static void backup_quota_cleanup() {
    Fixture fixture;
    auto core = open_core(fixture.name("source"));
    Model model;
    seed(core, model, 2);
    core->checkpoint();
    auto stages = [&] {
        unsigned count = 0;
        for (const auto &entry : fs::directory_iterator(fs::u8path(fixture.root)))
            if (entry.is_directory() && entry.path().filename().u8string().size() == 21 &&
                entry.path().filename().u8string()[0] == 't')
                ++count;
        return count;
    };
    auto policy = p::maintenance_defaults();
    fqdb_store_maintenance_io observed{};
    policy.max_write_bytes = 600000;
    policy.io_stats = &observed;
    const auto before = stages();
    auto attempt = [&](const std::string &destination) {
        fqdb_status result = FQDB_OK;
        try {
            core->backup(destination, policy);
        } catch (const p::Error &error) {
            result = error.status;
        }
        check(result == FQDB_LIMIT && !fs::exists(fs::u8path(destination)),
              "quota-limited backup unexpectedly published");
        check(stages() == before, "segmented quota failure leaked staging directory");
    };
    attempt(fixture.name("write-quota"));
    check(observed.read_bytes > 400000, "cleanup regression did not cross enough segments");
    policy.max_read_bytes = observed.read_bytes;
    attempt(fixture.name("both-quotas"));
    check(observed.read_bytes <= policy.max_read_bytes, "cleanup exceeded read quota");
    verify(core, model);
    std::cout << "PASS segmented backup cleanup with exhausted write and read quotas\n";
}

#ifndef __SANITIZE_THREAD__
static int child_work(const std::string &kind, const std::string &path,
                      const std::string &argument) {
    auto options = settings(false);
    options.durability = FQDB_DURABILITY_SYNC;
    auto core = std::make_shared<p::Core>(path, options);
    core->open();
    core->write_policy.mode = false; // Strict-path fixture.
    if (kind == "--crash") {
        const auto point = std::stoi(argument);
        fqdb_store_test_crash(point);
        if (point == 3 || point == 4) {
            p::Transaction tx(core, 0);
            tx.put("crash-commit", "durable", 0);
            core->commit(tx);
        } else {
            p::ReclaimSession session(*core);
            auto policy = p::maintenance_defaults();
            while (!session.step(policy)) {
            }
        }
        return 3;
    }
    for (unsigned i = 0; i < 8; ++i) {
        p::Transaction tx(core, 0);
        tx.put("external-kill", std::string(180000, static_cast<char>('a' + i)), 0);
        core->commit(tx);
    }
    core->sync();
    std::array<unsigned char, 16> ack{};
    std::memcpy(ack.data(), "ACK1", 4);
    fengqudb::storage::put64(ack.data() + 4, core->current()->version);
    fengqudb::storage::put32(ack.data() + 12, fengqudb::storage::crc32(ack.data(), 12));
    p::SharedFile marker;
    marker.open(argument, true);
    marker.write(0, ack.data(), ack.size());
    marker.sync();
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
struct Child {
#ifdef _WIN32
    HANDLE process = nullptr;
#else
    pid_t process = -1;
#endif
    Child(const std::string &exe, const std::string &mode, const std::string &path,
          const std::string &arg) {
#ifdef _WIN32
        auto executable = fs::u8path(exe).wstring();
        auto command = L"\"" + executable + L"\" " + fs::u8path(mode).wstring() + L" \"" +
                       fs::u8path(path).wstring() + L"\" \"" + fs::u8path(arg).wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != 0,
              "spawn segmented Store child");
        process = info.hProcess;
        CloseHandle(info.hThread);
#else
        process = fork();
        check(process >= 0, "fork segmented Store child");
        if (!process) {
            execl(exe.c_str(), exe.c_str(), mode.c_str(), path.c_str(), arg.c_str(), nullptr);
            _exit(127);
        }
#endif
    }
    ~Child() {
#ifdef _WIN32
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
                TerminateProcess(process, 87);
            CloseHandle(process);
        }
#else
        if (process > 0) {
            kill(process, SIGKILL);
            int ignored = 0;
            waitpid(process, &ignored, 0);
        }
#endif
    }
    void exited(unsigned expected) {
#ifdef _WIN32
        check(WaitForSingleObject(process, 30000) == WAIT_OBJECT_0, "crash child timed out");
        DWORD code = 0;
        check(GetExitCodeProcess(process, &code) != 0 && code == expected,
              "unexpected crash child status");
        CloseHandle(process);
        process = nullptr;
#else
        int status = 0;
        check(waitpid(process, &status, 0) == process && WIFEXITED(status) &&
                  static_cast<unsigned>(WEXITSTATUS(status)) == expected,
              "unexpected crash child status");
        process = -1;
#endif
    }
    void kill_now() {
#ifdef _WIN32
        check(WaitForSingleObject(process, 0) == WAIT_TIMEOUT && TerminateProcess(process, 86),
              "external termination failed");
        exited(86);
#else
        check(kill(process, SIGKILL) == 0, "external SIGKILL failed");
        int status = 0;
        check(waitpid(process, &status, 0) == process && WIFSIGNALED(status) &&
                  WTERMSIG(status) == SIGKILL,
              "SIGKILL was not observed");
        process = -1;
#endif
    }
};
static void crashes(const std::string &exe) {
    for (int point : {3, 4, 74, 75, 71, 72}) {
        Fixture fixture;
        const auto path = fixture.name("crash");
        Model model;
        {
            auto core = open_core(path);
            seed(core, model, 3);
            core->checkpoint();
        }
        {
            Child child(exe, "--crash", path, std::to_string(point));
            child.exited(86);
        }
        auto core = open_core(path, false);
        verify(core, model);
        if (point == 3 || point == 4)
            check(core->current()->find("crash-commit").has_value(), "flushed crash commit lost");
        p::Transaction tx(core, 0);
        tx.put("continued", "yes", 0);
        core->commit(tx);
        core->sync();
    }
    Fixture fixture;
    const auto path = fixture.name("kill"), marker = fixture.name("ACK");
    Model model;
    {
        auto core = open_core(path);
        seed(core, model, 2);
        core->checkpoint();
    }
    uint64_t acknowledged = 0;
    {
        Child child(exe, "--kill", path, marker);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            std::ifstream input(fs::u8path(marker), std::ios::binary);
            std::array<unsigned char, 16> data{};
            if (input.read(reinterpret_cast<char *>(data.data()), data.size()) &&
                !std::memcmp(data.data(), "ACK1", 4) &&
                fengqudb::storage::crc32(data.data(), 12) ==
                    fengqudb::storage::get32(data.data() + 12)) {
                acknowledged = fengqudb::storage::get64(data.data() + 4);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(acknowledged != 0, "child did not acknowledge commits");
        child.kill_now();
    }
    auto core = open_core(path, false);
    verify(core, model);
    check(core->current()->version == acknowledged,
          "externally killed acknowledged version changed");
    auto ref = core->current()->find("external-kill");
    check(ref && *core->current()->generation->value(*ref, core->current()->limit, false, false) ==
                     std::string(180000, 'h'),
          "external kill lost last confirmed value");
    std::cout << "PASS six transaction/reclaim crash boundaries and external kill after "
                 "acknowledged SYNC commits\n";
}
#endif
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        p::test_segment_geometry.store(65536);
#ifndef __SANITIZE_THREAD__
        if (argc == 4)
            return child_work(argv[1], argv[2], argv[3]);
#else
        (void)argc;
        (void)argv;
#endif
        real_reclaim();
        public_api();
        quota_and_fault();
        concurrent_reclaim();
        backup_quota_cleanup();
#ifndef __SANITIZE_THREAD__
        crashes(fs::absolute(fs::u8path(argv[0])).u8string());
#endif
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
