using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace FengquDB {
[StructLayout(LayoutKind.Sequential)]
public struct StoreAtomicInfo {
    public ulong Version, ObservedVersion;
    public long Value;
    uint matched, changed;
    public bool Matched => matched != 0;
    public bool Changed => changed != 0;
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreHistoryInfo {
    public ulong Version, Floor, OldestPin, ActivePins, TotalPins, RetainedGenerations,
        RetainedFileBytes;
}
public sealed class StoreEntry {
    public byte[] Value { get; }
    public ulong Version { get; }
    public ulong ExpiresAtMs { get; }
    internal StoreEntry(byte[] value, ulong version, ulong expires) {
        Value = value;
        Version = version;
        ExpiresAtMs = expires;
    }
}
public sealed class StorePut {
    public byte[] Key { get; }
    public byte[] Value { get; }
    public ulong ExpiresAtMs { get; }
    public StorePut(byte[] key, byte[] value, ulong expiresAtMs = 0) {
        Key = key ?? throw new ArgumentNullException(nameof(key));
        Value = value ?? throw new ArgumentNullException(nameof(value));
        ExpiresAtMs = expiresAtMs;
    }
}
public sealed class StoreI64FieldUpdate {
    public byte[] Key { get; }
    public ulong Offset { get; }
    public long Delta { get; }
    public long Minimum { get; }
    public long Maximum { get; }
    public StoreI64FieldUpdate(byte[] key, ulong offset, long delta,
                               long minimum = long.MinValue, long maximum = long.MaxValue) {
        Key = key ?? throw new ArgumentNullException(nameof(key));
        if (minimum > maximum)
            throw new ArgumentException("Invalid field bounds");
        Offset = offset;
        Delta = delta;
        Minimum = minimum;
        Maximum = maximum;
    }
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeI64FieldUpdate {
    internal Slice Key;
    internal ulong Offset;
    internal long Delta, Minimum, Maximum;
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativePut {
    internal Slice Key, Value;
    internal ulong Expires;
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeResult {
    internal Slice Value;
    internal ulong Version, Expires;
    internal uint Found, Reserved;
}
internal sealed class StorePins : IDisposable {
    readonly List<GCHandle> pins;
    internal readonly Slice[] Keys;
    internal readonly StoreNativePut[] Items;
    internal readonly StoreNativeI64FieldUpdate[] FieldUpdates = Array.Empty<StoreNativeI64FieldUpdate>();
    Slice Pin(byte[] value) {
        if (value == null)
            throw new ArgumentNullException(nameof(value));
        if (value.Length == 0)
            return new Slice(IntPtr.Zero, 0);
        var pin = GCHandle.Alloc(value, GCHandleType.Pinned);
        pins.Add(pin);
        return new Slice(pin.AddrOfPinnedObject(), (ulong)value.Length);
    }
    internal StorePins(byte[][] keys) {
        if (keys == null || keys.Length > 4096)
            throw new ArgumentException("Invalid keys");
        pins = new List<GCHandle>(keys.Length);
        Keys = new Slice[keys.Length];
        Items = Array.Empty<StoreNativePut>();
        try {
            for (int i = 0; i < keys.Length; i++)
                Keys[i] = Pin(keys[i]);
        } catch {
            Dispose();
            throw;
        }
    }
    internal StorePins(StorePut[] items) {
        if (items == null || items.Length > 4096)
            throw new ArgumentException("Invalid items");
        pins = new List<GCHandle>(items.Length * 2);
        Items = new StoreNativePut[items.Length];
        Keys = Array.Empty<Slice>();
        try {
            for (int i = 0; i < items.Length; i++) {
                var item = items[i] ?? throw new ArgumentException("Null item");
                Items[i] = new StoreNativePut { Key = Pin(item.Key), Value = Pin(item.Value),
                                                Expires = item.ExpiresAtMs };
            }
        } catch {
            Dispose();
            throw;
        }
    }
    internal StorePins(StoreI64FieldUpdate[] items) {
        if (items == null || items.Length == 0 || items.Length > 256)
            throw new ArgumentException("Expected 1..256 field updates");
        pins = new List<GCHandle>(items.Length);
        Items = Array.Empty<StoreNativePut>();
        Keys = Array.Empty<Slice>();
        FieldUpdates = new StoreNativeI64FieldUpdate[items.Length];
        try {
            for (int i = 0; i < items.Length; i++) {
                var item = items[i] ?? throw new ArgumentException("Null item");
                FieldUpdates[i] = new StoreNativeI64FieldUpdate {
                    Key = Pin(item.Key), Offset = item.Offset, Delta = item.Delta,
                    Minimum = item.Minimum, Maximum = item.Maximum
                };
            }
        } catch {
            Dispose();
            throw;
        }
    }
    public void Dispose() {
        foreach (var pin in pins)
            if (pin.IsAllocated)
                pin.Free();
        pins.Clear();
    }
}
internal static partial class StoreNative {
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_history_hold(StoreHandle h, Slice name, ulong from,
                                                       ulong timeout, out StoreAtomicInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_history_release(StoreHandle h, Slice name, ulong timeout,
                                                          out StoreAtomicInfo info);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_history_info_get(StoreHandle h,
                                                           out StoreHistoryInfo info);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_path_at([MarshalAs(UnmanagedType.LPUTF8Str)] string root,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
                                                  out Slice output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_open_at([MarshalAs(UnmanagedType.LPUTF8Str)] string root,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
                                                  ref StoreNativeOptions options,
                                                  out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_get_view(StoreHandle store, Slice key, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong fqdb_store_value_version(IntPtr value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong fqdb_store_value_expires_at_ms(IntPtr value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_compare_exchange(StoreHandle store, Slice key,
                                                           ulong expected, Slice value,
                                                           ulong expires, ulong timeout,
                                                           out StoreAtomicInfo info,
                                                           uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_delete_if_version(StoreHandle store, Slice key,
                                                            ulong expected, ulong timeout,
                                                            out StoreAtomicInfo info,
                                                            uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_put_if_changed(StoreHandle store, Slice key, Slice value,
                                                         ulong expires, ulong timeout,
                                                         out StoreAtomicInfo info,
                                                         uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_add_i64(StoreHandle store, Slice key, long delta,
                                                  long minimum, long maximum, ulong timeout,
                                                  out StoreAtomicInfo info, uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_add_i64_fields(
        StoreHandle store, [In] StoreNativeI64FieldUpdate[] items, uint count, ulong timeout,
        out StoreAtomicInfo info, uint forceReliable);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_get_many(StoreHandle store, [In] Slice[] keys, uint count,
                                                   out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_snapshot_get_many(StoreHandle store, [In] Slice[] keys,
                                                            uint count, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_get_many(StoreHandle store, [In] Slice[] keys,
                                                       uint count, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr fqdb_store_results_data(IntPtr results);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_results_close(IntPtr results);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_put_many(StoreHandle store,
                                                       [In] StoreNativePut[] items, uint count);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_delete_many(StoreHandle store, [In] Slice[] keys,
                                                          uint count);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_batch_create(StoreHandle store,
                                                       [In] StoreNativePut[] items, uint count,
                                                       out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_batch_close(IntPtr batch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_put_batch(StoreHandle txn, StoreHandle batch);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_key_prepare(StoreHandle store, Slice key,
                                                      out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_key_close(IntPtr key);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_key_get_view(StoreHandle key, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_key_get_into(StoreHandle key, IntPtr destination,
                                                       ulong capacity, out ulong required);
    internal static StoreEntry Entry(StoreNativeShared value) =>
        new StoreEntry(Native.Copy(value.Row.Value), value.Row.Version, value.Row.Expires);
    internal static unsafe StoreEntry? GetEntry(StoreHandle handle, byte[] key) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        fixed(byte *k = key) {
            int status = fqdb_store_get_shared(handle, new Slice((IntPtr)k, (ulong)key.Length),
                                               out var value);
            if (status == 1)
                return null;
            Native.Check(status);
            try {
                return Entry(value);
            } finally {
                fqdb_store_value_close(value.Owner);
            }
        }
    }
    internal static unsafe StoreEntry?[] GetMany(StoreHandle handle, byte[][] keys, int kind) {
        using (var input = new StorePins(keys)) {
            IntPtr output;
            Native.Check(
                kind == 0
                    ? fqdb_store_get_many(handle, input.Keys, (uint)input.Keys.Length, out output)
                : kind == 1 ? fqdb_store_txn_get_many(handle, input.Keys, (uint)input.Keys.Length,
                                                      out output)
                            : fqdb_store_snapshot_get_many(handle, input.Keys,
                                                           (uint)input.Keys.Length, out output));
            try {
                var rows = (StoreNativeResult *)fqdb_store_results_data(output);
                var result = new StoreEntry?[input.Keys.Length];
                for (int i = 0; i < result.Length; i++) {
                    var row = rows[i];
                    if (row.Found != 0)
                        result[i] =
                            new StoreEntry(Native.Copy(row.Value), row.Version, row.Expires);
                }
                return result;
            } finally {
                fqdb_store_results_close(output);
            }
        }
    }
    internal static unsafe byte[]?[] GetManyValues(StoreHandle handle, byte[][] keys, int kind) {
        using (var input = new StorePins(keys)) {
            IntPtr output;
            Native.Check(
                kind == 0
                    ? fqdb_store_get_many(handle, input.Keys, (uint)input.Keys.Length, out output)
                : kind == 1 ? fqdb_store_txn_get_many(handle, input.Keys, (uint)input.Keys.Length,
                                                      out output)
                            : fqdb_store_snapshot_get_many(handle, input.Keys,
                                                           (uint)input.Keys.Length, out output));
            try {
                var rows = (StoreNativeResult *)fqdb_store_results_data(output);
                var result = new byte[]?[input.Keys.Length];
                for (int i = 0; i < result.Length; i++) {
                    var row = rows[i];
                    if (row.Found != 0)
                        result[i] = Native.Copy(row.Value);
                }
                return result;
            } finally {
                fqdb_store_results_close(output);
            }
        }
    }
}
public sealed class StorePreparedKey : IDisposable {
    readonly StoreHandle handle;
    internal StorePreparedKey(IntPtr pointer) {
        handle = new StoreHandle(pointer, 4);
    }
    public StoreEntry? GetEntry() {
        int status = StoreNative.fqdb_store_key_get_shared(handle, out var output);
        if (status == 1)
            return null;
        Native.Check(status);
        try {
            return StoreNative.Entry(output);
        } finally {
            StoreNative.fqdb_store_value_close(output.Owner);
        }
    }
    public byte[]? Get() {
        int status = StoreNative.fqdb_store_key_get_shared(handle, out var output);
        if (status == 1)
            return null;
        Native.Check(status);
        try {
            return Native.Copy(output.Row.Value);
        } finally {
            StoreNative.fqdb_store_value_close(output.Owner);
        }
    }
    public unsafe ulong? GetInto(byte[] destination) {
        if (destination == null)
            throw new ArgumentNullException(nameof(destination));
        fixed(byte *p = destination) {
            int status = StoreNative.fqdb_store_key_get_into(
                handle, (IntPtr)p, (ulong)destination.Length, out var required);
            if (status == 1)
                return null;
            Native.Check(status);
            return required;
        }
    }
    public void Dispose() => handle.Dispose();
}
public sealed class StorePreparedBatch : IDisposable {
    internal readonly StoreHandle Handle;
    internal StorePreparedBatch(IntPtr pointer) {
        Handle = new StoreHandle(pointer, 5);
    }
    public void Dispose() => Handle.Dispose();
}
public sealed partial class Store {
    public unsafe StoreAtomicInfo HoldHistory(byte[] name, ulong fromVersion = ulong.MaxValue,
                                              ulong timeoutMs = 0) {
        if (name == null)
            throw new ArgumentNullException(nameof(name));
        fixed(byte *n = name) {
            Native.Check(StoreNative.fqdb_store_history_hold(
                handle, new Slice((IntPtr)n, (ulong)name.Length), fromVersion, timeoutMs,
                out var info));
            return info;
        }
    }
    public unsafe StoreAtomicInfo ReleaseHistory(byte[] name, ulong timeoutMs = 0) {
        if (name == null)
            throw new ArgumentNullException(nameof(name));
        fixed(byte *n = name) {
            Native.Check(StoreNative.fqdb_store_history_release(
                handle, new Slice((IntPtr)n, (ulong)name.Length), timeoutMs, out var info));
            return info;
        }
    }
    public StoreHistoryInfo HistoryInfo() {
        Native.Check(StoreNative.fqdb_store_history_info_get(handle, out var info));
        return info;
    }

    Store(IntPtr pointer) {
        handle = new StoreHandle(pointer, 0);
    }
    public static string PathAt(string root, string name) {
        StoreNative.Path(root);
        StoreNative.Path(name);
        Native.Check(StoreNative.fqdb_store_path_at(root, name, out var output));
        var bytes = Native.Read(0, output)!;
        return System.Text.Encoding.UTF8.GetString(bytes);
    }
    public static Store OpenAt(string root, string name, StoreOptions? options = null) {
        StoreNative.Path(root);
        StoreNative.Path(name);
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
        Native.Check(StoreNative.fqdb_store_open_at(root, name, ref native, out var pointer));
        try {
            return new Store(pointer);
        } catch {
            StoreNative.fqdb_store_close(pointer);
            throw;
        }
    }
    public StoreEntry? GetEntry(byte[] key) => StoreNative.GetEntry(handle, key);
    public byte[]?[] GetMany(byte[][] keys) => StoreNative.GetManyValues(handle, keys, 0);
    public StoreEntry?[] GetEntries(byte[][] keys) => StoreNative.GetMany(handle, keys, 0);
    public unsafe StorePreparedKey PrepareKey(byte[] key) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        fixed(byte *k = key) {
            Native.Check(StoreNative.fqdb_store_key_prepare(
                handle, new Slice((IntPtr)k, (ulong)key.Length), out var output));
            try {
                return new StorePreparedKey(output);
            } catch {
                StoreNative.fqdb_store_key_close(output);
                throw;
            }
        }
    }
    public StorePreparedBatch PrepareBatch(StorePut[] items) {
        using (var input = new StorePins(items)) {
            Native.Check(StoreNative.fqdb_store_batch_create(
                handle, input.Items, (uint)input.Items.Length, out var output));
            try {
                return new StorePreparedBatch(output);
            } catch {
                StoreNative.fqdb_store_batch_close(output);
                throw;
            }
        }
    }
    public unsafe StoreAtomicInfo CompareExchange(byte[] key, ulong expectedVersion, byte[] value,
                                                  ulong expiresAtMs = 0, ulong timeoutMs = 0,
                                                  bool forceReliable = false) {
        if (key == null || value == null)
            throw new ArgumentNullException();
        fixed(byte *k = key, v = value) {
            Native.Check(StoreNative.fqdb_store_compare_exchange(
                handle, new Slice((IntPtr)k, (ulong)key.Length), expectedVersion,
                new Slice((IntPtr)v, (ulong)value.Length), expiresAtMs, timeoutMs, out var info,
                forceReliable ? 1u : 0u));
            return info;
        }
    }
    public StoreAtomicInfo PutIfAbsent(byte[] key, byte[] value, ulong expiresAtMs = 0,
                                       ulong timeoutMs = 0, bool forceReliable = false) =>
        CompareExchange(key, 0, value, expiresAtMs, timeoutMs, forceReliable);
    public unsafe StoreAtomicInfo PutIfChanged(byte[] key, byte[] value, ulong expiresAtMs = 0,
                                               ulong timeoutMs = 0, bool forceReliable = false) {
        if (key == null || value == null)
            throw new ArgumentNullException();
        fixed(byte *k = key, v = value) {
            Native.Check(StoreNative.fqdb_store_put_if_changed(
                handle, new Slice((IntPtr)k, (ulong)key.Length),
                new Slice((IntPtr)v, (ulong)value.Length), expiresAtMs, timeoutMs, out var info,
                forceReliable ? 1u : 0u));
            return info;
        }
    }
    public unsafe StoreAtomicInfo DeleteIfVersion(byte[] key, ulong expectedVersion,
                                                  ulong timeoutMs = 0, bool forceReliable = false) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        fixed(byte *k = key) {
            Native.Check(StoreNative.fqdb_store_delete_if_version(
                handle, new Slice((IntPtr)k, (ulong)key.Length), expectedVersion, timeoutMs,
                out var info, forceReliable ? 1u : 0u));
            return info;
        }
    }
    public unsafe StoreAtomicInfo AddInt64(byte[] key, long delta, long minimum = long.MinValue,
                                           long maximum = long.MaxValue, ulong timeoutMs = 0,
                                           bool forceReliable = false) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        fixed(byte *k = key) {
            Native.Check(StoreNative.fqdb_store_add_i64(
                handle, new Slice((IntPtr)k, (ulong)key.Length), delta, minimum, maximum, timeoutMs,
                out var info, forceReliable ? 1u : 0u));
            return info;
        }
    }
    public StoreAtomicInfo AddInt64Fields(StoreI64FieldUpdate[] items, ulong timeoutMs = 0,
                                          bool forceReliable = false) {
        using (var pins = new StorePins(items)) {
            Native.Check(StoreNative.fqdb_store_add_i64_fields(
                handle, pins.FieldUpdates, (uint)pins.FieldUpdates.Length, timeoutMs,
                out var info, forceReliable ? 1u : 0u));
            return info;
        }
    }
    public ulong PutMany(StorePut[] items, bool forceReliable = false) {
        using (var tx = Begin()) {
            tx.PutMany(items);
            return tx.Commit(forceReliable);
        }
    }
    public ulong DeleteMany(byte[][] keys, bool forceReliable = false) {
        using (var tx = Begin()) {
            tx.DeleteMany(keys);
            return tx.Commit(forceReliable);
        }
    }
    public ulong PutBatch(StorePreparedBatch batch, bool forceReliable = false) {
        using (var tx = Begin()) {
            tx.PutBatch(batch);
            return tx.Commit(forceReliable);
        }
    }
}
public sealed partial class StoreSnapshot {
    public byte[]?[] GetMany(byte[][] keys) => StoreNative.GetManyValues(handle, keys, 2);
    public StoreEntry?[] GetEntries(byte[][] keys) => StoreNative.GetMany(handle, keys, 2);
}
public sealed partial class StoreTransaction {
    public byte[]?[] GetMany(byte[][] keys) {
        lock (gate) return StoreNative.GetManyValues(handle, keys, 1);
    }
    public StoreEntry?[] GetEntries(byte[][] keys) {
        lock (gate) return StoreNative.GetMany(handle, keys, 1);
    }
    public void PutMany(StorePut[] items) {
        using (var input = new StorePins(items)) lock (gate) Native.Check(
            StoreNative.fqdb_store_txn_put_many(handle, input.Items, (uint)input.Items.Length));
    }
    public void DeleteMany(byte[][] keys) {
        using (var input = new StorePins(keys)) lock (gate) Native.Check(
            StoreNative.fqdb_store_txn_delete_many(handle, input.Keys, (uint)input.Keys.Length));
    }
    public void PutBatch(StorePreparedBatch batch) {
        if (batch == null)
            throw new ArgumentNullException(nameof(batch));
        lock (gate) Native.Check(StoreNative.fqdb_store_txn_put_batch(handle, batch.Handle));
    }
}
}
