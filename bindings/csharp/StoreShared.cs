using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

namespace FengquDB {
// Ref structs prevent a safe managed caller from retaining borrowed native memory.
public readonly unsafe ref struct StoreSharedEntry {
    readonly StoreNativeResult *row;
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal StoreSharedEntry(StoreNativeResult *value) {
        row = value;
    }
    public bool Found {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => row != null && row->Found != 0;
    }
    public ulong Version => row == null ? 0 : row->Version;
    public ulong ExpiresAtMs => row == null ? 0 : row->Expires;
    public ReadOnlySpan<byte> Value {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => row == null
                   ? ReadOnlySpan<byte>.Empty
                   : new ReadOnlySpan<byte>((void *)row->Value.Data, checked((int)row->Value.Size));
    }
}
public readonly unsafe ref struct StoreReadBatch {
    readonly StoreNativeResult *rows;
    public int Count { get; }
    internal StoreReadBatch(IntPtr pointer, uint count) {
        rows = (StoreNativeResult *)pointer;
        Count = checked((int)count);
    }
    public StoreSharedEntry this[int index] {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get {
            if ((uint)index >= (uint)Count)
                throw new ArgumentOutOfRangeException(nameof(index));
            return new StoreSharedEntry(rows + index);
        }
    }
}
public delegate TResult StoreReadAction<TResult>(StoreReadBatch batch);
public delegate TResult StoreValueAction<TResult>(StoreSharedEntry entry);

/// <summary>Reusable keys and shared immutable results. Read callbacks run under
/// a reader-local gate; recursive reads, release and dispose are rejected.
/// Different readers can read concurrently. No payload or descriptor allocation
/// occurs on unchanged hot reads. Dispose or Release drops retained values.</summary>
public sealed class StoreReader : IDisposable {
    readonly StoreHandle handle;
    readonly object gate = new object();
    bool reading;
    internal StoreReader(IntPtr pointer) {
        handle = new StoreHandle(pointer, 7);
    }
    void Available() {
        if (reading)
            throw new InvalidOperationException("Reader callback is active");
        if (handle.IsClosed)
            throw new ObjectDisposedException(nameof(StoreReader));
    }
    public TResult Read<TResult>(StoreReadAction<TResult> action) {
        if (action == null)
            throw new ArgumentNullException(nameof(action));
        lock (gate) {
            Available();
            reading = true;
            try {
                // The gate excludes Dispose; KeepAlive below excludes finalization.
                Native.Check(StoreNative.fqdb_store_reader_read(handle.DangerousGetHandle(),
                                                                out var rows, out var count));
                return action(new StoreReadBatch(rows, count));
            } finally {
                reading = false;
                GC.KeepAlive(handle);
            }
        }
    }
    public void Release() {
        lock (gate) {
            Available();
            StoreNative.fqdb_store_reader_release(handle.DangerousGetHandle());
            GC.KeepAlive(handle);
        }
    }
    public void Dispose() {
        lock (gate) {
            if (reading)
                throw new InvalidOperationException("Reader callback is active");
            handle.Dispose();
        }
    }
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeShared {
    internal IntPtr Owner;
    internal StoreNativeResult Row;
}
internal static partial class StoreNative {
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_project_reader_create(StoreHandle store,
                                                                [In] Slice[] keys, uint count,
                                                                [In] uint[] fields, uint fieldCount,
                                                                out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_project(StoreHandle store, Slice key, [In] uint[] fields,
                                                  uint fieldCount, out Slice output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_key_get_shared(StoreHandle key, out StoreNativeShared output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_get_shared(StoreHandle store, Slice key,
                                                     out StoreNativeShared output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_snapshot_get_shared(StoreHandle snapshot, Slice key,
                                                              out StoreNativeShared output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_reader_create(StoreHandle store, [In] Slice[] keys,
                                                        uint count, out IntPtr output);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_reader_read(IntPtr reader, out IntPtr rows,
                                                      out uint count);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_reader_release(IntPtr reader);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_reader_close(IntPtr reader);
}
public sealed partial class Store {
    public StoreReader PrepareProjectReader(byte[][] keys, uint[] fields) {
        if (fields == null || fields.Length > 256)
            throw new ArgumentException("Invalid fields");
        using (var input = new StorePins(keys)) {
            Native.Check(StoreNative.fqdb_store_project_reader_create(
                handle, input.Keys, (uint)input.Keys.Length, fields, (uint)fields.Length,
                out var output));
            try {
                return new StoreReader(output);
            } catch {
                StoreNative.fqdb_store_reader_close(output);
                throw;
            }
        }
    }
    public StoreReader PrepareReader(byte[] key) => PrepareReader(new[] { key });
    public StoreReader PrepareReader(byte[][] keys) {
        using (var input = new StorePins(keys)) {
            Native.Check(StoreNative.fqdb_store_reader_create(
                handle, input.Keys, (uint)input.Keys.Length, out var output));
            try {
                return new StoreReader(output);
            } catch {
                StoreNative.fqdb_store_reader_close(output);
                throw;
            }
        }
    }
    public unsafe TResult ReadShared<TResult>(byte[] key, StoreValueAction<TResult> action) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        if (action == null)
            throw new ArgumentNullException(nameof(action));
        fixed(byte *pointer = key) {
            int status = StoreNative.fqdb_store_get_shared(
                handle, new Slice((IntPtr)pointer, (ulong)key.Length), out var value);
            if (status != 1)
                Native.Check(status);
            try {
                return action(new StoreSharedEntry(&value.Row));
            } finally {
                StoreNative.fqdb_store_value_close(value.Owner);
            }
        }
    }
}
}
