using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace FengquDB {
public enum StoreOperation : uint {
    HashGet = 1,
    HashSet = 2,
    HashDelete = 3,
    HashScan = 4,
    SetContains = 10,
    SetAdd = 11,
    SetRemove = 12,
    SetScan = 13,
    ListPushLeft = 20,
    ListPushRight = 21,
    ListPopLeft = 22,
    ListPopRight = 23,
    ListGet = 24,
    ListSet = 25,
    ListRange = 26,
    SortedAdd = 30,
    SortedRemove = 31,
    SortedScore = 32,
    SortedRange = 33,
    CollectionSize = 40,
    CollectionClear = 41,
    IndexLookup = 50,
    IndexQuery = 51,
    RecordProject = 52
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreIndexField {
    public uint Field, Type;
    public StoreIndexField(uint field, uint type) {
        Field = field;
        Type = type;
    }
}
public sealed class StoreIndexQuery {
    public byte[] Equal = Array.Empty<byte>();
    public byte[]? Lower, Upper;
    public bool LowerExclusive, UpperInclusive, Reverse, IncludeValues;
    public uint[] Fields = Array.Empty<uint>();
    internal byte[] Encode() {
        if (Fields == null || Fields.Length > 256)
            throw new ArgumentException("Invalid projection fields");
        using (var stream = new MemoryStream()) using (var w = new BinaryWriter(stream)) {
            StoreFeatureWire.Bytes(w, 1, Equal ?? Array.Empty<byte>());
            if (Lower != null)
                StoreFeatureWire.Bytes(w, 2, Lower);
            if (Upper != null)
                StoreFeatureWire.Bytes(w, 3, Upper);
            StoreFeatureWire.Var(w, 32);
            StoreFeatureWire.Var(w, (LowerExclusive ? 1ul : 0) | (UpperInclusive ? 2ul : 0) |
                                        (Reverse ? 4ul : 0) | (IncludeValues ? 8ul : 0));
            foreach (uint field in Fields) {
                if (field == 0 || field > 0x1fffffff)
                    throw new ArgumentException("Invalid projection field");
                StoreFeatureWire.Var(w, 40);
                StoreFeatureWire.Var(w, field);
            }
            return stream.ToArray();
        }
    }
}
public sealed class StoreFeatureRow {
    public byte[] Key = Array.Empty<byte>(), Value = Array.Empty<byte>();
    public double Score;
}
public sealed class StoreCommandResult {
    public ulong Version, Count;
    public bool Changed;
    public byte[]? Value;
    public byte[] Cursor = Array.Empty<byte>();
    public List<StoreFeatureRow> Rows { get; } = new List<StoreFeatureRow>();
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreRuntimeInfo {
    public ulong Running, LastStatus, ExpiryBatches, Compactions, GroupFlushes, GroupTransactions;
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreRuntimeNative {
    internal uint Size, Abi, Background, Interval, ExpiryKeys, GroupMax, Delay, Garbage;
    internal ulong Minimum, Growth, Timeout, MaxRead, MaxWrite;
}
public sealed class StoreRuntimeOptions {
    public bool Background { get; set; }
    public uint IntervalMs { get; set; } = 200;
    public uint ExpiryKeys { get; set; } = 256;
    public uint GroupMax { get; set; } = 128;
    public uint GroupDelayUs { get; set; }
    public uint GarbagePercent { get; set; } = 30;
    public ulong CompactMinBytes { get; set; } = 33554432;
    public ulong CompactGrowthBytes { get; set; } = 33554432;
    public ulong JobTimeoutMs { get; set; } = 1000;
    public ulong JobMaxReadBytes { get; set; }
    public ulong JobMaxWriteBytes { get; set; }
    internal StoreRuntimeNative Native() => new StoreRuntimeNative {
        Size = (uint)Marshal.SizeOf<StoreRuntimeNative>(),
        Abi = JobMaxReadBytes != 0 || JobMaxWriteBytes != 0 ? 2u : 1u,
        Background = Background ? 1u : 0u,
        Interval = IntervalMs,
        ExpiryKeys = ExpiryKeys,
        GroupMax = GroupMax,
        Delay = GroupDelayUs,
        Garbage = GarbagePercent,
        Minimum = CompactMinBytes,
        Growth = CompactGrowthBytes,
        Timeout = JobTimeoutMs,
        MaxRead = JobMaxReadBytes,
        MaxWrite = JobMaxWriteBytes
    };
}
internal static class StoreFeatureWire {
    internal static void Var(BinaryWriter w, ulong v) {
        while (v >= 128) {
            w.Write((byte)((v & 127) | 128));
            v >>= 7;
        }
        w.Write((byte)v);
    }
    internal static void Bytes(BinaryWriter w, int n, byte[] v) {
        Var(w, (ulong)(n * 8 + 2));
        Var(w, (ulong)v.Length);
        w.Write(v);
    }
    internal static byte[] Request(StoreOperation op, byte[] scope, byte[]? key, byte[]? value,
                                   long argument, double score, ulong limit, uint flags) {
        if (scope == null)
            throw new ArgumentNullException(nameof(scope));
        using (var stream = new MemoryStream()) using (var w = new BinaryWriter(stream)) {
            w.Write((byte)8);
            Var(w, (uint)op);
            Bytes(w, 2, scope);
            Bytes(w, 3, key ?? Array.Empty<byte>());
            Bytes(w, 4, value ?? Array.Empty<byte>());
            w.Write((byte)41);
            w.Write(argument);
            w.Write((byte)49);
            w.Write(score);
            w.Write((byte)57);
            w.Write(limit);
            w.Write((byte)64);
            Var(w, flags);
            return stream.ToArray();
        }
    }
    static ulong Var(BinaryReader r) {
        ulong value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            byte b = r.ReadByte();
            value |= (ulong)(b & 127) << shift;
            if ((b & 128) == 0)
                return value;
        }
        throw new InvalidDataException();
    }
    internal static IEnumerable<(int Number, byte[]? Bytes, ulong Value)> Fields(byte[] bytes) {
        using (var stream = new MemoryStream(bytes, false)) using (
            var r = new BinaryReader(stream)) while (stream.Position < stream.Length) {
            ulong tag = Var(r);
            int kind = (int)(tag & 7);
            byte[]? data = null;
            ulong value = 0;
            if (kind == 0)
                value = Var(r);
            else if (kind == 1)
                value = r.ReadUInt64();
            else if (kind == 5)
                value = r.ReadUInt32();
            else if (kind == 2) {
                ulong length = Var(r);
                if (length > (ulong)(stream.Length - stream.Position) || length > Int32.MaxValue)
                    throw new InvalidDataException();
                data = r.ReadBytes((int)length);
            } else
                throw new InvalidDataException();
            yield return ((int)(tag >> 3), data, value);
        }
    }
    internal static StoreCommandResult Result(byte[] bytes) {
        var result = new StoreCommandResult();
        foreach (var f in Fields(bytes)) {
            if (f.Number == 1)
                result.Value = f.Bytes;
            else if (f.Number == 2)
                result.Count = f.Value;
            else if (f.Number == 3)
                result.Changed = f.Value != 0;
            else if (f.Number == 5)
                result.Cursor = f.Bytes!;
            else if (f.Number == 4) {
                var row = new StoreFeatureRow();
                foreach (var p in Fields(f.Bytes!)) {
                    if (p.Number == 1)
                        row.Key = p.Bytes!;
                    else if (p.Number == 2)
                        row.Value = p.Bytes!;
                    else if (p.Number == 3)
                        row.Score = BitConverter.Int64BitsToDouble(unchecked((long)p.Value));
                }
                result.Rows.Add(row);
            }
        }
        return result;
    }
}
internal static partial class StoreNative {
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_hash_get_view(StoreHandle h, Slice scope, Slice key,
                                                        out IntPtr result);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern Slice fqdb_store_value_data(IntPtr h);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_value_close(IntPtr h);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_execute(StoreHandle h, Slice request,
                                                      out Slice result);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_configure(StoreHandle h, ref StoreRuntimeNative options);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_runtime_info_get(StoreHandle h,
                                                           out StoreRuntimeInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_index_define(StoreHandle h, Slice name, Slice prefix,
                                                       uint field, uint type, uint flags,
                                                       ref StoreNativeMaintenanceOptions options,
                                                       out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int fqdb_store_index_define_compound(
        StoreHandle h, Slice name, Slice prefix, StoreIndexField *fields, uint count, uint flags,
        ref StoreNativeMaintenanceOptions options, out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_index_drop(StoreHandle h, Slice name,
                                                     ref StoreNativeMaintenanceOptions options,
                                                     out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int
    fqdb_store_backup_incremental(StoreHandle h, [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                  ulong from, ref StoreNativeMaintenanceOptions options,
                                  out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_recover_chain(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string source, [In] IntPtr[] archives, uint count,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string destination, ulong version, ulong time,
        ref StoreNativeMaintenanceOptions options, out StoreMaintenanceInfo info);
}
public sealed partial class StoreTransaction {
    public StoreCommandResult
    QueryIndex(byte[] name, StoreIndexQuery? query = null, byte[]? cursor = null,
               ulong limit = 256) => Execute(StoreOperation.IndexQuery, name, cursor,
                                             (query ?? new StoreIndexQuery()).Encode(),
                                             limit: limit);
    public byte[] Project(byte[] key, uint[] fields) =>
        Execute(StoreOperation.RecordProject, Array.Empty<byte>(), key,
                new StoreIndexQuery { Fields = fields }.Encode())
            .Value!;

    public unsafe StoreCommandResult Execute(StoreOperation operation, byte[] scope,
                                             byte[]? key = null, byte[]? value = null,
                                             long argument = 0, double score = 0, ulong limit = 256,
                                             uint flags = 0) {
        var request =
            StoreFeatureWire.Request(operation, scope, key, value, argument, score, limit, flags);
        lock (gate) fixed(byte *p = request) {
            int status = StoreNative.fqdb_store_txn_execute(
                handle, new Slice((IntPtr)p, (ulong)request.Length), out var result);
            Native.Check(status);
            return StoreFeatureWire.Result(Native.Read(status, result)!);
        }
    }
}
public sealed partial class Store {
    public StoreCommandResult
    QueryIndex(byte[] name, StoreIndexQuery? query = null, byte[]? cursor = null,
               ulong limit = 256) => Execute(StoreOperation.IndexQuery, name, cursor,
                                             (query ?? new StoreIndexQuery()).Encode(),
                                             limit: limit);
    public unsafe byte[] Project(byte[] key, uint[] fields) {
        if (key == null || fields == null || fields.Length > 256)
            throw new ArgumentException("Invalid projection");
        fixed(byte *k = key) {
            int status =
                StoreNative.fqdb_store_project(handle, new Slice((IntPtr)k, (ulong)key.Length),
                                               fields, (uint)fields.Length, out var output);
            Native.Check(status);
            return Native.Read(status, output)!;
        }
    }

    public unsafe byte[]? HashGet(byte[] scope, byte[] key) {
        if (scope == null || key == null)
            throw new ArgumentNullException();
        IntPtr value;
        fixed(byte *a = scope, b = key) {
            int status = StoreNative.fqdb_store_hash_get_view(
                handle, new Slice((IntPtr)a, (ulong)scope.Length),
                new Slice((IntPtr)b, (ulong)key.Length), out value);
            if (status == 1)
                return null;
            Native.Check(status);
        }
        try {
            return Native.Copy(StoreNative.fqdb_store_value_data(value));
        } finally {
            StoreNative.fqdb_store_value_close(value);
        }
    }
    public StoreCommandResult Execute(StoreOperation operation, byte[] scope, byte[]? key = null,
                                      byte[]? value = null, long argument = 0, double score = 0,
                                      ulong limit = 256, uint flags = 0,
                                      bool forceReliable = false) {
        using (var tx = Begin()) {
            var result = tx.Execute(operation, scope, key, value, argument, score, limit, flags);
            result.Version = tx.Commit(forceReliable);
            return result;
        }
    }
    public void Configure(StoreRuntimeOptions? options = null) {
        var native = (options ?? new StoreRuntimeOptions()).Native();
        Native.Check(StoreNative.fqdb_store_configure(handle, ref native));
    }
    public StoreRuntimeInfo RuntimeInfo() {
        Native.Check(StoreNative.fqdb_store_runtime_info_get(handle, out var info));
        return info;
    }
    public unsafe
        StoreMaintenanceInfo DefineCompoundIndex(byte[] name, byte[] prefix,
                                                 StoreIndexField[] fields, uint flags = 0,
                                                 StoreMaintenanceOptions? options = null) {
        if (name == null || prefix == null || fields == null || fields.Length == 0 ||
            fields.Length > 8)
            throw new ArgumentException("Invalid index definition");
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var settings = lease.Options;
        fixed(byte *a = name, b = prefix) fixed(StoreIndexField *parts = fields) {
            Native.Check(StoreNative.fqdb_store_index_define_compound(
                handle, new Slice((IntPtr)a, (ulong)name.Length),
                new Slice((IntPtr)b, (ulong)prefix.Length), parts, (uint)fields.Length, flags,
                ref settings, out var info));
            return info;
        }
    }
    public unsafe StoreMaintenanceInfo DefineIndex(byte[] name, byte[] prefix, uint field,
                                                   uint type, uint flags = 0,
                                                   StoreMaintenanceOptions? options = null) {
        if (name == null || prefix == null)
            throw new ArgumentNullException();
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        fixed(byte *a = name, b = prefix) {
            Native.Check(StoreNative.fqdb_store_index_define(
                handle, new Slice((IntPtr)a, (ulong)name.Length),
                new Slice((IntPtr)b, (ulong)prefix.Length), field, type, flags, ref native,
                out var info));
            return info;
        }
    }
    public unsafe StoreMaintenanceInfo DropIndex(byte[] name,
                                                 StoreMaintenanceOptions? options = null) {
        if (name == null)
            throw new ArgumentNullException(nameof(name));
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        fixed(byte *a = name) {
            Native.Check(StoreNative.fqdb_store_index_drop(
                handle, new Slice((IntPtr)a, (ulong)name.Length), ref native, out var info));
            return info;
        }
    }
    public StoreMaintenanceInfo BackupIncremental(string destination, ulong fromVersion,
                                                  StoreMaintenanceOptions? options = null) {
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        Native.Check(StoreNative.fqdb_store_backup_incremental(
            handle, StoreNative.Path(destination), fromVersion, ref native, out var info));
        return info;
    }
    public static StoreMaintenanceInfo RecoverChain(string source, string[] archives,
                                                    string destination, ulong targetVersion = 0,
                                                    ulong targetUnixMs = 0,
                                                    StoreMaintenanceOptions? options = null) {
        if (archives == null || archives.Length > 1024)
            throw new ArgumentException(nameof(archives));
        var pointers = new IntPtr[archives.Length];
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        try {
            for (int i = 0; i < archives.Length; i++)
                pointers[i] = Marshal.StringToCoTaskMemUTF8(StoreNative.Path(archives[i]));
            Native.Check(StoreNative.fqdb_store_recover_chain(
                StoreNative.Path(source), pointers, (uint)pointers.Length,
                StoreNative.Path(destination), targetVersion, targetUnixMs, ref native,
                out var info));
            return info;
        } finally {
            foreach (var p in pointers)
                if (p != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(p);
        }
    }
}
}
