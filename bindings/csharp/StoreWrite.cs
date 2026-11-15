using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace FengquDB {
public enum StoreWriteMode : uint { Reliable = 0, Performance = 1 }
public sealed class StoreWriteOptions {
    public StoreWriteMode Mode { get; set; } = StoreWriteMode.Performance;
    public uint PeriodMs { get; set; } = 50;
    public ulong ThresholdBytes { get; set; } = 1048576;
    internal StoreNativeWriteOptions Native() => new StoreNativeWriteOptions {
        Size = (uint)Marshal.SizeOf<StoreNativeWriteOptions>(), Abi = 1, Mode = (uint)Mode,
        Period = PeriodMs, Bytes = ThresholdBytes
    };
}
[StructLayout(LayoutKind.Sequential)]
internal struct StoreNativeWriteOptions {
    internal uint Size, Abi, Mode, Period;
    internal ulong Bytes;
}
public sealed class StoreCommit : IDisposable {
    readonly StoreHandle handle;
    internal StoreCommit(IntPtr pointer) {
        handle = new StoreHandle(pointer, 8);
    }
    public ulong? Poll() {
        int status =
            StoreNative.fqdb_store_commit_poll(handle, out int completion, out ulong version);
        if (status == 18)
            return null;
        Native.Check(status);
        Native.Check(completion);
        return version;
    }
    public ulong Wait(ulong timeoutMs = 0) {
        Native.Check(StoreNative.fqdb_store_commit_wait(handle, timeoutMs, out int completion,
                                                        out ulong version));
        Native.Check(completion);
        return version;
    }
    // Uses shared runtime timers, not one blocking Task.Run/thread per write.
    // Unity can also poll once per frame or poll a list of receipts in one pass.
    public async Task<ulong> WaitAsync(int pollIntervalMs = 5) {
        if (pollIntervalMs < 1)
            throw new ArgumentOutOfRangeException(nameof(pollIntervalMs));
        for (;;) {
            ulong? value = Poll();
            if (value.HasValue)
                return value.Value;
            await Task.Delay(pollIntervalMs).ConfigureAwait(false);
        }
    }
    public void Cancel() => StoreNative.fqdb_store_commit_cancel(handle);
    // Closing a pending receipt waits. Dispose after WaitAsync or successful Poll.
    public void Dispose() => handle.Dispose();
}
[StructLayout(LayoutKind.Sequential)]
public struct StoreWriteState {
    public ulong Version, DurableVersion, PendingBytes, LastStatus;
}
public sealed partial class Store {
    public StoreWriteState WriteState() {
        Native.Check(StoreNative.fqdb_store_write_state_get(handle, out var result));
        return result;
    }
    public unsafe StoreAtomicInfo Put(byte[] key, byte[] value, ulong expiresAtMs = 0,
                                      bool forceReliable = false) {
        if (key == null || value == null)
            throw new ArgumentNullException();
        return Put(key.AsSpan(), value.AsSpan(), expiresAtMs, forceReliable);
    }
    public unsafe StoreAtomicInfo Put(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value,
                                      ulong expiresAtMs = 0, bool forceReliable = false) {
        fixed(byte *k = key, v = value) {
            Native.Check(StoreNative.fqdb_store_put(handle, new Slice((IntPtr)k, (ulong)key.Length),
                                                    new Slice((IntPtr)v, (ulong)value.Length),
                                                    expiresAtMs, out var result,
                                                    forceReliable ? 1u : 0u));
            return result;
        }
    }
    public unsafe StoreAtomicInfo Delete(byte[] key, bool forceReliable = false) {
        if (key == null)
            throw new ArgumentNullException(nameof(key));
        fixed(byte *k = key) {
            Native.Check(StoreNative.fqdb_store_delete(handle,
                                                       new Slice((IntPtr)k, (ulong)key.Length),
                                                       out var result, forceReliable ? 1u : 0u));
            return result;
        }
    }
    public unsafe StoreAtomicInfo Patch(byte[] key, ulong offset, ulong remove, byte[] insert,
                                        bool forceReliable = false) {
        if (key == null || insert == null)
            throw new ArgumentNullException();
        fixed(byte *k = key, v = insert) {
            Native.Check(StoreNative.fqdb_store_patch(
                handle, new Slice((IntPtr)k, (ulong)key.Length), offset, remove,
                new Slice((IntPtr)v, (ulong)insert.Length), out var result,
                forceReliable ? 1u : 0u));
            return result;
        }
    }
    public StoreWriteOptions WriteOptions() {
        Native.Check(StoreNative.fqdb_store_write_options_get(handle, out var n));
        return new StoreWriteOptions { Mode = (StoreWriteMode)n.Mode, PeriodMs = n.Period,
                                       ThresholdBytes = n.Bytes };
    }
    public void ConfigureWrite(StoreWriteOptions options) {
        if (options == null)
            throw new ArgumentNullException(nameof(options));
        var n = options.Native();
        Native.Check(StoreNative.fqdb_store_write_configure(handle, ref n));
    }
    public void
    ResetWriteOptions() => Native.Check(StoreNative.fqdb_store_write_reset(handle, IntPtr.Zero));
    // Infrequent control operations may wait for a drain and metadata sync.
    // These use the shared pool once per settings change, never once per write.
    public Task ConfigureWriteAsync(StoreWriteOptions options) {
        if (options == null)
            throw new ArgumentNullException(nameof(options));
        var copy = new StoreWriteOptions { Mode = options.Mode, PeriodMs = options.PeriodMs,
                                           ThresholdBytes = options.ThresholdBytes };
        return Task.Run(() => ConfigureWrite(copy));
    }
    public Task ResetWriteOptionsAsync() => Task.Run(() => ResetWriteOptions());
    public unsafe StoreCommit SubmitPut(byte[] key, byte[] value, ulong expiresAtMs = 0,
                                        bool forceReliable = false) {
        if (key == null || value == null)
            throw new ArgumentNullException();
        IntPtr result;
        fixed(byte *k = key, v = value) Native.Check(
            StoreNative.fqdb_store_put_submit(handle, new Slice((IntPtr)k, (ulong)key.Length),
                                              new Slice((IntPtr)v, (ulong)value.Length),
                                              expiresAtMs, out result, forceReliable ? 1u : 0u));
        try {
            return new StoreCommit(result);
        } catch {
            StoreNative.fqdb_store_commit_close(result);
            throw;
        }
    }
}
public sealed partial class StoreTransaction {
    public StoreCommit Submit(bool forceReliable = false) {
        lock (gate) {
            Native.Check(
                StoreNative.fqdb_store_txn_submit(handle, out var result, forceReliable ? 1u : 0u));
            try {
                return new StoreCommit(result);
            } catch {
                StoreNative.fqdb_store_commit_close(result);
                throw;
            }
        }
    }
}
internal static partial class StoreNative {
    static StoreNative() {
        if (Native.fqdb_abi_version() != 2)
            throw new NotSupportedException("Use the matching FengquDB ABI 2 native library");
    }
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_write_state_get(StoreHandle store,
                                                          out StoreWriteState state);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_put(StoreHandle store, Slice key, Slice value,
                                              ulong expires, out StoreAtomicInfo info,
                                              uint forceReliable);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_delete(StoreHandle store, Slice key,
                                                 out StoreAtomicInfo info, uint forceReliable);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_patch(StoreHandle store, Slice key, ulong offset,
                                                ulong remove, Slice insert,
                                                out StoreAtomicInfo info, uint forceReliable);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_write_configure(StoreHandle store,
                                                          ref StoreNativeWriteOptions options);
    [DllImport(Lib, EntryPoint = "fqdb_store_write_configure",
               CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_write_reset(StoreHandle store, IntPtr options);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_write_options_get(StoreHandle store,
                                                            out StoreNativeWriteOptions options);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_put_submit(StoreHandle store, Slice key, Slice value,
                                                     ulong expires, out IntPtr result,
                                                     uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_txn_submit(StoreHandle txn, out IntPtr result,
                                                     uint forceReliable = 0);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_commit_wait(StoreHandle value, ulong timeout,
                                                      out int status, out ulong version);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int fqdb_store_commit_poll(StoreHandle value, out int status,
                                                      out ulong version);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_commit_cancel(StoreHandle value);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_store_commit_close(IntPtr value);
}
}
