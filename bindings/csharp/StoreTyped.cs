using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;
using FengquDB.Entities;

namespace FengquDB {
internal delegate T StoreDecoder<T>(ReadOnlySpan<byte> source);

internal static partial class StoreNative {
    // Decode while the immutable native lease is alive. Falling back when the
    // pin budget is full preserves the ordinary Get contract without a byte[].
    internal static unsafe bool ReadDecoded<T>(StoreHandle handle, ReadOnlySpan<byte> key,
                                               int kind, StoreDecoder<T> decode, out T result) {
        result = default!;
        fixed (byte* pointer = key) {
            var slice = new Slice((IntPtr)pointer, (ulong)key.Length);
            if (kind != 1) {
                int status = kind == 0
                    ? fqdb_store_get_shared(handle, slice, out var shared)
                    : fqdb_store_snapshot_get_shared(handle, slice, out shared);
                if (status == 1) return false;
                if (status != 9) {
                    Native.Check(status);
                    try {
                        result = decode(new ReadOnlySpan<byte>((void*)shared.Row.Value.Data,
                                                     checked((int)shared.Row.Value.Size)));
                        return true;
                    } finally { fqdb_store_value_close(shared.Owner); }
                }
            }
            Slice output;
            int code = kind == 0 ? fqdb_store_get(handle, slice, out output)
                : kind == 1 ? fqdb_store_txn_get(handle, slice, out output)
                : fqdb_store_snapshot_get(handle, slice, out output);
            if (code == 1) return false;
            Native.Check(code);
            try {
                result = decode(new ReadOnlySpan<byte>((void*)output.Data, checked((int)output.Size)));
                return true;
            } finally { Native.fqdb_buffer_free(ref output); }
        }
    }
}

// A codec and its delegates are chosen once per closed scalar type. Numeric
// values are passed as T, with no boxing, reflection or payload allocation.
internal abstract class ScalarCodec<T> {
    internal readonly uint Tag;
    internal readonly StoreDecoder<T> Decoder;
    internal readonly StoreReadAction<T> ReadAction;
    protected ScalarCodec(uint tag, StoreDecoder<T>? decoder = null, StoreReadAction<T>? read = null) {
        Tag = tag; Decoder = decoder ?? Decode; ReadAction = read ?? ReadBatch;
    }
    T ReadBatch(StoreReadBatch rows) => rows[0].Found ? Decoder(rows[0].Value) : throw new KeyNotFoundException();
    internal abstract int Measure(T value);
    internal abstract int Write(Span<byte> output, T value);
    internal abstract T Read(ReadOnlySpan<byte> input, ref int position);
    protected virtual T Decode(ReadOnlySpan<byte> source) {
        int position = 0;
        bool found = false;
        T value = default!;
        while (position < source.Length) {
            uint tag = FqWire.ReadTag(source, ref position);
            if (tag == Tag) { value = Read(source, ref position); found = true; }
            else FqWire.Skip(source, ref position, tag);
        }
        return found ? value : throw new InvalidDataException("Scalar record is missing field 1 or has a different wire type");
    }
}
internal sealed class IntegerCodec<T> : ScalarCodec<T> {
    readonly Func<T, ulong> encode;
    readonly Func<ulong, T> decode;
    internal IntegerCodec(Func<T, ulong> encode, Func<ulong, T> decode,
                          StoreDecoder<T>? reader = null, StoreReadAction<T>? batch = null) : base(8, reader, batch) {
        this.encode = encode; this.decode = decode;
    }
    internal override int Measure(T value) => FqWire.VarintSize(encode(value));
    internal override int Write(Span<byte> output, T value) => FqWire.WriteVarint(output, encode(value));
    internal override T Read(ReadOnlySpan<byte> input, ref int position) =>
        decode(FqWire.ReadVarint(input, ref position));
    protected override T Decode(ReadOnlySpan<byte> source) {
        int position = 0;
        bool found = false;
        T value = default!;
        while (position < source.Length) {
            uint tag = FqWire.ReadTag(source, ref position);
            if (tag == 8) { value = decode(FqWire.ReadVarint(source, ref position)); found = true; }
            else FqWire.Skip(source, ref position, tag);
        }
        return found ? value : throw new InvalidDataException("Scalar record is missing integer field 1");
    }
}
internal static class ScalarReaders {
    // Common signed scalar reads have direct typed code. Passing their batch
    // callbacks straight to StoreReader avoids an adapter and conversion delegate.
    internal static int Int32(ReadOnlySpan<byte> source) {
        int position = 0, result = 0;
        bool found = false;
        while (position < source.Length) {
            uint tag = FqWire.ReadTag(source, ref position);
            if (tag == 8) {
                result = checked((int)FqWire.UnZigZag(FqWire.ReadVarint(source, ref position)));
                found = true;
            } else FqWire.Skip(source, ref position, tag);
        }
        return found ? result : throw new InvalidDataException("Scalar record is missing integer field 1");
    }
    internal static long Int64(ReadOnlySpan<byte> source) {
        int position = 0;
        long result = 0;
        bool found = false;
        while (position < source.Length) {
            uint tag = FqWire.ReadTag(source, ref position);
            if (tag == 8) { result = FqWire.UnZigZag(FqWire.ReadVarint(source, ref position)); found = true; }
            else FqWire.Skip(source, ref position, tag);
        }
        return found ? result : throw new InvalidDataException("Scalar record is missing integer field 1");
    }
    internal static int Int32Batch(StoreReadBatch rows) =>
        rows[0].Found ? Int32(rows[0].Value) : throw new KeyNotFoundException();
    internal static long Int64Batch(StoreReadBatch rows) =>
        rows[0].Found ? Int64(rows[0].Value) : throw new KeyNotFoundException();
}
internal sealed class DoubleCodec : ScalarCodec<double> {
    internal DoubleCodec() : base(9) { }
    internal override int Measure(double value) => 8;
    internal override int Write(Span<byte> output, double value) => FqWire.WriteDouble(output, value);
    internal override double Read(ReadOnlySpan<byte> input, ref int position) => FqWire.ReadDouble(input, ref position);
}
internal sealed class SingleCodec : ScalarCodec<float> {
    internal SingleCodec() : base(13) { }
    internal override int Measure(float value) => 4;
    internal override int Write(Span<byte> output, float value) => FqWire.WriteSingle(output, value);
    internal override float Read(ReadOnlySpan<byte> input, ref int position) => FqWire.ReadSingle(input, ref position);
}
internal sealed class StringCodec : ScalarCodec<string> {
    internal StringCodec() : base(10) { }
    internal override int Measure(string value) => FqWire.StringSize(value ?? throw new ArgumentNullException(nameof(value)));
    internal override int Write(Span<byte> output, string value) => FqWire.WriteString(output, value);
    internal override string Read(ReadOnlySpan<byte> input, ref int position) => FqWire.Utf8.GetString(FqWire.ReadBytes(input, ref position));
}
internal sealed class BytesCodec : ScalarCodec<byte[]> {
    internal BytesCodec() : base(10) { }
    internal override int Measure(byte[] value) => FqWire.BytesSize((value ?? throw new ArgumentNullException(nameof(value))).Length);
    internal override int Write(Span<byte> output, byte[] value) => FqWire.WriteBytes(output, value);
    internal override byte[] Read(ReadOnlySpan<byte> input, ref int position) => FqWire.ReadBytes(input, ref position).ToArray();
}
internal static class Scalar<T> {
    static readonly ScalarCodec<T>? instance = Create();
    internal static ScalarCodec<T> Codec => instance ?? throw new NotSupportedException(
        "Use [FqEntity] and Put(entity)/GetEntity<T>(id) for entity classes. Unsupported scalar: " + typeof(T).FullName);
    static ScalarCodec<T>? Create() {
        object codec;
        if (typeof(T) == typeof(int)) codec = new IntegerCodec<int>(v => FqWire.ZigZag(v),
            v => checked((int)FqWire.UnZigZag(v)), ScalarReaders.Int32, ScalarReaders.Int32Batch);
        else if (typeof(T) == typeof(long)) codec = new IntegerCodec<long>(FqWire.ZigZag,
            FqWire.UnZigZag, ScalarReaders.Int64, ScalarReaders.Int64Batch);
        else if (typeof(T) == typeof(uint)) codec = new IntegerCodec<uint>(v => v, v => checked((uint)v));
        else if (typeof(T) == typeof(ulong)) codec = new IntegerCodec<ulong>(v => v, v => v);
        else if (typeof(T) == typeof(short)) codec = new IntegerCodec<short>(v => FqWire.ZigZag(v), v => checked((short)FqWire.UnZigZag(v)));
        else if (typeof(T) == typeof(ushort)) codec = new IntegerCodec<ushort>(v => v, v => checked((ushort)v));
        else if (typeof(T) == typeof(sbyte)) codec = new IntegerCodec<sbyte>(v => FqWire.ZigZag(v), v => checked((sbyte)FqWire.UnZigZag(v)));
        else if (typeof(T) == typeof(byte)) codec = new IntegerCodec<byte>(v => v, v => checked((byte)v));
        else if (typeof(T) == typeof(bool)) codec = new IntegerCodec<bool>(v => v ? 1UL : 0UL,
            v => v <= 1 ? v != 0 : throw new InvalidDataException("Boolean must be zero or one"));
        else if (typeof(T) == typeof(double)) codec = new DoubleCodec();
        else if (typeof(T) == typeof(float)) codec = new SingleCodec();
        else if (typeof(T) == typeof(string)) codec = new StringCodec();
        else if (typeof(T) == typeof(byte[])) codec = new BytesCodec();
        else return null;
        return (ScalarCodec<T>)codec;
    }
}
internal static class EntityModel<T> where T : class, IFqEntity<T>, new() {
    internal static readonly T Prototype = new T();
    internal static readonly StoreDecoder<T> Decode = Prototype.ReadEntity;
}
internal readonly struct EntityId {
    readonly long signed;
    readonly ulong unsigned;
    readonly string? text;
    readonly FqEntityKeyKind kind;
    internal EntityId(long value) { signed = value; unsigned = 0; text = null; kind = FqEntityKeyKind.Signed; }
    internal EntityId(ulong value) { signed = 0; unsigned = value; text = null; kind = FqEntityKeyKind.Unsigned; }
    internal EntityId(string value) { signed = 0; unsigned = 0; text = value ?? throw new ArgumentNullException(nameof(value)); kind = FqEntityKeyKind.Text; }
    internal int Size<T>(T model) where T : class, IFqEntity<T> {
        if ((kind == FqEntityKeyKind.Text) != (model.KeyKind == FqEntityKeyKind.Text) ||
            (kind == FqEntityKeyKind.Signed && signed < 0 && model.KeyKind == FqEntityKeyKind.Unsigned) ||
            (kind == FqEntityKeyKind.Unsigned && unsigned > long.MaxValue && model.KeyKind == FqEntityKeyKind.Signed))
            throw new ArgumentException("Entity ID does not match its key type");
        return kind == FqEntityKeyKind.Text ? FqWire.KeySize(model.KeyPrefix, text!)
            : checked(model.KeyPrefix.Length + 20);
    }
    internal int Write(Span<byte> output, ReadOnlySpan<byte> prefix) =>
        kind == FqEntityKeyKind.Text ? FqWire.WriteKey(output, prefix, text!)
        : kind == FqEntityKeyKind.Signed ? FqWire.WriteKey(output, prefix, signed)
        : FqWire.WriteKey(output, prefix, unsigned);
}

// Shares buffer management between Store and transaction APIs. The transaction
// retains its existing gate and commits durability only when Commit is called.
internal readonly struct TypedTarget {
    readonly Store? store;
    readonly StoreTransaction? transaction;
    internal TypedTarget(Store value) { store = value; transaction = null; }
    internal TypedTarget(StoreTransaction value) { store = null; transaction = value; }
    internal StoreAtomicInfo Write(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value,
                                   ulong expires, bool reliable) {
        if (store != null) return store.Put(key, value, expires, reliable);
        transaction!.Put(key, value, expires);
        return default;
    }
    internal bool Read<T>(ReadOnlySpan<byte> key, StoreDecoder<T> decode, out T value) =>
        store != null ? store.TryReadDecoded(key, decode, out value)
                      : transaction!.TryReadDecoded(key, decode, out value);
}
internal static class TypedValues {
    const int StackBytes = 512;
    internal static StoreAtomicInfo Put<T>(TypedTarget target, string key, T value,
                                           ulong expires, bool reliable) {
        if (key == null) throw new ArgumentNullException(nameof(key));
        var codec = Scalar<T>.Codec;
        int keySize = FqWire.Utf8.GetByteCount(key);
        int size = checked(1 + codec.Measure(value));
        return PutSized(target, key, value, codec, keySize, size, expires, reliable);
    }
    internal static StoreAtomicInfo PutSized<T>(TypedTarget target, string key, T value,
        ScalarCodec<T> codec, int keySize, int size, ulong expires, bool reliable) {
        ValidateSizes(keySize, size, false);
        byte[]? rented = null;
        int capacity = checked(keySize + size);
        Span<byte> buffer = capacity <= StackBytes ? stackalloc byte[StackBytes]
            : (rented = ArrayPool<byte>.Shared.Rent(capacity));
        try {
            FqWire.Utf8.GetBytes(key.AsSpan(), buffer.Slice(0, keySize));
            var payload = buffer.Slice(keySize, size);
            payload[0] = (byte)codec.Tag;
            int written = checked(1 + codec.Write(payload.Slice(1), value));
            return target.Write(buffer.Slice(0, keySize), payload.Slice(0, written), expires, reliable);
        } finally { if (rented != null) ArrayPool<byte>.Shared.Return(rented); }
    }
    internal static bool TryGet<T>(TypedTarget target, string key, out T value) {
        if (key == null) throw new ArgumentNullException(nameof(key));
        var codec = Scalar<T>.Codec;
        int size = FqWire.Utf8.GetByteCount(key);
        byte[]? rented = null;
        Span<byte> buffer = size <= StackBytes ? stackalloc byte[StackBytes]
            : (rented = ArrayPool<byte>.Shared.Rent(size));
        try {
            int written = FqWire.Utf8.GetBytes(key.AsSpan(), buffer);
            return target.Read(buffer.Slice(0, written), codec.Decoder, out value);
        } finally { if (rented != null) ArrayPool<byte>.Shared.Return(rented); }
    }
    internal static T Get<T>(TypedTarget target, string key) =>
        TryGet(target, key, out T value) ? value : throw new KeyNotFoundException(key);
    internal static void ValidateSizes(int keyBytes, int valueBytes, bool keyUpperBound) {
        // Match FQDB_MAX_KEY_BYTES/FQDB_MAX_VALUE_BYTES before renting memory.
        if ((uint)keyBytes > 65536u + (keyUpperBound ? 20u : 0u))
            throw new ArgumentOutOfRangeException(nameof(keyBytes), "Encoded key exceeds the native limit");
        if ((uint)valueBytes > 16777216u)
            throw new ArgumentOutOfRangeException(nameof(valueBytes), "Encoded value exceeds the native limit");
    }
    internal static T? GetEntity<T>(TypedTarget target, EntityId id)
        where T : class, IFqEntity<T>, new() {
        var model = EntityModel<T>.Prototype;
        int size = id.Size(model);
        byte[]? rented = null;
        Span<byte> buffer = size <= StackBytes ? stackalloc byte[StackBytes]
            : (rented = ArrayPool<byte>.Shared.Rent(size));
        try {
            int written = id.Write(buffer, model.KeyPrefix);
            return target.Read(buffer.Slice(0, written), EntityModel<T>.Decode, out T value) ? value : null;
        } finally { if (rented != null) ArrayPool<byte>.Shared.Return(rented); }
    }
    internal static StoreTypedReader<T> PrepareEntity<T>(Store store, EntityId id)
        where T : class, IFqEntity<T>, new() {
        var model = EntityModel<T>.Prototype;
        var key = new byte[id.Size(model)];
        int count = id.Write(key, model.KeyPrefix);
        if (count != key.Length) Array.Resize(ref key, count);
        return StoreTypedReader<T>.Create(store.PrepareReader(key), EntityModel<T>.Decode);
    }
}
// Fixed keys are encoded once. A scalar Read or a user-supplied field decoder
// can then use the native prepared reader without payload copying or GC work.
public sealed class StoreTypedReader<T> : IDisposable {
    readonly StoreReader reader;
    readonly StoreReadAction<(bool Found, T Value)> action;
    readonly StoreReadAction<T> read;
    StoreTypedReader(StoreReader reader, StoreDecoder<T> decode, StoreReadAction<T>? directRead) {
        this.reader = reader;
        action = rows => rows[0].Found ? (true, decode(rows[0].Value)) : (false, default!);
        read = directRead ?? (rows => rows[0].Found ? decode(rows[0].Value) : throw new KeyNotFoundException());
    }
    internal static StoreTypedReader<T> Create(StoreReader reader, StoreDecoder<T> decode,
                                               StoreReadAction<T>? directRead = null) {
        try { return new StoreTypedReader<T>(reader, decode, directRead); }
        catch { reader.Dispose(); throw; }
    }
    public bool TryRead(out T value) {
        var result = reader.Read(action);
        value = result.Value;
        return result.Found;
    }
    public T Read() => reader.Read(read);
    public void Release() => reader.Release();
    public void Dispose() => reader.Dispose();
}
public sealed partial class Store {
    internal bool TryReadDecoded<T>(ReadOnlySpan<byte> key, StoreDecoder<T> decoder, out T value) =>
        StoreNative.ReadDecoded(handle, key, 0, decoder, out value);
    public ulong Put<T>(string key, T value, ulong expiresAtMs = 0, bool forceReliable = false) {
        if (key == null) throw new ArgumentNullException(nameof(key));
        var codec = Scalar<T>.Codec;
        int keySize = FqWire.Utf8.GetByteCount(key);
        int size = checked(1 + codec.Measure(value));
        if (checked(keySize + size) > 512)
            return TypedValues.PutSized(new TypedTarget(this), key, value, codec, keySize, size, expiresAtMs, forceReliable).Version;
        // Keep the common small-write path direct, without the pooled-buffer
        // cleanup/transaction adapter between serialization and native entry.
        Span<byte> buffer = stackalloc byte[512];
        FqWire.Utf8.GetBytes(key.AsSpan(), buffer.Slice(0, keySize));
        var payload = buffer.Slice(keySize, size);
        payload[0] = (byte)codec.Tag;
        int written = checked(1 + codec.Write(payload.Slice(1), value));
        return Put(buffer.Slice(0, keySize), payload.Slice(0, written), expiresAtMs, forceReliable).Version;
    }
    public T Get<T>(string key) => TypedValues.Get<T>(new TypedTarget(this), key);
    public bool TryGet<T>(string key, out T value) => TypedValues.TryGet(new TypedTarget(this), key, out value);
    public ulong Put<T>(T entity, ulong expiresAtMs = 0, bool forceReliable = false)
        where T : class, IFqEntity<T> {
        if (entity == null) throw new ArgumentNullException(nameof(entity));
        return entity.WriteTo(this, expiresAtMs, forceReliable);
    }
    public T? GetEntity<T>(long id) where T : class, IFqEntity<T>, new() => TypedValues.GetEntity<T>(new TypedTarget(this), new EntityId(id));
    public T? GetEntity<T>(ulong id) where T : class, IFqEntity<T>, new() => TypedValues.GetEntity<T>(new TypedTarget(this), new EntityId(id));
    public T? GetEntity<T>(string id) where T : class, IFqEntity<T>, new() => TypedValues.GetEntity<T>(new TypedTarget(this), new EntityId(id));
    public StoreTypedReader<T> PrepareValue<T>(string key) {
        if (key == null) throw new ArgumentNullException(nameof(key));
        var codec = Scalar<T>.Codec;
        return StoreTypedReader<T>.Create(PrepareReader(FqWire.Utf8.GetBytes(key)), codec.Decoder, codec.ReadAction);
    }
    public StoreTypedReader<T> PrepareEntity<T>(long id) where T : class, IFqEntity<T>, new() => TypedValues.PrepareEntity<T>(this, new EntityId(id));
    public StoreTypedReader<T> PrepareEntity<T>(ulong id) where T : class, IFqEntity<T>, new() => TypedValues.PrepareEntity<T>(this, new EntityId(id));
    public StoreTypedReader<T> PrepareEntity<T>(string id) where T : class, IFqEntity<T>, new() => TypedValues.PrepareEntity<T>(this, new EntityId(id));
}
public sealed partial class StoreTransaction {
    internal bool TryReadDecoded<T>(ReadOnlySpan<byte> key, StoreDecoder<T> decoder, out T value) {
        lock (gate) return StoreNative.ReadDecoded(handle, key, 1, decoder, out value);
    }
    public void Put<T>(string key, T value, ulong expiresAtMs = 0) =>
        TypedValues.Put(new TypedTarget(this), key, value, expiresAtMs, false);
    public T Get<T>(string key) => TypedValues.Get<T>(new TypedTarget(this), key);
    public bool TryGet<T>(string key, out T value) => TypedValues.TryGet(new TypedTarget(this), key, out value);
    public void Put<T>(T entity, ulong expiresAtMs = 0) where T : class, IFqEntity<T> {
        if (entity == null) throw new ArgumentNullException(nameof(entity));
        entity.WriteTo(this, expiresAtMs);
    }
    public T? GetEntity<T>(long id) where T : class, IFqEntity<T>, new() => TypedValues.GetEntity<T>(new TypedTarget(this), new EntityId(id));
    public T? GetEntity<T>(ulong id) where T : class, IFqEntity<T>, new() => TypedValues.GetEntity<T>(new TypedTarget(this), new EntityId(id));
    public T? GetEntity<T>(string id) where T : class, IFqEntity<T>, new() => TypedValues.GetEntity<T>(new TypedTarget(this), new EntityId(id));
}
}
