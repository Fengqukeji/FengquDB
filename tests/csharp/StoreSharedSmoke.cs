using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using FengquDB;
static class StoreSharedSmoke {
    static byte[] B(string s) => Encoding.UTF8.GetBytes(s);
    static void Check(bool value) {
        if (!value)
            throw new Exception("Shared reader assertion");
    }
    static void Rejected(Action action) {
        try {
            action();
        } catch (InvalidOperationException) {
            return;
        }
        throw new Exception("Reentrant operation accepted");
    }
    static void Main(string[] args) {
        NativeLibrary.SetDllImportResolver(
            typeof(Store).Assembly,
            (name, assembly, flags) =>
                name == "fengqudb" ? NativeLibrary.Load(Path.GetFullPath(args[1])) : IntPtr.Zero);
        Check(!default(StoreSharedEntry).Found && default(StoreSharedEntry).Value.IsEmpty);
        foreach (bool segmented in new[] { false, true }) {
            var db = Store.OpenAt(args[0], segmented ? "segments" : "shared",
                                  new StoreOptions { Buffered = true, Segmented = segmented });
            db.PutMany(new[] { new StorePut(B("a"), B("original")),
                               new StorePut(B("e"), Array.Empty<byte>()) });
            using var reader = db.PrepareReader(new[] { B("a"), B("missing"), B("e"), B("a") });
            Check(reader.Read(batch =>
                                  batch.Count == 4 && batch[0].Value.SequenceEqual(B("original")) &&
                                  !batch[1].Found && batch[2].Found && batch[2].Value.IsEmpty));
            reader.Read(batch => {
                Rejected(() => reader.Read(b => b.Count));
                Rejected(reader.Release);
                Rejected(reader.Dispose);
                using (var tx = db.Begin()) {
                    tx.Patch(B("a"), 0, 8, B("new"));
                    tx.Delete(B("e"));
                    tx.Commit();
                }
                db.Checkpoint();
                db.Compact();
                Check(batch[0].Value.SequenceEqual(B("original")) && batch[2].Found);
                return 0;
            });
            Check(reader.Read(batch => batch[0].Value.SequenceEqual(B("new")) && !batch[2].Found));
            try {
                reader.Read<int>(batch => throw new ApplicationException());
            } catch (ApplicationException) {
            }
            Check(reader.Read(batch => batch.Count) == 4);
            reader.Release();
            Check(db.ReadShared(B("a"), row => {
                db.Dispose();
                return row.Found && row.Value.SequenceEqual(B("new"));
            }));
            Check(reader.Read(batch => batch[0].Value.SequenceEqual(B("new"))));
            StoreReadAction<int> noAlloc = batch => batch[0].Value.Length;
            for (int i = 0; i < 20000; ++i)
                reader.Read(noAlloc);
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < 10000; ++i)
                Check(reader.Read(noAlloc) == 3);
            Check(GC.GetAllocatedBytesForCurrentThread() == before);
            using var entered = new ManualResetEventSlim();
            using var attempting = new ManualResetEventSlim();
            using var closed = new ManualResetEventSlim();
            var closer = Task.Run(() => {
                entered.Wait();
                attempting.Set();
                reader.Dispose();
                closed.Set();
            });
            reader.Read(batch => {
                entered.Set();
                attempting.Wait();
                Check(!closed.IsSet && batch[0].Value.Length == 3);
                return 0;
            });
            Check(closer.Wait(5000) && closed.IsSet);
            try {
                reader.Read(noAlloc);
                throw new Exception("Closed read accepted");
            } catch (ObjectDisposedException) {
            }
        }
        using (var db = Store.OpenAt(args[0], "limited",
                                     new StoreOptions { Buffered = true, PinnedBytes = 65536 })) {
            byte[] large = Enumerable.Repeat((byte)7, 131072).ToArray();
            db.PutMany(new[] { new StorePut(B("big"), large) });
            Check(db.Get(B("big"))!.SequenceEqual(large));
            Check(db.Get(B("missing")) == null);
            Check(!db.ReadShared(B("missing"), row => row.Found));
            using var reader = db.PrepareReader(B("big"));
            try {
                reader.Read(b => b.Count);
                throw new Exception("Pin limit ignored");
            } catch (DatabaseException error) {
                Check(error.Code == 9);
            }
            Check(db.Get(B("big"))!.SequenceEqual(large));
        }
        Console.WriteLine("PASS C# shared lifetime, COW, reentrancy, close races, pin fallback " +
                          "and zero hot allocations");
    }
}
