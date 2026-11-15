using System;
using System.Buffers.Binary;
using System.Buffers.Text;
using System.IO;
using System.Text;

namespace FengquDB.Entities {
// Stable field numbers describe the protobuf schema in the user's C# source.
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class FqEntityAttribute : Attribute {
    public string KeyPrefix { get; }
    public FqEntityAttribute(string keyPrefix) { KeyPrefix = keyPrefix; }
}
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class FqFieldAttribute : Attribute {
    public int Number { get; }
    public FqFieldAttribute(int number) { Number = number; }
}
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class FqKeyAttribute : Attribute { }

// Generated schema stamp lets the analyzer reject stale manual fallback code.
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class FqGeneratedEntityAttribute : Attribute {
    public string Schema { get; }
    public FqGeneratedEntityAttribute(string schema) { Schema = schema; }
}

public enum FqEntityKeyKind { Signed, Unsigned, Text }

// Implemented by the source generator. The reader prototype is created once per
// entity type, so no reflection or runtime code generation is needed by IL2CPP.
public interface IFqEntity<T> where T : class {
    int EncodedSize();
    int EncodeTo(Span<byte> destination);
    T ReadEntity(ReadOnlySpan<byte> source);
    ReadOnlySpan<byte> KeyPrefix { get; }
    FqEntityKeyKind KeyKind { get; }
    ulong WriteTo(global::FengquDB.Store store, ulong expiresAtMs, bool forceReliable);
    void WriteTo(global::FengquDB.StoreTransaction transaction, ulong expiresAtMs);
}

// Public only so generated code in a business assembly can validate a large
// write before renting memory. Limits remain shared with the managed runtime.
public static class FqEntityStorage {
    public static void ValidateSize(int keyBufferBytes, int valueBytes) =>
        global::FengquDB.TypedValues.ValidateSizes(keyBufferBytes, valueBytes, true);
}

// Shared bounded protobuf primitives used by generated code and scalar values.
public static class FqWire {
    public static readonly Encoding Utf8 = new UTF8Encoding(false, true);
    public static int VarintSize(ulong value) {
        int count = 1;
        while (value >= 128) { value >>= 7; ++count; }
        return count;
    }
    public static ulong ZigZag(long value) => unchecked((ulong)((value << 1) ^ (value >> 63)));
    public static long UnZigZag(ulong value) => unchecked((long)(value >> 1) ^ -((long)value & 1));
    public static int WriteVarint(Span<byte> output, ulong value) {
        int count = 0;
        while (value >= 128) {
            output[count++] = (byte)(value | 128);
            value >>= 7;
        }
        output[count++] = (byte)value;
        return count;
    }
    public static ulong ReadVarint(ReadOnlySpan<byte> input, ref int position) {
        ulong value = 0;
        for (int index = 0; index < 10; ++index) {
            if ((uint)position >= (uint)input.Length)
                throw new InvalidDataException("Truncated protobuf varint");
            byte next = input[position++];
            if (index == 9 && next > 1)
                throw new InvalidDataException("Protobuf varint overflows uint64");
            value |= (ulong)(next & 127) << (index * 7);
            if (next < 128) return value;
        }
        throw new InvalidDataException("Invalid protobuf varint");
    }
    public static uint ReadTag(ReadOnlySpan<byte> input, ref int position) {
        ulong tag = ReadVarint(input, ref position);
        if (tag > uint.MaxValue || (tag >> 3) == 0 || (tag & 7) > 5)
            throw new InvalidDataException("Invalid protobuf field tag");
        return (uint)tag;
    }
    public static ReadOnlySpan<byte> ReadBytes(ReadOnlySpan<byte> input, ref int position) {
        ulong length = ReadVarint(input, ref position);
        if (length > (ulong)(input.Length - position))
            throw new InvalidDataException("Truncated protobuf bytes");
        var result = input.Slice(position, (int)length);
        position += (int)length;
        return result;
    }
    public static bool ReadBool(ReadOnlySpan<byte> input, ref int position) {
        ulong value = ReadVarint(input, ref position);
        if (value > 1) throw new InvalidDataException("Boolean must be zero or one");
        return value != 0;
    }
    public static double ReadDouble(ReadOnlySpan<byte> input, ref int position) {
        Require(input, position, 8);
        long bits = BinaryPrimitives.ReadInt64LittleEndian(input.Slice(position, 8));
        position += 8;
        return BitConverter.Int64BitsToDouble(bits);
    }
    public static float ReadSingle(ReadOnlySpan<byte> input, ref int position) {
        Require(input, position, 4);
        int bits = BinaryPrimitives.ReadInt32LittleEndian(input.Slice(position, 4));
        position += 4;
        return BitConverter.Int32BitsToSingle(bits);
    }
    public static int WriteDouble(Span<byte> output, double value) {
        BinaryPrimitives.WriteInt64LittleEndian(output, BitConverter.DoubleToInt64Bits(value));
        return 8;
    }
    public static int WriteSingle(Span<byte> output, float value) {
        BinaryPrimitives.WriteInt32LittleEndian(output, BitConverter.SingleToInt32Bits(value));
        return 4;
    }
    public static int BytesSize(int length) => checked(VarintSize((ulong)length) + length);
    public static int WriteBytes(Span<byte> output, ReadOnlySpan<byte> value) {
        int count = WriteVarint(output, (ulong)value.Length);
        value.CopyTo(output.Slice(count));
        return checked(count + value.Length);
    }
    public static int StringSize(string value) => BytesSize(Utf8.GetByteCount(value));
    public static int WriteString(Span<byte> output, string value) {
        int length = Utf8.GetByteCount(value);
        int count = WriteVarint(output, (ulong)length);
        return checked(count + Utf8.GetBytes(value.AsSpan(), output.Slice(count, length)));
    }
    static void Require(ReadOnlySpan<byte> input, int position, int length) {
        if ((uint)position > (uint)input.Length || length > input.Length - position)
            throw new InvalidDataException("Truncated protobuf field");
    }
    public static void Skip(ReadOnlySpan<byte> input, ref int position, uint tag) =>
        Skip(input, ref position, tag, 0);
    static void Skip(ReadOnlySpan<byte> input, ref int position, uint tag, int depth) {
        int length;
        switch (tag & 7) {
        case 0: ReadVarint(input, ref position); return;
        case 1: length = 8; break;
        case 2: ReadBytes(input, ref position); return;
        case 3:
            if (depth == 64) throw new InvalidDataException("Protobuf group nesting limit");
            while (position < input.Length) {
                uint inner = ReadTag(input, ref position);
                if ((inner & 7) == 4) {
                    if ((inner >> 3) != (tag >> 3))
                        throw new InvalidDataException("Mismatched protobuf end group");
                    return;
                }
                Skip(input, ref position, inner, depth + 1);
            }
            throw new InvalidDataException("Unterminated protobuf group");
        case 5: length = 4; break;
        default: throw new InvalidDataException("Unexpected protobuf end group");
        }
        Require(input, position, length);
        position += length;
    }
    public static int KeySize(ReadOnlySpan<byte> prefix, long id) => checked(prefix.Length + 20);
    public static int KeySize(ReadOnlySpan<byte> prefix, ulong id) => checked(prefix.Length + 20);
    public static int KeySize(ReadOnlySpan<byte> prefix, string id) =>
        checked(prefix.Length + Utf8.GetByteCount(id));
    public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, long id) {
        prefix.CopyTo(output);
        if (!Utf8Formatter.TryFormat(id, output.Slice(prefix.Length), out int count))
            throw new ArgumentException("Key buffer is too small");
        return checked(prefix.Length + count);
    }
    public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, ulong id) {
        prefix.CopyTo(output);
        if (!Utf8Formatter.TryFormat(id, output.Slice(prefix.Length), out int count))
            throw new ArgumentException("Key buffer is too small");
        return checked(prefix.Length + count);
    }
    public static int WriteKey(Span<byte> output, ReadOnlySpan<byte> prefix, string id) {
        prefix.CopyTo(output);
        return checked(prefix.Length + Utf8.GetBytes(id.AsSpan(), output.Slice(prefix.Length)));
    }
    public static byte[] CopyUnknown(ReadOnlySpan<byte> source, int length, Func<uint, bool> known) {
        var result = new byte[length];
        int position = 0, output = 0;
        while (position < source.Length) {
            int begin = position;
            uint tag = ReadTag(source, ref position);
            Skip(source, ref position, tag);
            if (known(tag)) continue;
            int count = position - begin;
            source.Slice(begin, count).CopyTo(result.AsSpan(output));
            output += count;
        }
        return result;
    }
}
}
