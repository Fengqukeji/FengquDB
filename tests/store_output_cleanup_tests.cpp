#include "store_test_support.hpp"
#include <fstream>
namespace fs = std::filesystem;

static size_t temporary_outputs(const std::string &root) {
    size_t count = 0;
    for (const auto &entry : fs::directory_iterator(fs::u8path(root)))
        if (entry.path().extension() == ".tmp") ++count;
    return count;
}
static void marker(const p::OutputIntent &intent) {
    fs::create_directory(fs::u8path(intent.temporary));
    const auto bytes = p::output_identity(intent, "FQOWN001");
    p::NativeFile file;
    file.open(p::join(intent.temporary, "OWNER"), true);
    file.write(0, bytes.data(), bytes.size()); file.sync();
}
static void repeated_outputs() {
    Fixture fixture;
    const auto source = fixture.name("source");
    Store db(source);
    put(db, "stable", "preserved");
    for (unsigned i = 0; i < 12; ++i) {
        db.backup(fixture.name("backup-" + std::to_string(i)));
        check(!fs::exists(fs::u8path(source) / "OUTPUT"), "successful backup leaked intent");
        check(temporary_outputs(fixture.root) == 0, "successful backup leaked staging directory");
    }
    for (uint64_t quota : {UINT64_C(64), UINT64_C(2176), UINT64_C(2240)}) {
        fengqudb::StoreMaintenanceOptions options;
        options.max_write_bytes = quota;
        const auto result = status([&] { db.backup(fixture.name("quota-" + std::to_string(quota)), options); });
        check(result == FQDB_LIMIT, "small maintenance quota unexpectedly succeeded");
        check(!fs::exists(fs::u8path(source) / "OUTPUT") && temporary_outputs(fixture.root) == 0,
              "quota-exhausted backup leaked its owned output");
        check(db.get("stable")->view() == "preserved", "failed output changed source");
    }
}
static void ownership_guards() {
    Fixture fixture;
    const auto source = fixture.name("source");
    Store db(source);
    put(db, "stable", "preserved");
    const auto identity = p::read_current(source).uuid;
    auto intent = p::register_output(source, identity, fixture.name("destination"));
    marker(intent);
    const auto directory = fs::u8path(intent.temporary);
    {
        std::ofstream(directory / "user.txt") << "do not delete";
        std::ofstream(directory / "g0000000000000001.fdb") << "also preserve before validation";
    }
    check(!p::recover_output(source, identity), "unknown files were accepted for deletion");
    check(fs::exists(directory / "user.txt") && fs::exists(directory / "g0000000000000001.fdb"),
          "cleanup partially removed a foreign-containing output");
    fs::remove(directory / "user.txt");
    auto foreign = intent; foreign.token[0] ^= 1;
    {
        const auto bytes = p::output_identity(foreign, "FQOWN001");
        std::ofstream file(directory / "OWNER", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    check(!p::recover_output(source, identity) && fs::exists(directory / "g0000000000000001.fdb"),
          "foreign ownership marker was ignored");
    {
        const auto bytes = p::output_identity(intent, "FQOWN001");
        std::ofstream file(directory / "OWNER", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    check(p::recover_output(source, identity) && !fs::exists(directory), "owned output was not reclaimed");
    check(!fs::exists(fs::u8path(source) / "OUTPUT"), "completed cleanup leaked intent");
    intent = p::register_output(source, identity, fixture.name("partial-owner"));
    fs::create_directory(fs::u8path(intent.temporary));
    {
        const auto bytes = p::output_identity(intent, "FQOWN001");
        std::ofstream file(fs::u8path(intent.temporary)/"OWNER", std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), 8);
    }
    check(p::recover_output(source, identity) && !fs::exists(fs::u8path(intent.temporary)),
          "termination inside ownership marker write leaked the private output");
}
static void store_temporaries() {
    Fixture fixture;
    const auto source = fixture.name("source");
    p::Current current;
    std::vector<fs::path> owned;
    {
        Store db(source); put(db, "stable", "preserved");
        current = p::read_current(source);
        auto base = fs::u8path(source);
        owned.push_back(base / "w0123456789abcdef.tmp");
        fs::copy_file(base / "WRITE", owned.back());
        owned.push_back(base / "m0123456789abcdef.tmp");
        fs::copy_file(base / "CURRENT", owned.back());
        owned.push_back(base / p::temporary_name('x', p::Uuid{}));
        uint64_t token = fengqudb::storage::get64(current.uuid.data()) ^ current.generation;
        p::Uuid image{}; for (unsigned i = 0; i < 8; ++i) image[i] = static_cast<unsigned char>(token >> ((7-i)*8));
        owned.back() = base / p::temporary_name('x', image);
        std::ofstream(owned.back()) << "unpublished image";
        const auto scratch = p::new_uuid();
        owned.push_back(base / p::temporary_name('r', scratch));
        p::Generation generation(0); generation.create(owned.back().u8string(), scratch, 1);
        std::ofstream(base / "business.tmp") << "business";
        std::ofstream(base / "wfedcba9876543210.tmp") << "unrecognized";
        for (const auto *name : {"CURRENT.tmp", "WRITE.tmp", "RECOVER.tmp", "SEGMENT.tmp"}) {
            owned.push_back(base/name); std::ofstream(owned.back()) << "partial unpublished metadata";
        }
    }
    {
        Store db(source);
        for (const auto &path : owned) check(!fs::exists(path), "recognized Store temporary survived open");
        check(fs::exists(fs::u8path(source)/"business.tmp") &&
              fs::exists(fs::u8path(source)/"wfedcba9876543210.tmp"), "unknown temporary was removed");
        check(db.get("stable")->view() == "preserved", "temporary recovery changed source data");
    }
    fs::remove(fs::u8path(source)/"business.tmp"); fs::remove(fs::u8path(source)/"wfedcba9876543210.tmp");
}
static void linked_outputs_are_preserved() {
    Fixture fixture;
    const auto source = fixture.name("source");
    Store db(source);
    put(db, "stable", "preserved");
    const auto identity = p::read_current(source).uuid;
    const auto intent = p::register_output(source, identity, fixture.name("destination"));
    marker(intent);
    const auto external = fs::u8path(fixture.root) / "external.txt";
    std::ofstream(external) << "external data";
    const auto link = fs::u8path(intent.temporary) / "g0000000000000001.fdb";
    std::error_code error;
    fs::create_symlink(external, link, error);
#ifndef _WIN32
    check(!error, "cannot create cleanup symlink fixture");
#endif
    if (!error) {
        check(!p::recover_output(source, identity) && fs::exists(external) &&
                  fs::is_symlink(fs::symlink_status(link)),
              "output cleanup followed a link or removed its containing output");
        fs::remove(link);
        const auto fixed = fs::u8path(source) / "RECOVER.tmp";
        fs::create_symlink(external, fixed);
        p::reclaim_fixed_temporaries(source);
        check(fs::exists(external) && fs::is_symlink(fs::symlink_status(fixed)),
              "fixed temporary cleanup followed a link");
        fs::remove(fixed);
    }
    check(p::recover_output(source, identity), "output cleanup did not recover after link removal");
    fs::remove(external);
}
static void automatic_space_maintenance() {
    Fixture fixture;
    const auto source = fixture.name("automatic");
    fengqudb::Store db(source);
    check(!db.runtime_info().running, "small Store started a polling worker");
    std::string value(524288, 'a');
    for (unsigned i = 0; i < 80; ++i) {
        std::fill(value.begin(), value.end(), static_cast<char>(i));
        // Change the whole payload so delta encoding cannot hide the obsolete
        // bytes. The live value remains 512 KiB throughout.
        db.put("hot", value);
    }
    db.sync();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((!db.runtime_info().compactions || db.runtime_info().running) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(db.runtime_info().compactions > 0, "automatic space maintenance did not run");
    check(!db.runtime_info().running, "automatic housekeeping kept polling an idle Store");
    check(db.get("hot")->view() == value, "automatic maintenance changed the latest value");
    db.verify();
}
static void automatic_failure_does_not_poison_writes() {
    Fixture fixture;
    auto core = std::make_shared<p::Core>(fixture.name("automatic-failure"), p::defaults());
    core->open();
    core->fail_automatic_start = true;
    core->maybe_auto_maintenance(33554432, 0);
    check(!core->background_running && !core->poisoned &&
          core->background_status == FQDB_NO_MEMORY && core->auto_next_limit > 33554432,
          "optional housekeeping failure poisoned writes or caused an immediate retry loop");
    p::Transaction transaction(core, 0); transaction.put("stable", "preserved", 0);
    core->commit(transaction);
    check(core->current()->find("stable").has_value(), "write failed after optional thread allocation failure");
}
int main(int argc, char **argv) {
    try {
        if (argc == 5) {
            Store db(argv[2]); fqdb_store_test_crash(std::stoi(argv[4])); db.backup(argv[3]); return 88;
        }
        repeated_outputs(); ownership_guards(); store_temporaries(); linked_outputs_are_preserved();
        automatic_space_maintenance();
        automatic_failure_does_not_poison_writes();
#ifndef __SANITIZE_THREAD__
        const auto executable = fs::absolute(fs::u8path(argv[0])).u8string();
        for (int point : {30, 31, 32, 20, 21}) {
            Fixture fixture;
            const auto source = fixture.name("source"), backup = fixture.name("backup");
            { Store db(source); put(db, "stable", "preserved"); }
            check(child(executable, "--backup", source, backup, point) == 86, "output crash boundary was not reached");
            { Store db(source); check(db.get("stable")->view() == "preserved", "crash changed source"); db.verify(); }
            check(temporary_outputs(fixture.root) == 0 && !fs::exists(fs::u8path(source)/"OUTPUT"),
                  "reopen did not clean interrupted backup");
            if (point == 21) {
                Store copy(backup); check(copy.get("stable")->view() == "preserved", "published backup was removed");
            } else check(!fs::exists(fs::u8path(backup)), "unpublished backup became visible");
        }
#endif
        std::cout << "PASS output intent recovery, quota cleanup, ownership and Store temporary guards\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
