using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace FengquDB {
public class DatabaseException : Exception {
    public int Code { get; }
    public DatabaseException(int code)
        : base($"FengquDB {code}: {Marshal.PtrToStringAnsi(Native.fqdb_status_message(code))}") {
        Code = code;
    }
}
public sealed class BufferTooSmallException : DatabaseException {
    public ulong RequiredSize { get; }
    internal BufferTooSmallException(ulong required) : base(16) {
        RequiredSize = required;
    }
}
[StructLayout(LayoutKind.Sequential)]
internal struct Slice {
    internal IntPtr Data;
    internal ulong Size;
    internal Slice(IntPtr data, ulong size) {
        Data = data;
        Size = size;
    }
}
internal static class Native {
#if UNITY_IOS && !UNITY_EDITOR
    const string Lib = "__Internal";
#else
    const string Lib = "fengqudb";
#endif
    internal static void Check(int status) {
        if (status != 0)
            throw new DatabaseException(status);
    }
    internal static byte[] Copy(Slice value) {
        var bytes = new byte[checked((int)value.Size)];
        if (bytes.Length != 0)
            Marshal.Copy(value.Data, bytes, 0, bytes.Length);
        return bytes;
    }
    internal static byte[]? Read(int code, Slice value) {
        if (code == 1)
            return null;
        Check(code);
        try {
            return Copy(value);
        } finally {
            fqdb_buffer_free(ref value);
        }
    }
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint fqdb_abi_version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr fqdb_status_message(int code);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void fqdb_buffer_free(ref Slice value);
}
}
