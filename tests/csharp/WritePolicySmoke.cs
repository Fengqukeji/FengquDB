using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using FengquDB;
static class WritePolicySmoke {
    static byte[] B(string s) => Encoding.UTF8.GetBytes(s);
    static byte[] I64(long value) {
        var bytes = new byte[8];
        System.Buffers.Binary.BinaryPrimitives.WriteInt64LittleEndian(bytes, value);
        return bytes;
    }
    static void Check(bool value) {
        if (!value)
            throw new Exception("write policy C# assertion");
    }
    static async Task Main(string[] args) {
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,
                                           (name, assembly, flags) =>
                                               name == "fengqudb" ? NativeLibrary.Load(args[1])
                                                                  : IntPtr.Zero);
        Check(new StoreWriteOptions().PeriodMs == 50 &&
              new StoreWriteOptions().ThresholdBytes == 1048576);
        using (var db = new Store(args[0])) {
            var p = db.WriteOptions();
            Check(p.Mode == StoreWriteMode.Performance && p.PeriodMs == 50 &&
                  p.ThresholdBytes == 1048576);
            db.ConfigureWrite(new StoreWriteOptions { PeriodMs = 5000 });
            var original = new byte[] { 9, 7, 0, 0, 0, 0, 0, 0, 0, 18, 4, 98, 111, 100, 121 };
            db.Put(B("project"), original);
            Check(db.Project(B("project"), new uint[] { 1, 1 }).Length == 9 &&
                  db.Project(B("project"), Array.Empty<uint>()).Length == 0);
            using (var reader = db.PrepareProjectReader(new[] { B("project"), B("missing") },
                                                        new uint[] { 1 })) {
                Check(reader.Read(rows => rows[0].Value.Length == 9 && rows[0].Value[1] == 7 &&
                                          !rows[1].Found));
                db.Put(B("project"), new byte[] { 9, 8, 0, 0, 0, 0, 0, 0, 0 });
                Check(reader.Read(rows => rows[0].Value[1] == 8));
                reader.Release();
                Check(reader.Read(rows => rows[0].Value[1] == 8));
            }
            db.Put(B("field-a"), I64(100));db.Put(B("field-b"), I64(200));
            var fields = db.AddInt64Fields(new[] {
                new StoreI64FieldUpdate(B("field-a"), 0, -10, 0, 1000),
                new StoreI64FieldUpdate(B("field-b"), 0, 10, 0, 1000)}, forceReliable: true);
            Check(fields.Changed && fields.Value == 210 && db.WriteState().DurableVersion >= fields.Version);
            var fieldVersion = db.Info().Version;
            try {
                db.AddInt64Fields(new[] {
                    new StoreI64FieldUpdate(B("field-a"), 0, -1),
                    new StoreI64FieldUpdate(B("field-b"), 0, -211, 0, 1000)});
                throw new Exception("invalid field update succeeded");
            } catch (DatabaseException) {}
            Check(db.Info().Version == fieldVersion &&
                  System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(db.Get(B("field-a"))!) == 90);
            using var tx = db.Begin();
            tx.Put(B("tx"), B("value"));
            var version = tx.Commit();
            Check(db.Get(B("tx"))![0] == 'v' && db.WriteState().DurableVersion < version);
            using (var forced = db.Begin()) {
                forced.Put(B("forced-tx"), B("yes"));
                var v = forced.Commit(true);
                Check(db.WriteState().DurableVersion >= v);
            }
            db.Put(B("paid"), B("yes"), forceReliable: true);
            Check(db.WriteState().DurableVersion >= version);
            db.Patch(B("paid"), 0, 3, B("new"), true);
            db.Delete(B("paid"), true);
            Check(db.Get(B("paid")) == null);
            using var next = db.Begin();
            next.Put(B("async"), B("yes"));
            using (var receipt = next.Submit(true)) Check(await receipt.WaitAsync() > 0);
            await db.ConfigureWriteAsync(
                new StoreWriteOptions { PeriodMs = 5000, ThresholdBytes = 4096 });
            var receipts = new StoreCommit[4];
            var data = new byte[1024];
            Array.Fill(data, (byte)'a');
            for (int i = 0; i < 4; ++i)
                receipts[i] = db.SubmitPut(B("k" + i), data);
            Array.Fill(data, (byte)'b');
            foreach (var receipt in receipts) {
                Check(await receipt.WaitAsync() > 0);
                receipt.Dispose();
            }
            Check(db.Get(B("k0"))![0] == 'a');
            db.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable, PeriodMs = 37,
                                                      ThresholdBytes = 32768 });
            using var strict = db.Begin();
            strict.Put(B("strict"), B("yes"));
            strict.Commit();
        }
        using (var db = new Store(args[0])) {
            var p = db.WriteOptions();
            Check(p.Mode == StoreWriteMode.Reliable && p.PeriodMs == 37 &&
                  p.ThresholdBytes == 32768);
            await db.ResetWriteOptionsAsync();
            p = db.WriteOptions();
            Check(p.Mode == StoreWriteMode.Performance && p.PeriodMs == 50 &&
                  p.ThresholdBytes == 1048576);
            using var receipt = db.SubmitPut(B("reset"), B("yes"));
            db.Sync();
            Check(await receipt.WaitAsync() > 0);
        }
        using (var db = new Store(args[0])) Check(db.WriteOptions().PeriodMs == 50);
        Console.WriteLine("PASS C# periodic/strict configuration, async receipts and restart");
    }
}
