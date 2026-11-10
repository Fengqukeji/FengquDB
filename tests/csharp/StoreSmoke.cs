using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Runtime.InteropServices;
using FengquDB;
static class StoreSmoke {
    static byte[] B(string text) => Encoding.UTF8.GetBytes(text);
    static string S(byte[]? bytes) => bytes == null ? "<missing>" : Encoding.UTF8.GetString(bytes);
    static void Check(bool value) {
        if (!value)
            throw new Exception("C# Store assertion failed");
    }
    static void Queries(Store store) {
        var first = new byte[] { 10, 1, 97, 16, 2, 26, 3, 111, 110, 101 };
        store.PutMany(new[] { new StorePut(B("ops/query/1"), first),
                              new StorePut(B("ops/query/2"), new byte[] { 10, 1, 97, 16, 4, 26, 3,
                                                                          116, 119, 111 }) });
        store.DefineCompoundIndex(B("ops-query"), B("ops/query/"),
                                  new[] { new StoreIndexField(1, 4), new StoreIndexField(2, 5) },
                                  3);
        var query =
            new StoreIndexQuery { Equal = new byte[] { 10, 1, 97 }, Lower = new byte[] { 16, 2 },
                                  Upper = new byte[] { 16, 4 }, UpperInclusive = true,
                                  Fields = new uint[] { 3 } };
        var rows = store.QueryIndex(B("ops-query"), query).Rows;
        Check(rows.Count == 2 && S(rows[0].Key) == "ops/query/1" &&
              rows[0].Value.SequenceEqual(new byte[] { 26, 3, 111, 110, 101 }));
        Check(store.Project(B("ops/query/1"), new uint[] { 3 })
                  .SequenceEqual(new byte[] { 26, 3, 111, 110, 101 }));
        using (var tx = store.Begin()) {
            tx.Put(B("ops/query/3"), new byte[] { 10, 1, 97, 16, 6 });
            var equal = new StoreIndexQuery { Equal = new byte[] { 10, 1, 97 } };
            Check(tx.QueryIndex(B("ops-query"), equal).Rows.Count == 3);
            tx.Put(B("ops/query/3"), new byte[] { 10, 1, 98, 16, 6 });
            Check(tx.QueryIndex(B("ops-query"), equal).Rows.Count == 2);
            tx.Commit();
        }
        store.Verify();
    }
    internal static void Operations(Store store) {
        Queries(store);
        Check(store.PutIfAbsent(B("ops/cas"), B("one")).Changed);
        var entry = store.GetEntry(B("ops/cas"))!;
        Check(S(entry.Value) == "one" &&
              store.CompareExchange(B("ops/cas"), entry.Version, B("two")).Matched);
        Check(!store.CompareExchange(B("ops/cas"), entry.Version, B("wrong")).Matched &&
              !store.PutIfChanged(B("ops/cas"), B("two")).Changed);
        Check(store.AddInt64(B("ops/counter"), 3, 0, 10).Value == 3);
        try {
            store.AddInt64(B("ops/counter"), -4, 0, 10);
            throw new Exception("counter bound");
        } catch (DatabaseException error) {
            Check(error.Code == 20);
        }
        store.PutMany(new[] { new StorePut(B("ops/a"), B("last")),
                              new StorePut(B("ops/empty"), Array.Empty<byte>()) });
        var many =
            store.GetMany(new[] { B("ops/a"), B("ops/missing"), B("ops/empty"), B("ops/a") });
        Check(S(many[0]) == "last" && many[1] == null && many[2]!.Length == 0 &&
              S(many[3]) == "last");
        Check(store.GetEntries(new[] { B("ops/a") })[0]!.Version > 0);
        using (var snap = store.Snapshot()) using (var key = store.PrepareKey(B("ops/a"))) {
            store.PutIfChanged(B("ops/a"), B("new"));
            Check(S(snap.GetMany(new[] { B("ops/a") })[0]) == "last" && S(key.Get()) == "new");
            var buffer = new byte[16];
            Check(key.GetInto(buffer) == 3 && Encoding.UTF8.GetString(buffer, 0, 3) == "new");
            store.Checkpoint();
            Check(S(key.Get()) == "new");
        }
        var batch = store.PrepareBatch(new[] { new StorePut(B("ops/prepared"), B("payload")) });
        using (var tx = store.Begin()) {
            tx.PutBatch(batch);
            batch.Dispose();
            Check(S(tx.GetMany(new[] { B("ops/prepared") })[0]) == "payload" &&
                  tx.GetEntries(new[] { B("ops/prepared") })[0]!.Version == 0);
            tx.Patch(B("ops/prepared"), 0, 1, B("P"));
            tx.Commit();
        }
        Check(S(store.Get(B("ops/prepared"))) == "Payload");
        store.DeleteMany(new[] { B("ops/a"), B("ops/empty") });
    }
    static void HistoryRoundtrip(string path) {
        var root = path + "-history";
        var backup = path + "-history-base";
        var archive = path + "-history-inc";
        var copy = path + "-history-copy";
        ulong from;
        using (var db = Open(root)) {
            db.PutIfChanged(B("key"), B("A"));
            db.HoldHistory(B("backup"));
            from = db.Backup(backup).SnapshotVersion;
            db.HoldHistory(B("backup"), from);
            db.PutIfChanged(B("key"), B("B"));
            db.Compact();
            Check(db.HistoryInfo().Floor == from && db.HistoryInfo().RetainedGenerations == 1);
        }
        using (var db = Open(root)) {
            Check(db.HistoryInfo().ActivePins == 1);
            db.BackupIncremental(archive, from);
            db.ReleaseHistory(B("backup"));
            db.Compact();
            Check(db.HistoryInfo().RetainedGenerations == 0);
        }
        Store.RecoverChain(backup, new[] { archive }, copy);
        using (var db = Open(copy))
            Check(S(db.Get(B("key"))) == "B" && db.HistoryInfo().ActivePins == 0);
        foreach (var directory in new[] { root, backup, archive, copy })
            Clean(directory);
    }
    static void WatchRoundtrip(string path) {
        var root = path + "-watch";
        using (var db = Open(root)) {
            using (var watch = db.Watch(prefix: B("watch/"))) {
                db.PutMany(new[] { new StorePut(B("watch/a"), B("one")),
                                   new StorePut(B("watch/empty"), B("")) });
                db.Compact();
                var batch = watch.Next()!;
                Check(batch.Rows.Length == 2 && S(batch.Rows[0].Value) == "one" &&
                      S(batch.Rows[1].Value) == "");
                Check(watch.Next()!.Version == batch.Version);
                watch.Ack(batch.Version);
                Check(watch.Next(1) == null);
            }
            using (var watch = db.Watch(options: new StoreWatchOptions { MetadataOnly = true })) {
                db.PutIfChanged(B("watch/a"), B("two"));
                var batch = watch.Next()!;
                Check(batch.Rows[0].Value == null && batch.Rows[0].Kind == 1);
                watch.Ack(batch.Version);
            }
            var blocked = db.Watch();
            var waiting = System.Threading.Tasks.Task.Run(() => {
                try {
                    blocked.Next(5000);
                    return 0;
                } catch (DatabaseException e) {
                    return e.Code;
                }
            });
            System.Threading.Thread.Sleep(20);
            Check(!waiting.IsCompleted);
            blocked.Dispose();
            Check(waiting.GetAwaiter().GetResult() == 19);
            blocked.Dispose();
            db.Compact();
            db.Verify();
        }
        Clean(root);
    }
    static void IOBudgetRoundtrip(string path) {
        var root = path + "-io";
        var backup = path + "-io-base";
        var stats = new StoreMaintenanceIO();
        var options = new StoreMaintenanceOptions { IOStats = stats, MaxReadBytes = 1 };
        using (var db = Open(root)) {
            var value = new byte[64000];
            Array.Fill(value, (byte)120);
            db.PutIfChanged(B("key"), value);
            try {
                db.Verify(options);
                throw new Exception("Read quota ignored");
            } catch (DatabaseException error) {
                Check(error.Code == 9 && stats.ReadBytes <= 1 && stats.WriteBytes == 0);
            }
            options.MaxReadBytes = 0;
            db.Backup(backup, options);
            Check(stats.WriteBytes > 64000 && stats.SyncCalls > 0);
            options.MaxWriteBytes = 1;
            try {
                db.Compact(options);
                throw new Exception("Write quota ignored");
            } catch (DatabaseException error) {
                Check(error.Code == 9 && stats.WriteBytes <= 1);
            }
            Check(db.Get(B("key"))!.SequenceEqual(value));
            db.PutIfChanged(B("doc/1"), new byte[] { 8, 1 });
            try {
                db.DefineIndex(B("io-index"), B("doc/"), 1, 1, 3, options);
                throw new Exception("Index quota ignored");
            } catch (DatabaseException error) {
                Check(error.Code == 9 && stats.WriteBytes <= 1);
            }
            options.MaxWriteBytes = 0;
            db.DefineIndex(B("io-index"), B("doc/"), 1, 1, 3, options);
            Check(stats.WriteBytes > 0);
            db.Configure(
                new StoreRuntimeOptions { JobMaxReadBytes = 65536, JobMaxWriteBytes = 65536 });
            db.Configure();
            db.Compact();
            db.Verify();
        }
        Clean(root);
        Clean(backup);
    }
    static void Main(string[] args) {
        var path = args[0];
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,
                                           (name, assembly, flags) =>
                                               name == "fengqudb" ? NativeLibrary.Load(args[1])
                                                                  : IntPtr.Zero);
        if (args.Length > 2) {
            using (var store = Open(path)) {
                Check(
                    store.Get(B("跨语言"))!.SequenceEqual(new byte[] { 8, 150, 1, 18, 2, 0, 255 }));
                Check(S(store.Get(B("java"))) == "java");
                Check(store.HashGet(B("interop-hash"),
                                    B("python"))!.SequenceEqual(new byte[] { 0, 255 }));
                Check(S(store.HashGet(B("interop-hash"), B("java"))) == "java");
                Check(S(store
                            .Execute(StoreOperation.IndexLookup, B("interop-index"),
                                     value: new byte[] { 10, 6, 112, 121, 116, 104, 111, 110 })
                            .Rows[0]
                            .Key) == "inter/1");
                store.Execute(StoreOperation.HashSet, B("interop-hash"), B("csharp"), B("csharp"));
                Check(store.AddInt64(B("interop-counter"), 3).Value == 6);
                Check(S(store.GetMany(new[] { B("interop-bulk"), B("interop-java-batch") })[1]) ==
                      "java");
                var current = store.GetEntry(B("interop-bulk"))!;
                Check(
                    store.CompareExchange(B("interop-bulk"), current.Version, B("csharp")).Matched);
                using (var batch = store.PrepareBatch(
                           new[] { new StorePut(B("interop-csharp-batch"), B("csharp")) }))
                    store.PutBatch(batch);
                using (var tx = store.Begin()) {
                    tx.Put(B("csharp"), B("csharp"));
                    tx.Commit();
                }
            }
            Console.WriteLine("PASS C# interoperated with Python/Java Store bytes");
            return;
        }
        Check(!Directory.Exists(path));
        var backup = path + "-backup";
        var restored = path + "-restored";
        var incremental = path + "-incremental";
        var chain = path + "-chain";
        var legacy = path + "-legacy.fdb";
        var migrated = path + "-migrated";
        using (var store = Open(path)) {
            using (var tx = store.Begin()) {
                tx.Put(B("a"), B("0123456789"));
                tx.Put(B("empty"), Array.Empty<byte>());
                tx.Commit();
            }
            using (var old = store.Snapshot()) {
                using (var tx = store.Begin()) {
                    var mark = tx.Savepoint();
                    tx.Patch(B("a"), 2, 5, B("X"));
                    Check(S(tx.Get(B("a"))) == "01X789");
                    tx.RollbackTo(mark);
                    tx.Patch(B("a"), 2, 3, B("ABC"));
                    tx.Commit();
                }
                Check(S(old.Get(B("a"))) == "0123456789" && S(store.Get(B("a"))) == "01ABC56789");
            }
            using (var snap = store.Snapshot()) {
                var rows = snap.Scan(reverse: true).ToArray();
                Check(rows.Length == 2 && S(rows[0].Key) == "empty" && rows[0].Value.Length == 0);
            }
            using (var tx = store.Begin()) {
                tx.Put(B("cancel"), B("no"));
                tx.Cancel();
                try {
                    tx.Commit();
                    throw new Exception("Cancellation ignored");
                } catch (DatabaseException e) {
                    Check(e.Code == 19);
                }
            }
            using (var tx = store.Begin()) {
                tx.Expire(B("a"), 1);
                tx.Commit();
            }
            Check(store.Get(B("a")) == null);
            var purged = store.PurgeExpired(new StoreExpiryOptions { MaxKeys = 1 });
            Check(purged.RemovedKeys == 1 && purged.RemovedValueBytes == 10 &&
                  store.PurgeExpired().RemovedKeys == 0);
            store.Checkpoint();
            Check(store.Info().DeltaBytes == 0);
            var version = store.Info().Version;
            Check(store.Compact().FinalVersion == version && store.Verify().StoredKeys == 1);
            Check(store.Backup(backup).SnapshotVersion == version);
            Operations(store);
            Check(store.Execute(StoreOperation.HashSet, B("hash"), B("field"), B("value")).Changed);
            Check(S(store.Execute(StoreOperation.HashGet, B("hash"), B("field")).Value) == "value");
            Check(S(store.HashGet(B("hash"), B("field"))) == "value" &&
                  store.HashGet(B("hash"), B("missing")) == null);
            store.Execute(StoreOperation.SetAdd, B("set"), B("member"));
            store.Execute(StoreOperation.ListPushRight, B("list"), value: B("right"));
            Check(S(store.Execute(StoreOperation.ListGet, B("list"), argument: -1).Value) ==
                  "right");
            store.Execute(StoreOperation.SortedAdd, B("scores"), B("member"), score: -1.5);
            Check(store.Execute(StoreOperation.SortedRange, B("scores")).Rows[0].Score == -1.5);
            using (var tx = store.Begin()) {
                tx.Put(B("doc/1"), new byte[] { 10, 1, 120 });
                tx.Commit();
            }
            store.DefineIndex(B("by-name"), B("doc/"), 1, 4, 3);
            Check(S(store
                        .Execute(StoreOperation.IndexLookup, B("by-name"),
                                 value: new byte[] { 10, 1, 120 })
                        .Rows[0]
                        .Key) == "doc/1");
            store.Configure(new StoreRuntimeOptions { GroupMax = 8 });
            Check(store.RuntimeInfo().GroupTransactions > 0);
            store.BackupIncremental(incremental, version);
            store.Checkpoint();
            Store.RecoverChain(backup, new[] { incremental }, chain);
            using (var recovered = Open(chain)) {
                Check(S(recovered.Execute(StoreOperation.HashGet, B("hash"), B("field")).Value) ==
                      "value");
                recovered.Verify();
                recovered.DropIndex(B("by-name"));
            }
        }
        using (var store = Open(path))
            Check(store.Get(B("empty"))!.Length == 0 && store.Info().RecoveredTransactions == 0);
        Store.Restore(backup, restored);
        using (var store = Open(restored))
            Check(store.Get(B("empty"))!.Length == 0 && store.Get(B("a")) == null);
        Clean(incremental);
        Clean(chain);
        Clean(backup);
        Clean(restored);
        Clean(path);
        var root = path + "-root-\u5b58\u6863";
        var resolved = Store.PathAt(root, "slot1");
        Check(!Directory.Exists(root));
        using (var store = Store.OpenAt(root, "slot1", Options())) {
            store.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
            store.PutIfAbsent(B("root"), B("configured"));
        }
        using (var store = Store.OpenAt(root, "slot1", new StoreOptions { Create = false }))
            Check(S(store.Get(B("root"))) == "configured");
        Check(Directory.GetDirectories(root).Length == 1 &&
              Directory.GetFiles(resolved).All(f => Path.GetFileName(f).Length <= 30));
        Clean(resolved);
        Directory.Delete(root);
        HistoryRoundtrip(path);
        WatchRoundtrip(path);
        IOBudgetRoundtrip(path);
        Console.WriteLine(
            "PASS C# Store binding, transactions, compact/verify/backup/restore and restart");
    }
    static StoreOptions Options() => new StoreOptions {
        Segmented = Environment.GetEnvironmentVariable("FQDB_TEST_SEGMENTED") == "1"
    };
    static Store Open(string path) {
        var store = new Store(path, Options());
        if (store.WriteOptions().Mode == StoreWriteMode.Performance)
            store.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
        return store;
    }
    internal static void Clean(string path) {
        var start = new System.Diagnostics.ProcessStartInfo(
            Environment.GetEnvironmentVariable("FQDB_TEST_PYTHON")!) { UseShellExecute = false };
        start.ArgumentList.Add(Environment.GetEnvironmentVariable("FQDB_TEST_CLEANER")!);
        start.ArgumentList.Add("--owner-root");
        start.ArgumentList.Add(Environment.GetEnvironmentVariable("FQDB_TEST_OWNED_ROOT")!);
        start.ArgumentList.Add("--store");
        start.ArgumentList.Add(path);
        using var process = System.Diagnostics.Process.Start(start)!;
        process.WaitForExit();
        Check(process.ExitCode == 0);
    }
}
