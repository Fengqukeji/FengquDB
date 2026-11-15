using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using FengquDB;
using FengquDB.Entities;
using FengquDB.Examples;

[FqEntity("values/")]
public partial class AllTypes {
    [FqKey, FqField(1)] public string Id = "";
    [FqField(2)] public long Signed;
    [FqField(3)] public ulong Unsigned;
    [FqField(4)] public byte Byte;
    [FqField(5)] public short Short;
    [FqField(6)] public ushort UShort;
    [FqField(7)] public sbyte SByte;
    [FqField(8)] public uint UInt;
    [FqField(9)] public bool Boolean;
    [FqField(10)] public float Single;
    [FqField(11)] public double Double;
    [FqField(12)] public byte[] Bytes = Array.Empty<byte>();
    [FqField(13)] public int? Optional;
    [FqField(14)] public string? Text;
    [FqField(536870911)] public long Last;
}
[FqEntity("unsigned/")]
public partial class UnsignedEntity {
    [FqKey, FqField(1)] public ulong Id;
}
[FqEntity("validated/")]
public partial class ValidatedEntity {
    int id;
    string name = "initial";
    [FqKey, FqField(1)] public int Id {
        get => id;
        set => id = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value));
    }
    [FqField(2)] public string Name {
        get => name;
        set => name = !string.IsNullOrEmpty(value) ? value : throw new ArgumentException(nameof(value));
    }
}
static class TypedSmoke {
    static int sink;
    static void Check(bool value, string message = "Typed assertion") { if (!value) throw new Exception(message); }
    static void Reject(Action action) {
        try { action(); } catch (InvalidDataException) { return; }
        throw new Exception("Malformed protobuf accepted");
    }
    static byte[] Encode<T>(T value) where T : class, IFqEntity<T> {
        byte[] bytes = new byte[value.EncodedSize()];
        Check(value.EncodeTo(bytes) == bytes.Length);
        return bytes;
    }
    static void Scalar<T>(Store db, T value) {
        db.Put("scalar", value);
        Check(EqualityComparer<T>.Default.Equals(db.Get<T>("scalar"), value), typeof(T).Name);
    }
    static double Time(Action action, int count) {
        var timer = Stopwatch.StartNew();
        for (int i = 0; i < count; ++i) action();
        return timer.Elapsed.TotalNanoseconds / count;
    }
    static void Compare(string name, Action baseline, Action typed, int count) {
        for (int i = 0; i < 20000; ++i) { baseline(); typed(); }
        var before = new double[5]; var after = new double[5];
        for (int sample = 0; sample < before.Length; ++sample) {
            if ((sample & 1) == 0) { before[sample] = Time(baseline, count); after[sample] = Time(typed, count); }
            else { after[sample] = Time(typed, count); before[sample] = Time(baseline, count); }
        }
        Array.Sort(before); Array.Sort(after);
        Console.WriteLine($"{name}: baseline {before[2]:F1} ns/op, typed {after[2]:F1} ns/op, ratio {after[2] / before[2]:F3}");
    }
    static void CompareSaves(Action raw, Action handwritten, Action generated) {
        // Measure all three paths in one window. Rotate their order so WAL
        // growth, tiered compilation and periodic I/O do not favor one path.
        var actions = new[] { raw, handwritten, generated };
        var times = new[] { new double[201], new double[201], new double[201] };
        var order = new[] { new[] {0,1,2}, new[] {0,2,1}, new[] {1,0,2},
                            new[] {1,2,0}, new[] {2,0,1}, new[] {2,1,0} };
        for (int i = 0; i < 20000; ++i) { raw(); handwritten(); generated(); }
        for (int sample = 0; sample < times[0].Length; ++sample)
            foreach (int path in order[sample % order.Length]) times[path][sample] = Time(actions[path], 128);
        foreach (var values in times) Array.Sort(values);
        Console.WriteLine($"entity save same window: pre-encoded {times[0][100]:F1} ns/op, handwritten {times[1][100]:F1} ns/op, generated {times[2][100]:F1} ns/op");
    }
    static int HandEncode(Student student, Span<byte> output) {
        int position = FqWire.WriteVarint(output, 8);
        position += FqWire.WriteVarint(output.Slice(position), FqWire.ZigZag(student.Id));
        position += FqWire.WriteVarint(output.Slice(position), 18);
        position += FqWire.WriteString(output.Slice(position), student.Name);
        position += FqWire.WriteVarint(output.Slice(position), 24);
        position += FqWire.WriteVarint(output.Slice(position), FqWire.ZigZag(student.Score));
        return position;
    }
    static int HandReadInt(ReadOnlySpan<byte> source) {
        int position = 0, result = 0;
        bool found = false;
        while (position < source.Length) {
            uint tag = FqWire.ReadTag(source, ref position);
            if (tag == 8) { result = checked((int)FqWire.UnZigZag(FqWire.ReadVarint(source, ref position))); found = true; }
            else FqWire.Skip(source, ref position, tag);
        }
        return found ? result : throw new InvalidDataException();
    }
    static ulong HandSave(Store db, Student student, byte[] prefix) {
        Span<byte> buffer = stackalloc byte[512];
        int keySize = FqWire.KeySize(prefix, (long)student.Id);
        int keyBytes = FqWire.WriteKey(buffer.Slice(0, keySize), prefix, (long)student.Id);
        int size = HandEncode(student, buffer.Slice(keySize));
        return db.Put(buffer.Slice(0, keyBytes), buffer.Slice(keySize, size)).Version;
    }
    static void Main(string[] args) {
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,
            (name, assembly, flags) => name == "fengqudb" ? NativeLibrary.Load(Path.GetFullPath(args[1])) : IntPtr.Zero);
        string path = Path.GetFullPath(args[0]);
        using (var db = new Store(path)) {
            Scalar(db, int.MinValue); Scalar(db, int.MaxValue); Scalar(db, 0);
            db.Put("golden", int.MinValue);
            Check(db.Get(FqWire.Utf8.GetBytes("golden"))!.SequenceEqual(new byte[] { 8, 255, 255, 255, 255, 15 }));
            db.Put("golden", 1.5f);
            Check(db.Get(FqWire.Utf8.GetBytes("golden"))!.SequenceEqual(new byte[] { 13, 0, 0, 192, 63 }));
            Scalar(db, long.MinValue); Scalar(db, long.MaxValue); Scalar(db, ulong.MaxValue);
            Scalar(db, uint.MaxValue); Scalar(db, byte.MaxValue); Scalar(db, sbyte.MinValue);
            Scalar(db, short.MinValue); Scalar(db, ushort.MaxValue);
            Scalar(db, true); Scalar(db, false); Scalar(db, -123.5); Scalar(db, -123.5f);
            Scalar(db, "中文🙂\0"); Scalar(db, ""); Scalar(db, new string('x', 8192));
            db.Put("bytes", new byte[] { 0, 255 }); Check(db.Get<byte[]>("bytes").SequenceEqual(new byte[] { 0, 255 }));
            Check(!db.TryGet<int>("absent", out _));
            db.Put("expired", 42, expiresAtMs: 1);
            Check(!db.TryGet<int>("expired", out _));
            try { db.Put("unsupported", new object()); throw new Exception("Unsupported scalar accepted"); }
            catch (NotSupportedException) { }
            try { db.Put<string>("null", null!); throw new Exception("Null scalar accepted"); }
            catch (ArgumentNullException) { }
            try { db.Put("too-large", new byte[16777216]); throw new Exception("Oversized scalar accepted"); }
            catch (ArgumentOutOfRangeException) { }
            Check(!db.TryGet<byte[]>("too-large", out _));
            try { db.Get<int>("absent"); throw new Exception("Missing value accepted"); } catch (KeyNotFoundException) { }
            var student = new Student { Id = 1, Name = "张三🙂", Score = 95 };
            Check(db.Put(student, forceReliable: true) == db.Info().Version);
            Check(db.Info().DurableVersion >= db.Info().Version);
            Check(db.GetEntity<Student>(1)!.Name == student.Name);
            Check(db.GetEntity<Student>(2) == null);
            db.Put(new ValidatedEntity { Id = 1, Name = "Ada" });
            Check(db.GetEntity<ValidatedEntity>(1)!.Name == "Ada", "Decoder assigned transient defaults through validated setters");
            using (var tx = db.Begin()) {
                tx.Put("inside", -42); tx.Put(new Student { Id = 2, Name = "李四", Score = -1 });
                Check(tx.Get<int>("inside") == -42 && tx.GetEntity<Student>(2)!.Score == -1);
                Check(db.GetEntity<Student>(2) == null);
                tx.Commit(forceReliable: true);
            }
            using (var tx = db.Begin()) { tx.Put("rollback", 1); }
            Check(!db.TryGet<int>("rollback", out _));
            var full = new AllTypes { Id = "中文", Signed = long.MinValue, Unsigned = ulong.MaxValue,
                Byte = 255, Short = short.MinValue, UShort = ushort.MaxValue, SByte = sbyte.MinValue,
                UInt = uint.MaxValue, Boolean = true, Single = -0.25f, Double = double.MaxValue,
                Bytes = new byte[] { 0, 128, 255 }, Optional = -42, Text = "", Last = long.MaxValue };
            byte[] encoded = Encode(full);
            var decoded = AllTypes.Decode(encoded);
            Check(Encode(decoded).SequenceEqual(encoded));
            full.Optional = null; full.Text = null;
            Check(AllTypes.Decode(Encode(full)).Optional == null && AllTypes.Decode(Encode(full)).Text == null);
            db.Put(full); Check(db.GetEntity<AllTypes>("中文")!.Signed == long.MinValue);
            db.Put(new UnsignedEntity { Id = ulong.MaxValue });
            Check(db.GetEntity<UnsignedEntity>(ulong.MaxValue)!.Id == ulong.MaxValue);
            byte[] original = Encode(student);
            Check(original.SequenceEqual(new byte[] { 8, 2, 18, 10, 229, 188, 160, 228, 184, 137, 240, 159, 153, 130, 24, 190, 1 }));
            // Unknown scalar and group fields survive an older client's save.
            byte[] unknown = new byte[] { 0xA0, 0x06, 0x7F, 0xAB, 0x06, 0x08, 0x01, 0xAC, 0x06 };
            Check(Encode(Student.Decode(original.Concat(unknown).ToArray())).SequenceEqual(original.Concat(unknown)));
            Check(Student.Decode(new byte[] { 24, 2, 24, 4 }).Score == 2);
            for (int length = 1; length < encoded.Length; ++length) {
                // A prefix ending at a field boundary is a valid older schema.
                try { AllTypes.Decode(encoded.AsSpan(0, length)); } catch (InvalidDataException) { }
            }
            foreach (var bad in new[] { new byte[] { 0 }, new byte[] { 0x0E }, new byte[] { 0x0C },
                new byte[] { 8, 0x80 }, new byte[] { 18, 5, 65 },
                new byte[] { 8, 255, 255, 255, 255, 255, 255, 255, 255, 255, 2 },
                new byte[] { 18, 2, 0xC0, 0x80 }, new byte[] { 0xAB, 6 } }) {
                try { Reject(() => Student.Decode(bad)); }
                catch (System.Text.DecoderFallbackException) { }
            }
            Reject(() => AllTypes.Decode(new byte[] { 72, 2 }));
            try { AllTypes.Decode(new byte[] { 32, 128, 2 }); throw new Exception("Narrow integer overflow accepted"); }
            catch (OverflowException) { }
            db.Put(FqWire.Utf8.GetBytes("invalid"), new byte[] { 8, 0x80 });
            ulong pins = db.Info().PinnedBytes;
            for (int i = 0; i < 10; ++i) Reject(() => db.Get<int>("invalid"));
            Check(db.Info().PinnedBytes == pins, "Decoder failure leaked a native lease");
            foreach (int length in new[] { 170, 500 }) {
                var boundary = new Student { Id = 2000 + length, Name = new string('x', length), Score = 42 };
                db.Put(boundary);
                Check(db.GetEntity<Student>(boundary.Id)!.Name == boundary.Name, "Capacity-bound fallback failed");
            }
            var scores = Q.From("student/").Where(Student.Cols.Score >= 90L)
                .Select(Student.Cols.Name).Run(db);
            Check(scores.Count == 1);
            Parallel.For(0, 4, thread => {
                for (int i = 0; i < 100; ++i) {
                    var value = new Student { Id = 100 + thread, Name = new string((char)('a' + thread), 2048), Score = i };
                    db.Put(value); Check(db.GetEntity<Student>(value.Id)!.Score == i);
                }
            });
            db.Put("hot", 95);
            using var scalarReader = db.PrepareValue<int>("hot");
            using (var missingReader = db.PrepareValue<int>("later")) {
                Check(!missingReader.TryRead(out _));
                db.Put("later", 1);
                Check(missingReader.Read() == 1);
            }
            using var manualScalarReader = db.PrepareReader(FqWire.Utf8.GetBytes("hot"));
            using var entityReader = db.PrepareEntity<Student>(1);
            using var rawReader = db.PrepareReader(Student.MakeKey(1));
            StoreReadAction<int> field = rows => Student.ReadScore(rows[0].Value);
            for (int i = 0; i < 20000; ++i) { scalarReader.Read(); rawReader.Read(field); entityReader.Read(); }
            long allocations = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < 10000; ++i) {
                Check(scalarReader.Read() == 95); Check(rawReader.Read(field) == 95);
                Check(db.Get<int>("hot") == 95);
            }
            Check(GC.GetAllocatedBytesForCurrentThread() == allocations, "Hot scalar/field reader allocated");
            allocations = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < 1000; ++i) { db.Put("hot", 95); db.Put(student); }
            Check(GC.GetAllocatedBytesForCurrentThread() == allocations, "Small typed writes allocated");
            StoreReadAction<int> manualScalar = rows => HandReadInt(rows[0].Value);
            Compare("prepared scalar decode", () => sink = manualScalarReader.Read(manualScalar),
                () => sink = scalarReader.Read(), 200000);
            byte[] buffer = new byte[128];
            Compare("protobuf encode", () => sink = HandEncode(student, buffer),
                () => sink = student.EncodeTo(buffer), 200000);
            byte[] key = Student.MakeKey(1);
            var variants = Enumerable.Range(0, 16).Select(i => Encode(new Student { Id = 1, Name = student.Name, Score = 95 + i })).ToArray();
            int rawIndex = 0, manualIndex = 0, typedIndex = 0;
            var manual = new Student { Id = student.Id, Name = student.Name };
            byte[] prefix = FqWire.Utf8.GetBytes("student/");
            CompareSaves(() => db.Put(key, variants[rawIndex++ & 15]),
                () => { manual.Score = 95 + (manualIndex++ & 15); HandSave(db, manual, prefix); },
                () => { student.Score = 95 + (typedIndex++ & 15); db.Put(student); });
            student.Score = 95; db.Put(student);
            db.Sync();
        }
        using (var reopened = new Store(path)) {
            Check(reopened.GetEntity<Student>(1)!.Score == 95 && reopened.Get<int>("inside") == -42);
        }
        using (var limited = new Store(path + "-limited", new StoreOptions { PinnedBytes = 65536 })) {
            var large = new Student { Id = 1, Name = new string('x', 131072), Score = 95 };
            limited.Put(large);
            Check(limited.GetEntity<Student>(1)!.Name == large.Name, "Pin-budget fallback failed");
        }
        Console.WriteLine("PASS typed scalars, generated entities, queries, transactions, schema evolution, malformed input and zero-allocation hot reads");
    }
}
