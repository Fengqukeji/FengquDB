using System;
using System.Runtime.InteropServices;

namespace FengquDB {
public sealed class StoreWatchOptions {
    public bool MetadataOnly { get; set; }
    public bool IncludeSystem { get; set; }
    public uint MaxScanCommits { get; set; } = 256;
    public ulong MaxBatchBytes { get; set; } = 67108864;
    internal StoreNativeWatchOptions Native() => new StoreNativeWatchOptions {
        Size = (uint)Marshal.SizeOf<StoreNativeWatchOptions>(), Abi = 1,
        Flags = (MetadataOnly ? 1u : 0u) | (IncludeSystem ? 2u : 0u), Scans = MaxScanCommits,
        Bytes = MaxBatchBytes
    };
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeWatchOptions {
    internal uint Size, Abi, Flags, Scans;
    internal ulong Bytes;
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeChange {
    internal Slice Key, Value;
    internal ulong Expires;
    internal uint Kind, Reserved;
}
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct StoreNativeChanges {
    internal ulong Version, UnixMs, DurableVersion;
    internal uint Count, Flags;
    internal StoreNativeChange *Rows;
}
public sealed class StoreChange {
    public byte[] Key { get; }
    public byte[]? Value { get; }
    public uint Kind { get; }
    public ulong ExpiresAtMs { get; }
    internal StoreChange(StoreNativeChange row, uint flags) {
        Key = FengquDB.Native.Copy(row.Key);
        Value = row.Kind == 2 || (flags & 1) != 0 ? null : FengquDB.Native.Copy(row.Value);
        Kind = row.Kind;
        ExpiresAtMs = row.Expires;
    }
}
public sealed class StoreChanges {
    public ulong Version { get; }
    public ulong UnixMs { get; }
    public ulong DurableVersion { get; }
    public uint Flags { get; }
    public StoreChange[] Rows { get; }
    internal unsafe StoreChanges(StoreNativeChanges data) {
        Version = data.Version;
        UnixMs = data.UnixMs;
        DurableVersion = data.DurableVersion;
        Flags = data.Flags;
        Rows = new StoreChange[checked((int)data.Count)];
        for (int i = 0; i < Rows.Length; ++i)
            Rows[i] = new StoreChange(data.Rows[i], Flags);
    }
}
public sealed class StoreWatch : IDisposable {
    readonly StoreHandle handle;
    internal StoreWatch(IntPtr pointer) {
        handle = new StoreHandle(pointer, 6);
    }
    public unsafe StoreChanges? Next(ulong timeoutMs = 0) {
        int status = StoreNative.fqdb_store_watch_next(handle, timeoutMs, out var batch);
        if (status == 17 || status == 18)
            return null;
        Native.Check(status);
        try {
            return new StoreChanges(
                *(StoreNativeChanges *)StoreNative.fqdb_store_change_batch_data(batch));
        } finally {
            StoreNative.fqdb_store_change_batch_close(batch);
        }
    }
    public void Ack(ulong version) => Native.Check(StoreNative.fqdb_store_watch_ack(handle,
                                                                                    version));
    public void Cancel() => StoreNative.fqdb_store_watch_cancel(handle);
    public void Dispose() {
        try {
            if (!handle.IsClosed)
                Cancel();
        } catch (ObjectDisposedException) {
        } finally {
            handle.Dispose();
        }
    }
}
internal static partial class StoreNative {
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_watch_open(StoreHandle store, ulong from, Slice prefix,
                                                     ref StoreNativeWatchOptions options,
                                                     out IntPtr watch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_watch_next(StoreHandle watch, ulong timeout,
                                                     out IntPtr batch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_watch_ack(StoreHandle watch, ulong version);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_watch_cancel(StoreHandle watch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_watch_close(IntPtr watch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr fqdb_store_change_batch_data(IntPtr batch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_change_batch_close(IntPtr batch);
}
public sealed partial class Store {
    public unsafe StoreWatch Watch(ulong fromVersion = ulong.MaxValue, byte[]? prefix = null,
                                   StoreWatchOptions? options = null) {
        prefix = prefix ?? Array.Empty<byte>();
        var settings = (options ?? new StoreWatchOptions()).Native();
        fixed(byte *key = prefix) {
            Native.Check(StoreNative.fqdb_store_watch_open(
                handle, fromVersion, new Slice((IntPtr)key, (ulong)prefix.Length), ref settings,
                out var watch));
            try {
                return new StoreWatch(watch);
            } catch {
                StoreNative.fqdb_store_watch_close(watch);
                throw;
            }
        }
    }
}
}
