using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using FengquDB;

static class StoreLimits {
    static byte[] B(string value) => Encoding.UTF8.GetBytes(value);
    static void Check(bool value) {
        if (!value)
            throw new InvalidOperationException("C# unsigned version boundary");
    }
    static void Main(string[] args) {
        string library = Path.GetFullPath(args[1]);
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,
                                           (name, assembly, flags) =>
                                               name == "fengqudb" ? NativeLibrary.Load(library)
                                                                  : IntPtr.Zero);
        using var db = Store.OpenAt(args[0], "main", new StoreOptions { Create = false });
        var entry = db.GetEntry(B("key"))!;
        Check(entry.Version == 0x8000000000000002UL && Encoding.UTF8.GetString(entry.Value) == "C");
        using var watch = db.Watch(entry.Version, B("key"));
        var changed = db.CompareExchange(B("key"), entry.Version, B("D"));
        Check(changed.Matched && changed.Version == entry.Version + 1);
        var batch = watch.Next()!;
        Check(batch.Version == changed.Version);
        watch.Ack(batch.Version);
        Check(db.AddInt64(B("maximum"), long.MaxValue).Value == long.MaxValue);
        Check(db.AddInt64(B("minimum"), long.MinValue).Value == long.MinValue);
        db.Checkpoint();
        db.Verify();
        Console.WriteLine(
            "PASS C# high unsigned versions, CAS/watch, signed counter extremes and restart state");
    }
}
