#include "model.hpp"
#include "runtimes.hpp"
#include <sstream>
namespace codegen {
static std::string jtype(const Field &f) {
    if (f.type == "sint32") return "int";
    if (f.type == "sint64" || f.type == "uint64" || f.type == "uint32") return "long";
    if (f.type == "bool") return "boolean";
    if (f.type == "string") return "String";
    return f.type == "bytes" ? "byte[]" : f.type;
}
void emit_java(const Model &m, Files &files) {
    auto dir = "java/" + replace(m.ns, ".", "/") + "/";
    files[dir + "FqModelWire.java"] = banner + "package " + m.ns + ";\n" + runtime_java;
    for (const auto &e : m.entities) {
        std::ostringstream o;
        o << banner << "package " << m.ns << ";\nimport java.nio.charset.StandardCharsets;\nimport java.io.ByteArrayOutputStream;\n\npublic final class " << e.name << " {\n";
        for (const auto &f : e.fields) o << "    public " << jtype(f) << " " << f.name << (f.type == "string" ? " = \"\"" : f.type == "bytes" ? " = new byte[0]" : "") << ";\n";
        o << "    private byte[] fqUnknown = new byte[0];\n"
          << "    private static final byte[] fqPrefix = FqModelWire.text(" << quote(e.prefix) << ");\n"
          << "    private void fqWrite(FqModelWire.Writer w) {\n";
        for (const auto &f : e.fields) {
            auto v = "this." + f.name;
            o << "        w.u(" << tag(f) << "L); ";
            if (text(f)) o << "w.b(" << (f.type == "string" ? "FqModelWire.text(" + v + ")" : v) << ");";
            else if (f.type == "bool") o << "w.u(" << v << " ? 1 : 0);";
            else if (signed_int(f)) o << "w.u(FqModelWire.z(" << v << "));";
            else if (f.type == "uint32") o << "w.u(FqModelWire.u32(" << v << "));";
            else if (f.type == "uint64") o << "w.u(" << v << ");";
            else o << "w.fixed(" << (f.type == "float" ? "Float.floatToRawIntBits(" : "Double.doubleToRawLongBits(") << v << ")," << (f.type == "float" ? 4 : 8) << ");";
            o << "\n";
        }
        o << "        w.raw(fqUnknown);\n    }\n"
          << "    public byte[] fqEncode() {FqModelWire.Writer w=new FqModelWire.Writer(null,0);fqWrite(w);return w.finish();}\n"
          << "    public int fqEncodeInto(byte[] destination,int offset) {if(destination==null)throw new NullPointerException();FqModelWire.Writer w=new FqModelWire.Writer(destination,offset);fqWrite(w);return w.pos-offset;}\n"
          << "    public static " << e.name << " fqDecode(byte[] source) {\n        FqModelWire.Reader r=new FqModelWire.Reader(source); " << e.name << " out=new " << e.name << "(); ByteArrayOutputStream unknown=null;\n"
          << "        while(r.pos<source.length){int begin=r.pos; int t=r.tag();switch(t){\n";
        for (const auto &f : e.fields) {
            o << "        case " << static_cast<int32_t>(tag(f)) << ": out." << f.name << " = ";
            if (f.type == "string") o << "FqModelWire.text(r.b())";
            else if (f.type == "bytes") o << "r.b()";
            else if (f.type == "bool") o << "FqModelWire.bool(r.u())";
            else if (f.type == "sint32") o << "FqModelWire.i32(FqModelWire.unz(r.u()))";
            else if (f.type == "sint64") o << "FqModelWire.unz(r.u())";
            else if (f.type == "uint32") o << "FqModelWire.u32(r.u())";
            else if (f.type == "uint64") o << "r.u()";
            else o << (f.type == "float" ? "Float.intBitsToFloat((int)r.fixed(4))" : "Double.longBitsToDouble(r.fixed(8))");
            o << "; break;\n";
        }
        o << "        default:r.skip(t,0);if(unknown==null)unknown=new ByteArrayOutputStream();unknown.write(source,begin,r.pos-begin);break;\n"
          << "        }}\n        if(unknown!=null)out.fqUnknown=unknown.toByteArray();return out;\n    }\n    public byte[] fqKey() {return FqModelWire.key(fqPrefix,";
        const auto &k = key(e);
        if (k.type == "string") o << "FqModelWire.text(this." << k.name << ")";
        else if (k.type == "uint64") o << "Long.toUnsignedString(this." << k.name << ").getBytes(StandardCharsets.US_ASCII)";
        else o << "Long.toString(" << (k.type == "uint32" ? "FqModelWire.u32(this." + k.name + ")" : "this." + k.name) << ").getBytes(StandardCharsets.US_ASCII)";
        o << ");}\n}\n";
        files[dir + e.name + ".java"] = o.str();
    }
}
static std::string cstype(const Field &f) {
    if (f.type == "sint32") return "int";
    if (f.type == "sint64") return "long";
    if (f.type == "uint32") return "uint";
    if (f.type == "uint64") return "ulong";
    return f.type == "bytes" ? "byte[]" : f.type;
}
static std::string csread(const Field &f) {
    if (f.type == "string") return "FqWire.Utf8.GetString(FqWire.ReadBytes(source, ref p))";
    if (f.type == "bytes") return "FqWire.ReadBytes(source, ref p).ToArray()";
    if (f.type == "bool") return "FqWire.ReadBool(source, ref p)";
    if (f.type == "float") return "FqWire.ReadSingle(source, ref p)";
    if (f.type == "double") return "FqWire.ReadDouble(source, ref p)";
    return "checked((" + cstype(f) + ")" + (signed_int(f) ? "FqWire.UnZigZag(FqWire.ReadVarint(source, ref p))" : "FqWire.ReadVarint(source, ref p)") + ")";
}
void emit_csharp(const Model &m, Files &files) {
    std::ostringstream o;
    o << banner << "#nullable enable\nusing System;\nusing System.Buffers;\nusing System.IO;\nusing FengquDB.Entities;\nnamespace " << m.ns << " {\n";
    for (const auto &e : m.entities) {
        auto contract = "IFqEntity<" + e.name + ">";
        o << "public sealed partial class " << e.name << " : " << contract << " {\n";
        for (const auto &f : e.fields) o << "    public " << cstype(f) << " " << f.name << " {get;set;}" << (f.type == "string" ? " = string.Empty;" : f.type == "bytes" ? " = Array.Empty<byte>();" : "") << "\n";
        o << "    private byte[] fqUnknown = Array.Empty<byte>();\n    private static readonly byte[] fqPrefix = FqWire.Utf8.GetBytes(" << quote(e.prefix) << ");\n"
          << "    private int FqSize(bool capacity) {int n=fqUnknown.Length;checked {\n";
        for (const auto &f : e.fields) {
            auto v = "this." + f.name;
            o << "        n += FqWire.VarintSize(" << tag(f) << "u) + ";
            if (f.type == "string") o << "(capacity ? FqWire.BytesSize(checked(" << v << ".Length*3)) : FqWire.StringSize(" << v << "))";
            else if (f.type == "bytes") o << "FqWire.BytesSize(" << v << ".Length)";
            else if (wire(f) == 1) o << "8";
            else if (wire(f) == 5) o << "4";
            else if (f.type == "bool") o << "1";
            else o << "FqWire.VarintSize(" << (signed_int(f) ? "FqWire.ZigZag(" + v + ")" : v) << ")";
            o << ";\n";
        }
        o << "    }return n;}\n    public int EncodedSize() => FqSize(false);\n"
          << "    public int EncodeTo(Span<byte> output) {int p=0;\n";
        for (const auto &f : e.fields) {
            auto v = "this." + f.name;
            o << "        p += FqWire.WriteVarint(output.Slice(p), " << tag(f) << "u);\n        p += ";
            if (f.type == "string") o << "FqWire.WriteString(output.Slice(p), " << v << ")";
            else if (f.type == "bytes") o << "FqWire.WriteBytes(output.Slice(p), " << v << ")";
            else if (f.type == "float") o << "FqWire.WriteSingle(output.Slice(p), " << v << ")";
            else if (f.type == "double") o << "FqWire.WriteDouble(output.Slice(p), " << v << ")";
            else if (f.type == "bool") o << "FqWire.WriteVarint(output.Slice(p), " << v << "?1u:0u)";
            else o << "FqWire.WriteVarint(output.Slice(p), " << (signed_int(f) ? "FqWire.ZigZag(" + v + ")" : v) << ")";
            o << ";\n";
        }
        o << "        fqUnknown.CopyTo(output.Slice(p));p=checked(p+fqUnknown.Length);if(p>16777216)throw new InvalidDataException(\"Entity exceeds 16 MiB\");return p;\n    }\n"
          << "    public byte[] FqEncode() {var b=new byte[EncodedSize()];EncodeTo(b);return b;}\n"
          << "    private static bool FqKnown(uint t) => ";
        for (size_t i = 0; i < e.fields.size(); ++i) o << (i ? " || " : "") << "t==" << tag(e.fields[i]) << "u";
        o << ";\n    public static " << e.name << " Decode(ReadOnlySpan<byte> source) {\n        if(source.Length>16777216)throw new InvalidDataException(\"Entity exceeds 16 MiB\");\n"
          << "        var v=new " << e.name << "();int p=0,unknown=0;\n        while(p<source.Length){int begin=p;uint t=FqWire.ReadTag(source,ref p);switch(t){\n";
        for (const auto &f : e.fields) o << "        case " << tag(f) << "u:v." << f.name << "=" << csread(f) << ";break;\n";
        o << "        default:FqWire.Skip(source,ref p,t);unknown=checked(unknown+p-begin);break;\n        }}\n        if(unknown!=0)v.fqUnknown=FqWire.CopyUnknown(source,unknown,FqKnown);return v;\n    }\n"
          << "    " << e.name << " " << contract << ".ReadEntity(ReadOnlySpan<byte> source) => Decode(source);\n"
          << "    ReadOnlySpan<byte> " << contract << ".KeyPrefix => fqPrefix;\n";
        const auto &k = key(e);
        auto kind = k.type == "string" ? "Text" : signed_int(k) ? "Signed" : "Unsigned";
        auto kt = k.type == "string" ? "string" : signed_int(k) ? "long" : "ulong";
        o << "    FqEntityKeyKind " << contract << ".KeyKind => FqEntityKeyKind." << kind << ";\n"
          << "    public static byte[] MakeKey(" << cstype(k) << " id) {int n=FqWire.KeySize(fqPrefix,(" << kt << ")id);FqEntityStorage.ValidateSize(n,0);var b=new byte[n];int count=FqWire.WriteKey(b,fqPrefix,(" << kt << ")id);return count==b.Length?b:b.AsSpan(0,count).ToArray();}\n";
        for (bool tx : {false, true}) {
            auto ret = tx ? "" : "return ";
            o << "    " << (tx ? "void" : "ulong") << " " << contract << ".WriteTo(global::FengquDB." << (tx ? "StoreTransaction" : "Store") << " store,ulong expiresAtMs" << (tx ? "" : ",bool forceReliable") << ") {\n"
              << "        " << kt << " id=(" << kt << ")this." << k.name << ";int kn=FqWire.KeySize(fqPrefix,id),vn=FqSize(true);\n"
              << "        if(checked(kn+vn)>512)vn=EncodedSize();FqEntityStorage.ValidateSize(kn,vn);byte[]? rented=null;\n"
              << "        Span<byte> b=checked(kn+vn)<=512?stackalloc byte[512]:(rented=ArrayPool<byte>.Shared.Rent(checked(kn+vn)));\n"
              << "        try{int k=FqWire.WriteKey(b.Slice(0,kn),fqPrefix,id);int v=EncodeTo(b.Slice(kn,vn));" << ret
              << "store.Put(b.Slice(0,k),b.Slice(kn,v),expiresAtMs" << (tx ? ");" : ",forceReliable).Version;") << "}finally{if(rented!=null)ArrayPool<byte>.Shared.Return(rented);}\n    }\n";
        }
        o << "}\n";
    }
    o << "}\n"; files["csharp/FengquDB.Models.g.cs"] = o.str();
}
}
