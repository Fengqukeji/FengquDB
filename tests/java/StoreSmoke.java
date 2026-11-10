import io.fengqudb.Store;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
public final class StoreSmoke {
    static byte[] b(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }
    static String s(byte[] value) {
        return value == null ? "<missing>" : new String(value, StandardCharsets.UTF_8);
    }
    static void check(boolean condition) {
        if (!condition)
            throw new AssertionError("Java Store assertion failed");
    }
    static void queries(Store store) {
        store.putMany(new Store.Put[] {
            new Store.Put(b("ops/query/1"), new byte[] {10, 1, 97, 16, 2, 26, 3, 111, 110, 101}),
            new Store.Put(b("ops/query/2"), new byte[] {10, 1, 97, 16, 4, 26, 3, 116, 119, 111})});
        store.defineCompoundIndex(
            b("ops-query"), b("ops/query/"),
            new Store.IndexField[] {new Store.IndexField(1, 4), new Store.IndexField(2, 5)}, 3);
        Store.IndexQuery query = new Store.IndexQuery();
        query.equal = new byte[] {10, 1, 97};
        query.lower = new byte[] {16, 2};
        query.upper = new byte[] {16, 4};
        query.upperInclusive = true;
        query.fields = new int[] {3};
        Store.CommandResult rows = store.queryIndex(b("ops-query"), query, null, 256);
        check(rows.rows.size() == 2 && s(rows.rows.get(0).key).equals("ops/query/1") &&
              Arrays.equals(rows.rows.get(0).value, new byte[] {26, 3, 111, 110, 101}));
        check(Arrays.equals(store.project(b("ops/query/1"), new int[] {3}),
                            new byte[] {26, 3, 111, 110, 101}));
        try (Store.Transaction tx = store.begin()) {
            tx.put(b("ops/query/3"), new byte[] {10, 1, 97, 16, 6});
            Store.IndexQuery equal = new Store.IndexQuery();
            equal.equal = new byte[] {10, 1, 97};
            check(tx.queryIndex(b("ops-query"), equal, null, 256).rows.size() == 3);
            tx.put(b("ops/query/3"), new byte[] {10, 1, 98, 16, 6});
            check(tx.queryIndex(b("ops-query"), equal, null, 256).rows.size() == 2);
            tx.commit();
        }
        store.verify();
    }
    static void operations(Store store) {
        queries(store);
        check(store.putIfAbsent(b("ops/cas"), b("one")).changed);
        Store.Entry entry = store.getEntry(b("ops/cas"));
        check(s(entry.value).equals("one") &&
              store.compareExchange(b("ops/cas"), entry.version, b("two")).matched);
        check(!store.compareExchange(b("ops/cas"), entry.version, b("wrong")).matched &&
              !store.putIfChanged(b("ops/cas"), b("two")).changed);
        check(store.addInt64(b("ops/counter"), 3, 0, 10, 0).value == 3);
        try {
            store.addInt64(b("ops/counter"), -4, 0, 10, 0);
            throw new AssertionError("counter bound");
        } catch (Store.DbException error) {
            check(error.code == 20);
        }
        store.putMany(new Store.Put[] {new Store.Put(b("ops/a"), b("last")),
                                       new Store.Put(b("ops/empty"), new byte[0])});
        byte[][] many =
            store.getMany(new byte[][] {b("ops/a"), b("ops/missing"), b("ops/empty"), b("ops/a")});
        check(s(many[0]).equals("last") && many[1] == null && many[2].length == 0 &&
              s(many[3]).equals("last"));
        check(store.getEntries(new byte[][] {b("ops/a")})[0].version > 0);
        try (Store.Snapshot snap = store.snapshot();
             Store.PreparedKey key = store.prepareKey(b("ops/a"))) {
            store.putIfChanged(b("ops/a"), b("new"));
            check(s(snap.getMany(new byte[][] {b("ops/a")})[0]).equals("last") &&
                  s(key.get()).equals("new"));
            byte[] buffer = new byte[16];
            check(key.getInto(buffer) == 3 &&
                  new String(buffer, 0, 3, StandardCharsets.UTF_8).equals("new"));
            store.checkpoint();
            check(s(key.get()).equals("new"));
        }
        Store.PreparedBatch batch =
            store.prepareBatch(new Store.Put[] {new Store.Put(b("ops/prepared"), b("payload"))});
        try (Store.Transaction tx = store.begin()) {
            tx.putBatch(batch);
            batch.close();
            check(s(tx.getMany(new byte[][] {b("ops/prepared")})[0]).equals("payload") &&
                  tx.getEntries(new byte[][] {b("ops/prepared")})[0].version == 0);
            tx.patch(b("ops/prepared"), 0, 1, b("P"));
            tx.commit();
        }
        check(s(store.get(b("ops/prepared"))).equals("Payload"));
        store.deleteMany(new byte[][] {b("ops/a"), b("ops/empty")});
    }
    static void historyRoundtrip(Path path) throws Exception {
        Path root = Paths.get(path + "-history"), base = Paths.get(path + "-history-base"),
             archive = Paths.get(path + "-history-inc"), copy = Paths.get(path + "-history-copy");
        long from;
        try (Store db = open(root.toString())) {
            db.putIfChanged(b("key"), b("A"));
            db.holdHistory(b("backup"));
            from = db.backup(base.toString()).snapshotVersion;
            db.holdHistory(b("backup"), from);
            db.putIfChanged(b("key"), b("B"));
            db.compact();
            check(db.historyInfo().floor == from && db.historyInfo().retainedGenerations == 1);
        }
        try (Store db = open(root.toString())) {
            check(db.historyInfo().activePins == 1);
            db.backupIncremental(archive.toString(), from);
            db.releaseHistory(b("backup"));
            db.compact();
            check(db.historyInfo().retainedGenerations == 0);
        }
        Store.recoverChain(base.toString(), new String[] {archive.toString()}, copy.toString(), 0,
                           0);
        try (Store db = open(copy.toString())) {
            check(s(db.get(b("key"))).equals("B") && db.historyInfo().activePins == 0);
        }
        for (Path directory : new Path[] {root, base, archive, copy})
            clean(directory);
    }
    static void watchRoundtrip(Path path) throws Exception {
        Path root = Paths.get(path + "-watch");
        try (Store db = open(root.toString())) {
            try (Store.Watch watch = db.watch(-1, b("watch/"), new Store.WatchOptions())) {
                db.putMany(new Store.Put[] {new Store.Put(b("watch/a"), b("one")),
                                            new Store.Put(b("watch/empty"), b(""))});
                db.compact();
                Store.Changes batch = watch.next();
                check(batch.rows.length == 2 && s(batch.rows[0].value).equals("one") &&
                      s(batch.rows[1].value).equals(""));
                check(watch.next().version == batch.version);
                watch.ack(batch.version);
                check(watch.next(1) == null);
            }
            Store.WatchOptions options = new Store.WatchOptions();
            options.metadataOnly = true;
            try (Store.Watch watch = db.watch(-1, b(""), options)) {
                db.putIfChanged(b("watch/a"), b("two"));
                Store.Changes batch = watch.next();
                check(batch.rows[0].value == null && batch.rows[0].kind == 1);
                watch.ack(batch.version);
            }
            Store.Watch blocked = db.watch();
            java.util.concurrent.CompletableFuture<Integer> waiting =
                java.util.concurrent.CompletableFuture.supplyAsync(() -> {
                    try {
                        blocked.next(5000);
                        return 0;
                    } catch (Store.DbException error) {
                        return error.code;
                    }
                });
            Thread.sleep(20);
            check(!waiting.isDone());
            blocked.close();
            check(waiting.get() == 19);
            blocked.close();
            db.compact();
            db.verify();
        }
        clean(root);
    }
    static void ioBudgetRoundtrip(Path path) throws Exception {
        Path root = Paths.get(path + "-io"), backup = Paths.get(path + "-io-base");
        Store.MaintenanceIO stats = new Store.MaintenanceIO();
        Store.MaintenanceOptions options = new Store.MaintenanceOptions();
        options.ioStats = stats;
        options.maxReadBytes = 1;
        try (Store db = open(root.toString())) {
            byte[] value = new byte[64000];
            Arrays.fill(value, (byte)120);
            db.putIfChanged(b("key"), value);
            try {
                db.verify(options);
                throw new AssertionError("Read quota ignored");
            } catch (Store.DbException error) {
                check(error.code == 9 && stats.readBytes <= 1 && stats.writeBytes == 0);
            }
            options.maxReadBytes = 0;
            db.backup(backup.toString(), options);
            check(stats.writeBytes > 64000 && stats.syncCalls > 0);
            options.maxWriteBytes = 1;
            try {
                db.compact(options);
                throw new AssertionError("Write quota ignored");
            } catch (Store.DbException error) {
                check(error.code == 9 && stats.writeBytes <= 1);
            }
            check(Arrays.equals(db.get(b("key")), value));
            db.putIfChanged(b("doc/1"), new byte[] {8, 1});
            try {
                db.defineIndex(b("io-index"), b("doc/"), 1, 1, 3, options);
                throw new AssertionError("Index quota ignored");
            } catch (Store.DbException error) {
                check(error.code == 9 && stats.writeBytes <= 1);
            }
            options.maxWriteBytes = 0;
            db.defineIndex(b("io-index"), b("doc/"), 1, 1, 3, options);
            check(stats.writeBytes > 0);
            Store.RuntimeOptions runtime = new Store.RuntimeOptions();
            runtime.jobMaxReadBytes = 65536;
            runtime.jobMaxWriteBytes = 65536;
            db.configure(runtime);
            db.configure();
            db.compact();
            db.verify();
        }
        clean(root);
        clean(backup);
    }
    public static void main(String[] args) throws Exception {
        Path path = Paths.get(args[0]);
        Path incremental = Paths.get(args[0] + "-incremental"),
             chain = Paths.get(args[0] + "-chain"), legacy = Paths.get(args[0] + "-legacy.fdb"),
             migrated = Paths.get(args[0] + "-migrated");

        if (args.length > 1) {
            try (Store store = open(path.toString())) {
                check(Arrays.equals(store.get(b("跨语言")),
                                    new byte[] {8, (byte)150, 1, 18, 2, 0, (byte)255}));
                try (Store.Transaction tx = store.begin()) {
                    tx.put(b("java"), b("java"));
                    tx.commit();
                }
                check(store.purgeExpired().removedKeys == 1);
                check(Arrays.equals(store.hashGet(b("interop-hash"), b("python")),
                                    new byte[] {0, (byte)255}));
                check(s(store
                            .execute(Store.Operation.INDEX_LOOKUP, b("interop-index"), null,
                                     new byte[] {10, 6, 112, 121, 116, 104, 111, 110})
                            .rows.get(0)
                            .key)
                          .equals("inter/1"));
                store.execute(Store.Operation.HASH_SET, b("interop-hash"), b("java"), b("java"));
                check(store.addInt64(b("interop-counter"), 2).value == 3);
                Store.Entry current = store.getEntry(b("interop-bulk"));
                check(store.compareExchange(b("interop-bulk"), current.version, b("java")).matched);
                try (Store.PreparedBatch batch = store.prepareBatch(
                         new Store.Put[] {new Store.Put(b("interop-java-batch"), b("java"))})) {
                    store.putBatch(batch);
                }
                store.compact();
                store.verify();
            }
            System.out.println("PASS Java interoperated with Python Store bytes");
            return;
        }
        check(!Files.exists(path));
        Path backup = Paths.get(args[0] + "-backup"), restored = Paths.get(args[0] + ("-restore"
                                                                                        + "d"));
        try (Store store = open(path.toString())) {
            try (Store.Transaction tx = store.begin()) {
                tx.put(b("a"), b("0123456789"));
                tx.put(b("empty"), new byte[0]);
                tx.commit();
            }
            try (Store.Snapshot old = store.snapshot()) {
                try (Store.Transaction tx = store.begin()) {
                    long mark = tx.savepoint();
                    tx.patch(b("a"), 2, 5, b("X"));
                    check(s(tx.get(b("a"))).equals("01X789"));
                    tx.rollbackTo(mark);
                    tx.patch(b("a"), 2, 3, b("ABC"));
                    tx.commit();
                }
                check(s(old.get(b("a"))).equals("0123456789") &&
                      s(store.get(b("a"))).equals("01ABC56789"));
            }
            try (Store.Snapshot snap = store.snapshot();
                 Store.Cursor rows = snap.scan(new byte[0], new byte[0], true)) {
                check(s(rows.next().key).equals("empty") && s(rows.next().key).equals("a") &&
                      !rows.hasNext());
            }
            try (Store.Transaction tx = store.begin()) {
                tx.put(b("cancel"), b("no"));
                tx.cancel();
                try {
                    tx.commit();
                    throw new AssertionError("Cancellation ignored");
                } catch (Store.DbException e) {
                    check(e.code == 19);
                }
            }
            try (Store.Transaction tx = store.begin()) {
                tx.expire(b("a"), 1);
                tx.commit();
            }
            check(store.get(b("a")) == null);
            Store.ExpiryOptions expiry = new Store.ExpiryOptions();
            expiry.maxKeys = 1;
            Store.ExpiryInfo purged = store.purgeExpired(expiry);
            check(purged.removedKeys == 1 && purged.removedValueBytes == 10 &&
                  store.purgeExpired().removedKeys == 0);
            store.checkpoint();
            check(store.info()[6] == 0);
            long version = store.info()[0];
            check(store.compact().finalVersion == version && store.verify().storedKeys == 1);
            check(store.backup(backup.toString()).snapshotVersion == version);
            operations(store);
            check(
                store.execute(Store.Operation.HASH_SET, b("hash"), b("field"), b("value")).changed);
            check(s(store.execute(Store.Operation.HASH_GET, b("hash"), b("field"), null).value)
                      .equals("value"));
            check(s(store.hashGet(b("hash"), b("field"))).equals("value") &&
                  store.hashGet(b("hash"), b("missing")) == null);
            store.execute(Store.Operation.SET_ADD, b("set"), b("member"), null);
            store.execute(Store.Operation.LIST_PUSH_RIGHT, b("list"), null, b("right"));
            check(s(store.execute(Store.Operation.LIST_GET, b("list"), null, null, -1, 0, 256, 0)
                        .value)
                      .equals("right"));
            store.execute(Store.Operation.SORTED_ADD, b("scores"), b("member"), null, 0, -1.5, 256,
                          0);
            check(store.execute(Store.Operation.SORTED_RANGE, b("scores"), null, null)
                      .rows.get(0)
                      .score == -1.5);
            try (Store.Transaction tx = store.begin()) {
                tx.put(b("doc/1"), new byte[] {10, 1, 120});
                tx.commit();
            }
            store.defineIndex(b("by-name"), b("doc/"), 1, 4, 3);
            check(s(store
                        .execute(Store.Operation.INDEX_LOOKUP, b("by-name"), null,
                                 new byte[] {10, 1, 120})
                        .rows.get(0)
                        .key)
                      .equals("doc/1"));
            Store.RuntimeOptions runtime = new Store.RuntimeOptions();
            runtime.groupMax = 8;
            store.configure(runtime);
            check(store.runtimeInfo().groupTransactions > 0);
            store.backupIncremental(incremental.toString(), version);
            store.checkpoint();
            Store.recoverChain(backup.toString(), new String[] {incremental.toString()},
                               chain.toString(), 0, 0);
            try (Store recovered = open(chain.toString())) {
                check(s(recovered.execute(Store.Operation.HASH_GET, b("hash"), b("field"), null)
                            .value)
                          .equals("value"));
                recovered.verify();
                recovered.dropIndex(b("by-name"));
            }
        }
        try (Store store = open(path.toString())) {
            check(store.get(b("empty")).length == 0 && store.info()[10] == 0);
        }
        Store.restore(backup.toString(), restored.toString());
        try (Store store = open(restored.toString())) {
            check(store.get(b("empty")).length == 0 && store.get(b("a")) == null);
        }
        clean(incremental);
        clean(chain);
        clean(backup);
        clean(restored);
        clean(path);
        Path root = Paths.get(path + "-root-\u5b58\u6863"),
             resolved = Paths.get(Store.pathAt(root.toString(), "slot1"));
        check(!Files.exists(root));
        try (Store store = Store.openAt(root.toString(), "slot1", options())) {
            Store.WriteOptions p = new Store.WriteOptions();
            p.mode = Store.WriteMode.RELIABLE;
            store.configureWrite(p);
            store.putIfAbsent(b("root"), b("configured"));
        }
        Store.Options existing = new Store.Options();
        existing.create = false;
        try (Store store = Store.openAt(root.toString(), "slot1", existing)) {
            check(s(store.get(b("root"))).equals("configured"));
        }
        try (DirectoryStream<Path> files = Files.newDirectoryStream(resolved)) {
            for (Path file : files)
                check(file.getFileName().toString().length() <= 30);
        }
        clean(resolved);
        Files.delete(root);
        historyRoundtrip(path);
        watchRoundtrip(path);
        ioBudgetRoundtrip(path);
        System.out.println(
            "PASS Java Store binding, transactions, compact/verify/backup/restore and restart");
    }
    static Store.Options options() {
        Store.Options options = new Store.Options();
        options.segmented = "1".equals(System.getenv("FQDB_TEST_SEGMENTED"));
        return options;
    }
    static Store open(String path) {
        Store store = new Store(path, options());
        if (store.writeOptions().mode == Store.WriteMode.PERFORMANCE) {
            Store.WriteOptions p = new Store.WriteOptions();
            p.mode = Store.WriteMode.RELIABLE;
            store.configureWrite(p);
        }
        return store;
    }
    static void clean(Path path) throws Exception {
        Process process =
            new ProcessBuilder(System.getenv("FQDB_TEST_PYTHON"),
                               System.getenv("FQDB_TEST_CLEANER"), "--owner-root",
                               System.getenv("FQDB_TEST_OWNED_ROOT"), "--store", path.toString())
                .inheritIO()
                .start();
        check(process.waitFor() == 0);
    }
}
