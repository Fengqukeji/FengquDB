using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace FengquDB {
public sealed class StoreOptions {
    public bool Buffered { get; set; }
    public bool Create { get; set; } = true;
    public bool Segmented { get; set; }
    public ulong CacheBytes { get; set; } = 64UL * 1024 * 1024;
    public ulong DeltaBytes { get; set; } = 4UL * 1024 * 1024;
    public ulong WriterBytes { get; set; } = 32UL * 1024 * 1024;
    public ulong TransactionBytes { get; set; } = 64UL * 1024 * 1024;
    public ulong AllTransactionBytes { get; set; } = 256UL * 1024 * 1024;
    public ulong PinnedBytes { get; set; } = 128UL * 1024 * 1024;
    public ulong MaxSnapshots { get; set; } = 1024;
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreInfo {
    public ulong Version, DurableVersion, FileBytes, IndexedKeys, StoredKeys, StoredValueBytes;
    public ulong DeltaBytes, CacheBytes, PinnedBytes, TransactionBytes, RecoveredTransactions,
        RepairedBytes;
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreMaintenanceInfo {
    public ulong SnapshotVersion, FinalVersion, SourceFileBytes, OutputFileBytes;
    public ulong StoredKeys, StoredValueBytes, CopiedKeys, CatchupRounds;
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreExpiryInfo {
    public ulong Version, CutoffMs, RemovedKeys, RemovedValueBytes, VisitedPages, More;
}
public sealed class StoreExpiryOptions {
    public uint MaxKeys { get; set; } = 1024;
    public ulong TimeoutMs { get; set; }
    public ulong MaxBatchBytes { get; set; } = 1048576;
    internal StoreNativeExpiryOptions Native() => new StoreNativeExpiryOptions {
        Size = (uint)Marshal.SizeOf<StoreNativeExpiryOptions>(), Abi = 1, Keys = MaxKeys,
        Timeout = TimeoutMs, Batch = MaxBatchBytes
    };
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeExpiryOptions {
    internal uint Size, Abi, Keys, Reserved;
    internal ulong Timeout, Batch;
}
public sealed class StoreMaintenanceOptions {
    public uint MaxCatchupRounds { get; set; } = 8;
    public ulong TimeoutMs { get; set; }
    public ulong MaxBarrierBytes { get; set; } = 1048576;
    public ulong MaxReadBytes { get; set; }
    public ulong MaxWriteBytes { get; set; }
    public StoreMaintenanceIO? IOStats { get; set; }
    internal StoreMaintenanceLease Lease() => new StoreMaintenanceLease(this);
}
[StructLayout(LayoutKind.Sequential)]
public sealed class StoreMaintenanceIO {
    public ulong ReadBytes, WriteBytes, SyncCalls;
}
internal struct StoreMaintenanceLease : IDisposable {
    GCHandle pin;
    internal StoreNativeMaintenanceOptions Options;
    internal StoreMaintenanceLease(StoreMaintenanceOptions options) {
        pin = default;
        Options = new StoreNativeMaintenanceOptions {
            Size = (uint)Marshal.SizeOf<StoreNativeMaintenanceOptions>(),
            Abi = options.MaxReadBytes != 0 || options.MaxWriteBytes != 0 || options.IOStats != null
                      ? 2u
                      : 1u,
            Rounds = options.MaxCatchupRounds,
            Timeout = options.TimeoutMs,
            Barrier = options.MaxBarrierBytes,
            MaxRead = options.MaxReadBytes,
            MaxWrite = options.MaxWriteBytes
        };
        if (options.IOStats != null) {
            pin = GCHandle.Alloc(options.IOStats, GCHandleType.Pinned);
            try {
                Options.Stats = pin.AddrOfPinnedObject();
            } catch {
                pin.Free();
                throw;
            }
        }
    }
    public void Dispose() {
        if (pin.IsAllocated)
            pin.Free();
    }
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeMaintenanceOptions {
    internal uint Size, Abi, Rounds, Reserved;
    internal ulong Timeout, Barrier;
    internal ulong MaxRead, MaxWrite;
    internal IntPtr Stats;
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeOptions {
    internal uint Size, Abi, Durability, Flags;
    internal ulong Cache, Delta, Writer, Transaction, AllTransactions, Pins, Snapshots;
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeRow {
    internal Slice Key, Value;
    internal ulong Version, ExpiresAtMs;
}
public sealed class StoreRow {
    public byte[] Key { get; }
    public byte[] Value { get; }
    public ulong Version { get; }
    public ulong ExpiresAtMs { get; }
    internal StoreRow(StoreNativeRow row) {
        Key = Native.Copy(row.Key);
        Value = Native.Copy(row.Value);
        Version = row.Version;
        ExpiresAtMs = row.ExpiresAtMs;
    }
}
internal sealed class StoreHandle : SafeHandleZeroOrMinusOneIsInvalid {
    readonly int kind;
    internal StoreHandle(IntPtr pointer, int kind) : base(true) {
        this.kind = kind;
        SetHandle(pointer);
    }
    protected override bool ReleaseHandle() {
        switch (kind) {
        case 0:
            StoreNative.fqdb_store_close(handle);
            break;
        case 1:
            StoreNative.fqdb_store_txn_close(handle);
            break;
        case 2:
            StoreNative.fqdb_store_snapshot_close(handle);
            break;
        case 3:
            StoreNative.fqdb_store_cursor_close(handle);
            break;
        case 4:
            StoreNative.fqdb_store_key_close(handle);
            break;
        case 5:
            StoreNative.fqdb_store_batch_close(handle);
            break;
        case 6:
            StoreNative.fqdb_store_watch_close(handle);
            break;
        case 7:
            StoreNative.fqdb_store_reader_close(handle);
            break;
        case 8:
            StoreNative.fqdb_store_commit_close(handle);
            break;
        }
        return true;
    }
}
internal static partial class StoreNative {
#if UNITY_IOS && !UNITY_EDITOR
    const string Lib = "__Internal";
#else
    const string Lib = "fengqudb";
#endif
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_open([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                               ref StoreNativeOptions options, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_close(IntPtr value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_get_info(StoreHandle value, out StoreInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_sync(StoreHandle value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_checkpoint(StoreHandle value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_compact(StoreHandle value,
                                                  ref StoreNativeMaintenanceOptions options,
                                                  out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_backup(StoreHandle value,
                                                 [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                                 ref StoreNativeMaintenanceOptions options,
                                                 out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_verify(StoreHandle value,
                                                 ref StoreNativeMaintenanceOptions options,
                                                 out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int
    fqdb_store_restore([MarshalAs(UnmanagedType.LPUTF8Str)] string source,
                       [MarshalAs(UnmanagedType.LPUTF8Str)] string destination,
                       ref StoreNativeMaintenanceOptions options, out StoreMaintenanceInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_cancel_maintenance(StoreHandle value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_purge_expired(StoreHandle value,
                                                        ref StoreNativeExpiryOptions options,
                                                        out StoreExpiryInfo info);
    internal static string Path(string path) {
        if (String.IsNullOrEmpty(path) || path.IndexOf('\0') >= 0)
            throw new ArgumentException("Invalid directory", nameof(path));
        return path;
    }
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_get(StoreHandle value, Slice key, out Slice output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_snapshot_create(StoreHandle value, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_snapshot_get(StoreHandle value, Slice key,
                                                       out Slice output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_snapshot_close(IntPtr value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_begin(StoreHandle value, ulong timeout,
                                                    out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_get(StoreHandle value, Slice key, out Slice output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_put(StoreHandle value, Slice key, Slice bytes,
                                                  ulong expires);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_delete(StoreHandle value, Slice key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_expire(StoreHandle value, Slice key, ulong expires);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_patch(StoreHandle value, Slice key, ulong offset,
                                                    ulong remove, Slice insert);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_savepoint(StoreHandle value, out ulong id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_rollback_to(StoreHandle value, ulong id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_release(StoreHandle value, ulong id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_txn_cancel(StoreHandle value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_commit(StoreHandle value, out ulong version,
                                                     uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_txn_close(IntPtr value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_cursor_open(StoreHandle value, Slice begin, Slice end,
                                                      uint reverse, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_cursor_open(StoreHandle value, Slice begin, Slice end,
                                                          uint reverse, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_cursor_next(StoreHandle value, out StoreNativeRow row);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_cursor_close(IntPtr value);
    internal static unsafe byte[]? Read(StoreHandle handle, byte[] key, int kind) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        fixed(byte *pointer = key) {
            var slice = new Slice((IntPtr)pointer, (ulong)key.Length);
            // Keep the owned byte[] contract, but copy directly from immutable
            // storage once. A full pin budget must not break ordinary copied Get.
            if (kind != 1) {
                int sharedStatus = kind == 0
                                       ? fqdb_store_get_shared(handle, slice, out var shared)
                                       : fqdb_store_snapshot_get_shared(handle, slice, out shared);
                if (sharedStatus == 1)
                    return null;
                if (sharedStatus != 9) {
                    Native.Check(sharedStatus);
                    try {
                        return Native.Copy(shared.Row.Value);
                    } finally {
                        fqdb_store_value_close(shared.Owner);
                    }
                }
            }
            Slice output;
            int status = kind == 0   ? fqdb_store_get(handle, slice, out output)
                         : kind == 1 ? fqdb_store_txn_get(handle, slice, out output)
                                     : fqdb_store_snapshot_get(handle, slice, out output);
            return Native.Read(status, output);
        }
    }
    internal static unsafe IEnumerable<StoreRow> Scan(StoreHandle handle, byte[]? begin,
                                                      byte[]? end, bool reverse, bool transaction) {
        var low = begin ?? Array.Empty<byte>();
        var high = end ?? Array.Empty<byte>();
        IntPtr output;
        fixed(byte *l = low, h = high) {
            var a = new Slice((IntPtr)l, (ulong)low.Length);
            var b = new Slice((IntPtr)h, (ulong)high.Length);
            Native.Check(
                transaction
                    ? fqdb_store_txn_cursor_open(handle, a, b, reverse ? 1u : 0u, out output)
                    : fqdb_store_cursor_open(handle, a, b, reverse ? 1u : 0u, out output));
        }
        return Rows(new StoreHandle(output, 3));
    }
    static IEnumerable<StoreRow> Rows(StoreHandle cursor) {
        using (cursor) {
            while (true) {
                int status = fqdb_store_cursor_next(cursor, out var row);
                if (status == 2)
                    yield break;
                Native.Check(status);
                yield return new StoreRow(row);
            }
        }
    }
}
/// <summary>Paged database directory. Managed reads return owned byte arrays.</summary>
public sealed partial class Store : IDisposable {
    readonly StoreHandle handle;
    public Store(string directory, StoreOptions? options = null) {
        if (String.IsNullOrEmpty(directory) || directory.IndexOf('\0') >= 0)
            throw new ArgumentException("Invalid database directory", nameof(directory));
        var o = options ?? new StoreOptions();
        var native =
            new StoreNativeOptions { Size = (uint)Marshal.SizeOf<StoreNativeOptions>(),
                                     Abi = 1,
                                     Durability = o.Buffered ? 1u : 0u,
                                     Flags = (o.Create ? 1u : 0u) | (o.Segmented ? 2u : 0u),
                                     Cache = o.CacheBytes,
                                     Delta = o.DeltaBytes,
                                     Writer = o.WriterBytes,
                                     Transaction = o.TransactionBytes,
                                     AllTransactions = o.AllTransactionBytes,
                                     Pins = o.PinnedBytes,
                                     Snapshots = o.MaxSnapshots };
        Native.Check(StoreNative.fqdb_store_open(directory, ref native, out var pointer));
        handle = new StoreHandle(pointer, 0);
    }
    public byte[]? Get(byte[] key) => StoreNative.Read(handle, key, 0);
    public StoreInfo Info() {
        Native.Check(StoreNative.fqdb_store_get_info(handle, out var info));
        return info;
    }
    public void Sync() => Native.Check(StoreNative.fqdb_store_sync(handle));
    public void Checkpoint() => Native.Check(StoreNative.fqdb_store_checkpoint(handle));
    public StoreMaintenanceInfo Compact(StoreMaintenanceOptions? options = null) {
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        Native.Check(StoreNative.fqdb_store_compact(handle, ref native, out var info));
        return info;
    }
    public StoreMaintenanceInfo Backup(string destination,
                                       StoreMaintenanceOptions? options = null) {
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        Native.Check(StoreNative.fqdb_store_backup(handle, StoreNative.Path(destination),
                                                   ref native, out var info));
        return info;
    }
    public StoreMaintenanceInfo Verify(StoreMaintenanceOptions? options = null) {
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        Native.Check(StoreNative.fqdb_store_verify(handle, ref native, out var info));
        return info;
    }
    public static StoreMaintenanceInfo Restore(string source, string destination,
                                               StoreMaintenanceOptions? options = null) {
        using var lease = (options ?? new StoreMaintenanceOptions()).Lease();
        var native = lease.Options;
        Native.Check(StoreNative.fqdb_store_restore(
            StoreNative.Path(source), StoreNative.Path(destination), ref native, out var info));
        return info;
    }
    public StoreExpiryInfo PurgeExpired(StoreExpiryOptions? options = null) {
        var native = (options ?? new StoreExpiryOptions()).Native();
        Native.Check(StoreNative.fqdb_store_purge_expired(handle, ref native, out var info));
        return info;
    }
    public void CancelMaintenance() => StoreNative.fqdb_store_cancel_maintenance(handle);
    public StoreSnapshot Snapshot() {
        Native.Check(StoreNative.fqdb_store_snapshot_create(handle, out var pointer));
        return new StoreSnapshot(new StoreHandle(pointer, 2));
    }
    public StoreTransaction Begin(ulong timeoutMs = 0) {
        Native.Check(StoreNative.fqdb_store_txn_begin(handle, timeoutMs, out var pointer));
        return new StoreTransaction(new StoreHandle(pointer, 1));
    }
    public void Dispose() => handle.Dispose();
}
public sealed partial class StoreSnapshot : IDisposable {
    readonly StoreHandle handle;
    internal StoreSnapshot(StoreHandle handle) {
        this.handle = handle;
    }
    public byte[]? Get(byte[] key) => StoreNative.Read(handle, key, 2);
    public IEnumerable<StoreRow> Scan(byte[]? begin = null, byte[]? end = null,
                                      bool reverse = false) => StoreNative.Scan(handle, begin, end,
                                                                                reverse, false);
    public void Dispose() => handle.Dispose();
}
public sealed partial class StoreTransaction : IDisposable {
    readonly StoreHandle handle;
    readonly object gate = new object();
    internal StoreTransaction(StoreHandle handle) {
        this.handle = handle;
    }
    public byte[]? Get(byte[] key) {
        lock (gate) return StoreNative.Read(handle, key, 1);
    }
    public unsafe void Put(byte[] key, byte[] value, ulong expiresAtMs = 0) {
        if (key == null || value == null)
            throw new ArgumentNullException();
        lock (gate) fixed(byte *k = key, v = value) Native.Check(
            StoreNative.fqdb_store_txn_put(handle, new Slice((IntPtr)k, (ulong)key.Length),
                                           new Slice((IntPtr)v, (ulong)value.Length), expiresAtMs));
    }
    public unsafe void Delete(byte[] key) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        lock (gate) fixed(byte *k = key) Native.Check(
            StoreNative.fqdb_store_txn_delete(handle, new Slice((IntPtr)k, (ulong)key.Length)));
    }
    public unsafe void Expire(byte[] key, ulong expiresAtMs) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        lock (gate) fixed(byte *k = key) Native.Check(StoreNative.fqdb_store_txn_expire(
            handle, new Slice((IntPtr)k, (ulong)key.Length), expiresAtMs));
    }
    public unsafe void Patch(byte[] key, ulong offset, ulong remove, byte[] insert) {
        if (key == null || insert == null)
            throw new ArgumentNullException();
        lock (gate) fixed(byte *k = key, i = insert) Native.Check(StoreNative.fqdb_store_txn_patch(
            handle, new Slice((IntPtr)k, (ulong)key.Length), offset, remove,
            new Slice((IntPtr)i, (ulong)insert.Length)));
    }
    public ulong Savepoint() {
        lock (gate) {
            Native.Check(StoreNative.fqdb_store_txn_savepoint(handle, out var id));
            return id;
        }
    }
    public void RollbackTo(ulong id) {
        lock (gate) Native.Check(StoreNative.fqdb_store_txn_rollback_to(handle, id));
    }
    public void Release(ulong id) {
        lock (gate) Native.Check(StoreNative.fqdb_store_txn_release(handle, id));
    }
    public void Cancel() => StoreNative.fqdb_store_txn_cancel(handle);
    public ulong Commit(bool forceReliable = false) {
        lock (gate) {
            Native.Check(StoreNative.fqdb_store_txn_commit(handle, out var version,
                                                           forceReliable ? 1u : 0u));
            return version;
        }
    }
    public IEnumerable<StoreRow> Scan(byte[]? begin = null, byte[]? end = null,
                                      bool reverse = false) {
        lock (gate) return StoreNative.Scan(handle, begin, end, reverse, true);
    }
    public void Dispose() => handle.Dispose();
}
}
