#include "store_test_support.hpp"
static void empty_target_copy_io() {
    Fixture fixture;
    auto path = fixture.name("copy-io");
    fengqudb::StoreOptions settings;
    settings.durability = fengqudb::Durability::buffered;
    settings.cache_bytes = 0;
    settings.delta_bytes = 65536;
    uint64_t expected = 0, version = 0;
    {
        Store store(path, settings);
        for (unsigned begin = 0; begin < 2048; begin += 32) {
            auto tx = store.transaction();
            for (unsigned i = begin; i < begin + 32; ++i) {
                tx.put("key/" + std::to_string(100000 + i),
                       std::string(128, static_cast<char>(i % 127)), i % 17 ? 0 : 1);
                expected += i % 17 && i % 31;
            }
            commit(tx);
        }
        {
            auto tx = store.transaction();
            for (unsigned i = 0; i < 2048; i += 31)
                tx.erase("key/" + std::to_string(100000 + i));
            commit(tx);
        }
        auto old = store.snapshot();
        version = store.info().version;
        fengqudb::StoreMaintenanceIO io{};
        fengqudb::StoreMaintenanceOptions policy;
        policy.max_read_bytes = 2 * 1024 * 1024;
        policy.max_write_bytes = 4 * 1024 * 1024;
        policy.io_stats = &io;
        fengqudb::StoreMaintenanceInfo copied{};
        try {
            copied = store.compact(policy);
        } catch (const fengqudb::Error &error) {
            std::cerr << "empty-target copy status=" << error.code()
                      << " read_bytes=" << io.read_bytes << " write_bytes=" << io.write_bytes
                      << '\n';
            throw;
        }
        std::cout << "empty-target copy read_bytes=" << io.read_bytes
                  << " write_bytes=" << io.write_bytes << '\n';
        check(copied.final_version == version && copied.stored_keys == expected &&
                  io.read_bytes <= policy.max_read_bytes &&
                  io.write_bytes <= policy.max_write_bytes,
              "empty-target compaction exceeded linear I/O or changed visible counts");
        for (unsigned i = 0; i < 2048; ++i) {
            auto value = store.get("key/" + std::to_string(100000 + i));
            if (i % 17 && i % 31)
                check(value && value->view() == std::string(128, static_cast<char>(i % 127)),
                      "empty-target copy changed a value");
            else
                check(!value, "empty-target copy resurrected deleted or expired data");
        }
        put(store, "later", "new");
        check(!old.get("later") && old.get("key/100001")->view() == std::string(128, 1),
              "empty-target compaction changed a retained snapshot");
        store.verify();
    }
    Store reopened(path, settings);
    check(reopened.info().version == version + 1 && reopened.info().stored_keys == expected + 1 &&
              reopened.get("later")->view() == "new",
          "empty-target compaction lost state on reopen");
    reopened.verify();
}
int main(int argc, char **argv) {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        if (argc == 5) {
            Store store(argv[2]);
            fqdb_store_test_crash(std::stoi(argv[4]));
            if (std::string_view(argv[1]) == "--compact")
                store.compact();
            else
                store.backup(argv[3]);
            return 88;
        }
        empty_target_copy_io();
        {
            Fixture fixture;
            Store store(fixture.name("source"));
            put(store, "key", "value");
            auto oversized =
                (std::filesystem::u8path(fixture.root) / std::string(256, 'b')).u8string();
            check(status([&] { store.backup(oversized); }) == FQDB_LIMIT,
                  "long backup directory component accepted");
            size_t children = 0;
            for (const auto &entry :
                 std::filesystem::directory_iterator(std::filesystem::u8path(fixture.root))) {
                ++children;
                check(entry.path().filename() == "source",
                      "rejected backup created a staging directory");
            }
            check(children == 1, "rejected backup changed directory layout");
#ifdef _WIN32
            constexpr size_t path_limit = 240;
#else
            constexpr size_t path_limit = 960;
#endif
            // The final directory fits, but its 21-character staging sibling
            // cannot leave enough room for internal files. Its parent is
            // deliberately absent: LIMIT must precede any mkdir/IO_ERROR.
            auto parent = std::filesystem::u8path(fixture.root);
            const size_t parent_size = path_limit - 27;
            check(parent.native().size() + 2 < parent_size, "fixture path exceeds boundary");
            while (parent.native().size() < parent_size) {
                const auto remaining = parent_size - parent.native().size();
                auto count = std::min<size_t>(180, remaining - 1);
                if (remaining == count + 2)
                    --count; // Never leave a final component with no room for a name.
                check(count != 0, "invalid boundary fixture component");
                parent /= std::string(count, 'x');
            }
            auto destination = (parent / "b").u8string();
            check(p::directory_path(destination) == destination, "final path should fit");
            check(status([&] { store.backup(destination); }) == FQDB_LIMIT,
                  "staging path was not checked before disk access");
            check(!std::filesystem::exists(parent), "rejected backup created parent directories");
            check(store.get("key")->view() == "value", "rejected backup changed source");
        }
        {
            Fixture fixture;
            auto path = fixture.name("source");
            Store store(path);
            for (unsigned i = 0; i < 8; ++i)
                put(store, "obsolete", std::string(50000, static_cast<char>(i)));
            put(store, "hot", "initial");
            put(store, "delete", "gone later");
            {
                auto tx = store.transaction();
                tx.erase("obsolete");
                tx.put("expired", "drop", 1);
                tx.put("", "");
                commit(tx);
            }
            auto original_blob = std::string(2097152, 'a');
            put(store, "patch", original_blob);
            {
                auto tx = store.transaction();
                tx.patch("patch", 128, 1900000, "z");
                commit(tx);
            }
            auto expected_blob = original_blob;
            expected_blob.replace(128, 1900000, "z");
            std::optional<fengqudb::StoreSnapshot> snapshot(store.snapshot());
            std::optional<fengqudb::StoreTransaction> stale(store.transaction());
            stale->put("stale", "must conflict");
            auto pinned = snapshot->get("hot");
            auto before = p::read_current(path);
            auto old_file = p::join(path, before.filename());
            fengqudb::StoreMaintenanceOptions options;
            options.max_barrier_bytes = 4096;
            std::future<fqdb_store_maintenance_info> task;
            Held hold(10);
            task = std::async(std::launch::async, [&] { return store.compact(options); });
            reached();
            check(status([&] { store.verify(); }) == FQDB_LOCKED,
                  "two maintenance jobs ran together");
            for (unsigned i = 0; i < 100; ++i)
                put(store, "hot", std::string(64000, static_cast<char>(i)));
            {
                auto tx = store.transaction();
                tx.erase("delete");
                tx.put("new", "retained");
                commit(tx);
            }
            auto latest = store.info().version;
            hold.release();
            auto info = task.get();
            check(info.final_version == latest && info.catchup_rounds > 0 && info.copied_keys < 15,
                  "compaction copied intermediate overwrites");
            check(info.output_file_bytes < info.source_file_bytes && !store.get("delete") &&
                      !store.get("expired") && !store.get("obsolete"),
                  "compaction reduction/visibility");
            check(store.get("hot")->view() == std::string(64000, static_cast<char>(99)) &&
                      store.get("new")->view() == "retained",
                  "catch-up lost latest write");
            check(store.get("patch")->view() == expected_blob,
                  "compaction did not collapse subtraction recipe");
            check(snapshot->get("delete")->view() == "gone later" &&
                      snapshot->get("hot")->view() == "initial" && pinned->view() == "initial",
                  "old generation snapshot changed");
            check(stale->commit().status == fengqudb::Status::conflict,
                  "cross-generation transaction accepted");
            check(std::filesystem::exists(std::filesystem::u8path(old_file)),
                  "pinned old generation removed early");
            stale.reset();
            snapshot.reset();
            check(!std::filesystem::exists(std::filesystem::u8path(old_file)),
                  "retired generation not reclaimed");
            check(pinned->view() == "initial", "memory value invalid after file reclamation");
            auto current = p::read_current(path);
            check(current.generation == before.generation + 1, "generation not advanced");
            check(store.verify().stored_keys == store.info().stored_keys,
                  "compacted verify counters");
        }
        {
            Fixture fixture;
            auto path = fixture.name("source"), backup = fixture.name("backup"),
                 restored = fixture.name("restored");
            Store store(path);
            put(store, "value", "before");
            auto version = store.info().version;
            std::future<fqdb_store_maintenance_info> task;
            Held hold(20);
            task = std::async(std::launch::async, [&] { return store.backup(backup); });
            reached();
            check(!std::filesystem::exists(std::filesystem::u8path(backup)),
                  "backup published before completion");
            put(store, "value", "after");
            hold.release();
            auto info = task.get();
            check(info.snapshot_version == version, "backup snapshot drifted");
            {
                Store copy(backup);
                check(copy.get("value")->view() == "before" && copy.info().version == version,
                      "backup not independently readable");
                copy.verify();
            }
            check(store.get("value")->view() == "after", "backup changed source");
            check(status([&] { store.backup(backup); }) == FQDB_ALREADY_EXISTS,
                  "existing backup overwritten");
            auto restored_info = Store::restore(backup, restored);
            check(restored_info.final_version == version, "restore version mismatch");
            {
                Store copy(restored);
                check(copy.get("value")->view() == "before", "restored value mismatch");
                copy.verify();
            }
            check(status([&] { Store::restore(backup, restored); }) == FQDB_ALREADY_EXISTS,
                  "restore overwrote destination");
        }
        {
            Fixture fixture;
            auto path = fixture.name("source");
            Store store(path);
            put(store, "k", "old");
            auto original = p::read_current(path);
            std::future<fqdb_status> cancelled, backlog;
            Held hold(10);
            cancelled =
                std::async(std::launch::async, [&] { return status([&] { store.compact(); }); });
            reached();
            store.cancel_maintenance();
            hold.release();
            check(cancelled.get() == FQDB_CANCELLED, "maintenance cancellation ignored");
            check(p::read_current(path).generation == original.generation,
                  "cancelled maintenance published");
            fengqudb::StoreMaintenanceOptions options;
            options.max_catchup_rounds = 0;
            options.max_barrier_bytes = 0;
            fqdb_store_test_maintenance_hold(10);
            backlog = std::async(std::launch::async,
                                 [&] { return status([&] { store.compact(options); }); });
            reached();
            put(store, "k", "new");
            hold.release();
            check(backlog.get() == FQDB_LIMIT, "unbounded final barrier accepted");
            check(p::read_current(path).generation == original.generation &&
                      store.get("k")->view() == "new",
                  "failed maintenance changed source");
            fqdb_store_test_io_fault(1, 0);
            auto failed = status([&] { store.compact(); });
            fqdb_store_test_io_fault(0, 0);
            check(failed == FQDB_IO_ERROR, "candidate write fault not reported");
            put(store, "healthy", "yes");
            store.verify();
            fqdb_store_test_open_fault(true);
            failed = status([&] { store.compact(); });
            fqdb_store_test_open_fault(false);
            check(failed == FQDB_IO_ERROR, "partial candidate open fault not reported");
            store.compact();
            store.verify();
            fengqudb::StoreMaintenanceOptions deadline;
            deadline.timeout_ms = 500;
            fqdb_store_test_maintenance_hold(10);
            backlog = std::async(std::launch::async,
                                 [&] { return status([&] { store.compact(deadline); }); });
            reached();
            std::this_thread::sleep_for(std::chrono::milliseconds(550));
            hold.release();
            check(backlog.get() == FQDB_TIMEOUT, "maintenance deadline ignored");
            store.verify();
        }
        {
            Fixture fixture;
            auto path = fixture.name("source"), backup = fixture.name("backup");
            {
                Store store(path);
                put(store, "key", "safe");
                fqdb_store_test_publication_fault(true, false);
                check(status([&] { store.compact(); }) == FQDB_COMMIT_UNKNOWN,
                      "ambiguous manifest publication status");
                fqdb_store_test_publication_fault(false, false);
                auto tx = store.transaction();
                tx.put("later", "forbidden");
                check(tx.commit().status == fengqudb::Status::recovery_required,
                      "ambiguous switch allowed source writes");
            }
            {
                Store recovered(path);
                check(recovered.get("key")->view() == "safe" && !recovered.get("later"),
                      "ambiguous switch lost stable state");
                fqdb_store_test_publication_fault(false, true);
                check(status([&] { recovered.backup(backup); }) == FQDB_COMMIT_UNKNOWN,
                      "ambiguous backup publication status");
                fqdb_store_test_publication_fault(false, false);
                put(recovered, "healthy", "yes");
            }
            {
                Store copy(backup);
                check(copy.get("key")->view() == "safe" && !copy.get("healthy"),
                      "ambiguous published backup not usable");
            }
        }
        {
            Fixture fixture;
            auto path = fixture.name("source");
            auto options = p::defaults();
            options.cache_bytes = 0;
            auto core = std::make_shared<p::Core>(path, options);
            core->open();
            core->write_policy.mode = false; // Strict-path fixture.
            {
                p::Transaction tx(core, 0);
                for (unsigned i = 0; i < 2000; ++i)
                    tx.put("key/" + std::to_string(10000 + i), "x", 0);
                core->commit(tx);
            }
            core->checkpoint();
            {
                p::Transaction tx(core, 0);
                tx.erase("key/11999");
                core->commit(tx);
            }
            core->checkpoint();
            auto view = core->current();
            auto before = view->generation->file->reads.load();
            {
                p::ViewCursor all(*view);
                size_t rows = 0;
                while (all.next())
                    ++rows;
                check(rows == 2000, "full cursor count");
            }
            auto full_reads = view->generation->file->reads.load() - before;
            before = view->generation->file->reads.load();
            {
                p::ViewCursor changed(*view, {}, false, view->version - 1);
                auto row = changed.next();
                check(row && row->key == "key/11999" && row->value.deleted() && !changed.next(),
                      "filtered disk cursor lost tombstone");
            }
            check(view->generation->file->reads.load() - before < full_reads / 2,
                  "changed traversal did not prune unchanged subtrees");
            auto ref = view->find("key/10000").value();
            char damaged = 'z';
            view->generation->file->write(ref.offset, &damaged, 1);
            bool corrupt = false;
            try {
                core->verify(p::maintenance_defaults());
            } catch (const p::Error &error) {
                corrupt = error.status == FQDB_CORRUPT;
            }
            check(corrupt, "verify missed disk value damage");
            char repaired = 'x';
            view->generation->file->write(ref.offset, &repaired, 1);
            core->verify(p::maintenance_defaults());
        }
#ifndef __SANITIZE_THREAD__
        auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0])).u8string();
        for (int point : {10, 11, 12, 13, 20, 21}) {
            Fixture fixture;
            auto source = fixture.name("source"), backup = fixture.name("backup");
            uint64_t version;
            {
                Store store(source);
                put(store, "value", std::string(50000, 'a'));
                put(store, "value", "latest");
                store.checkpoint();
                version = store.info().version;
            }
            check(child(executable, point < 20 ? "--compact" : "--backup", source, backup, point) ==
                      86,
                  "maintenance crash point not reached");
            {
                Store reopened(source);
                check(reopened.get("value")->view() == "latest" &&
                          reopened.info().version == version,
                      "maintenance crash lost accepted state");
                reopened.verify();
            }
            if (point == 20)
                check(!std::filesystem::exists(std::filesystem::u8path(backup)),
                      "unpublished backup visible after crash");
            if (point == 21) {
                Store copy(backup);
                check(copy.get("value")->view() == "latest", "published crash backup invalid");
                copy.verify();
            }
            if (point < 20) {
                size_t generations = 0;
                for (const auto &entry :
                     std::filesystem::directory_iterator(std::filesystem::u8path(source)))
                    if (entry.path().extension() == ".fdb")
                        ++generations;
                check(generations == 1, "startup did not reclaim unselected generation");
            }
        }
#else
        std::cout << "SKIP maintenance crash children under TSan; exercised in Windows/ASan runs\n";
#endif
        std::cout << "PASS online compaction, changed-subtree catch-up, snapshot retirement, full "
                     "backup/restore, verify and publication crashes\n";
    } catch (const std::exception &error) {
        fqdb_store_test_maintenance_hold(0);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
