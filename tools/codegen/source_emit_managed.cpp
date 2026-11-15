#include "runtimes.hpp"
#include "source.hpp"
#include <sstream>
namespace codegen {
static void source_cs(const SourceGroup &g, Files &files) {
    std::ostringstream o;
    const auto ns = g.entities.begin()->second.module;
    o << banner << "#nullable enable\nusing System;\nusing System.IO;\nusing FengquDB.Entities;\n"
      << (ns.empty() ? "" : "namespace " + ns + " {\n");
    for (const auto &e : g.model.entities) {
        const auto &b = g.entities.at(e.name);
        auto type = b.qualified, codec = e.name + "FqCodec", record = e.name + "FqRecord";
        auto member = [&](const Field &f) { return b.fields.at(f.name).member; };
        o << "public sealed class " << record << " {public " << type
          << " Value {get;}internal readonly byte[] Unknown;internal " << record << "(" << type
          << " value,byte[] unknown){Value=value;Unknown=unknown;}public byte[] Encode()=>" << codec
          << ".Encode(Value,Unknown);public byte[] Key()=>" << codec << ".Key(Value);}\n"
          << "public static class " << codec << " {\n    public static int EncodedSize(" << type
          << " value,int unknownSize=0){int n=unknownSize;checked{\n";
        for (const auto &f : e.fields) {
            auto v = "value." + member(f);
            o << "        n+=FqWire.VarintSize(" << tag(f) << "u)+";
            if (f.type == "string")
                o << "FqWire.StringSize(" << v << ")";
            else if (f.type == "bytes")
                o << "FqWire.BytesSize(" << v << ".Length)";
            else if (f.type == "bool")
                o << "1";
            else if (wire(f) == 1)
                o << "8";
            else if (wire(f) == 5)
                o << "4";
            else
                o << "FqWire.VarintSize(" << (signed_int(f) ? "FqWire.ZigZag(" + v + ")" : v)
                  << ")";
            o << ";\n";
        }
        o << "    }if(n>16777216)throw new InvalidDataException(\"Entity exceeds 16 MiB\");return "
             "n;}\n"
          << "    public static byte[] Encode(" << type
          << " value,ReadOnlySpan<byte> unknown=default){var b=new "
             "byte[EncodedSize(value,unknown.Length)];EncodeInto(value,b,unknown);return b;}\n"
          << "    public static int EncodeInto(" << type
          << " value,Span<byte> output,ReadOnlySpan<byte> unknown=default){int p=0;\n";
        for (const auto &f : e.fields) {
            auto v = "value." + member(f);
            o << "        p+=FqWire.WriteVarint(output.Slice(p)," << tag(f) << "u);p+=";
            if (f.type == "string")
                o << "FqWire.WriteString(output.Slice(p)," << v << ")";
            else if (f.type == "bytes")
                o << "FqWire.WriteBytes(output.Slice(p)," << v << ")";
            else if (f.type == "float")
                o << "FqWire.WriteSingle(output.Slice(p)," << v << ")";
            else if (f.type == "double")
                o << "FqWire.WriteDouble(output.Slice(p)," << v << ")";
            else
                o << "FqWire.WriteVarint(output.Slice(p),"
                  << (f.type == "bool" ? v + "?1u:0u"
                      : signed_int(f)  ? "FqWire.ZigZag(" + v + ")"
                                       : v)
                  << ")";
            o << ";\n";
        }
        o << "        "
             "unknown.CopyTo(output.Slice(p));p=checked(p+unknown.Length);if(p>16777216)throw new "
             "InvalidDataException(\"Entity exceeds 16 MiB\");return p;}\n"
          << "    static bool Known(uint t)=>";
        for (size_t i = 0; i < e.fields.size(); ++i)
            o << (i ? "||" : "") << "t==" << tag(e.fields[i]) << "u";
        o << ";\n"
          << "    public static " << record
          << " Decode(ReadOnlySpan<byte> source){if(source.Length>16777216)throw new "
             "InvalidDataException(\"Entity exceeds 16 MiB\");int p=0,unknown=0;\n";
        for (const auto &f : e.fields)
            o << "        " << b.fields.at(f.name).native_type << " v_" << f.name << "="
              << (f.type == "string"  ? "string.Empty"
                  : f.type == "bytes" ? "Array.Empty<byte>()"
                                      : "default")
              << ";\n";
        o << "        while(p<source.Length){int begin=p;uint t=FqWire.ReadTag(source,ref "
             "p);switch(t){\n";
        for (const auto &f : e.fields) {
            o << "        case " << tag(f) << "u:v_" << f.name << "=";
            if (f.type == "string")
                o << "FqWire.Utf8.GetString(FqWire.ReadBytes(source,ref p))";
            else if (f.type == "bytes")
                o << "FqWire.ReadBytes(source,ref p).ToArray()";
            else if (f.type == "bool")
                o << "FqWire.ReadBool(source,ref p)";
            else if (f.type == "float")
                o << "FqWire.ReadSingle(source,ref p)";
            else if (f.type == "double")
                o << "FqWire.ReadDouble(source,ref p)";
            else
                o << "checked((" << b.fields.at(f.name).native_type << ")"
                  << (signed_int(f) ? "FqWire.UnZigZag(FqWire.ReadVarint(source,ref p))"
                                    : "FqWire.ReadVarint(source,ref p)")
                  << ")";
            o << ";break;\n";
        }
        o << "        default:FqWire.Skip(source,ref "
             "p,t);unknown=checked(unknown+p-begin);break;}}\n        var value=new "
          << type << "{";
        for (size_t i = 0; i < e.fields.size(); ++i)
            o << (i ? "," : "") << member(e.fields[i]) << "=v_" << e.fields[i].name;
        o << "};return new " << record
          << "(value,unknown==0?Array.Empty<byte>():FqWire.CopyUnknown(source,unknown,Known));}\n"
          << "    static readonly byte[] Prefix=FqWire.Utf8.GetBytes(" << quote(e.prefix)
          << ");\n    public static byte[] Key(" << type << " value){";
        auto k = key(e);
        auto kt = k.type == "string" ? "string" : signed_int(k) ? "long" : "ulong";
        o << kt << " id=(" << kt << ")value." << member(k)
          << ";int n=FqWire.KeySize(Prefix,id);FqEntityStorage.ValidateSize(n,0);var b=new "
             "byte[n];int count=FqWire.WriteKey(b,Prefix,id);return "
             "b.AsSpan(0,count).ToArray();}\n}\n";
    }
    if (!ns.empty())
        o << "}\n";
    files["csharp/" + replace(g.model.ns, ".", "/") + "/fq_codecs.cs"] = o.str();
}
void source_managed(const SourceGroup &g, Files &files, const std::string &) {
    if (g.entities.begin()->second.language == "csharp") {
        source_cs(g, files);
        return;
    }
    auto dir = "java/" + replace(g.model.ns, ".", "/") + "/";
    const auto ns = g.entities.begin()->second.module;
    const auto package = ns.empty() ? std::string{} : "package " + ns + ";\n";
    files[dir + "FqModelWire.java"] = banner + package + runtime_java;
    for (const auto &e : g.model.entities) {
        const auto &b = g.entities.at(e.name);
        auto n = e.name + "FqCodec";
        std::ostringstream o;
        auto member = [&](const Field &f) { return b.fields.at(f.name).member; };
        o << banner << package
          << "import java.io.ByteArrayOutputStream;\nimport "
             "java.nio.charset.StandardCharsets;\npublic final class "
          << n << " {\n"
          << "    private " << n
          << "(){}\n    private static final byte[] EMPTY=new byte[0];\n    private static final "
             "byte[] PREFIX=FqModelWire.text("
          << quote(e.prefix) << ");\n"
          << "    public static final class Record {\n        public final " << e.name
          << " value; private final byte[] unknown;\n"
          << "        private Record(" << e.name
          << " value,byte[] unknown){this.value=value;this.unknown=unknown;}\n"
          << "        public byte[] fqEncode(){return encode(value,unknown);}\n        public "
             "byte[] fqKey(){return key(value);}\n    }\n"
          << "    private static void write(" << e.name
          << " value,FqModelWire.Writer w,byte[] unknown){\n";
        for (const auto &f : e.fields) {
            const auto &source = b.fields.at(f.name);
            auto v = "value." + (source.getter.empty() ? member(f) : source.getter + "()");
            o << "        w.u(" << tag(f) << "L);";
            if (text(f))
                o << "w.b(" << (f.type == "string" ? "FqModelWire.text(" + v + ")" : v) << ");";
            else if (f.type == "bool")
                o << "w.u(" << v << "?1:0);";
            else if (signed_int(f))
                o << "w.u(FqModelWire.z(" << v << "));";
            else if (f.type == "uint32")
                o << "w.u(FqModelWire.u32(" << v << "));";
            else if (wire(f) == 0)
                o << "w.u(" << v << ");";
            else
                o << "w.fixed("
                  << (f.type == "float" ? "Float.floatToRawIntBits("
                                        : "Double.doubleToRawLongBits(")
                  << v << ")," << (f.type == "float" ? 4 : 8) << ");";
            o << "\n";
        }
        o << "        w.raw(unknown);\n    }\n    public static byte[] encode(" << e.name
          << " value){return encode(value,EMPTY);}\n"
          << "    private static byte[] encode(" << e.name
          << " value,byte[] unknown){FqModelWire.Writer w=new "
             "FqModelWire.Writer(null,0);write(value,w,unknown);return w.finish();}\n"
          << "    public static int encodeInto(" << e.name
          << " value,byte[] destination,int offset){if(destination==null)throw new "
             "NullPointerException();FqModelWire.Writer w=new "
             "FqModelWire.Writer(destination,offset);write(value,w,EMPTY);return "
             "w.pos-offset;}\n"
          << "    public static Record decode(byte[] data){FqModelWire.Reader r=new "
             "FqModelWire.Reader(data);"
          << e.name << " value=new " << e.name << "();ByteArrayOutputStream unknown=null;\n"
          << "        while(r.pos<data.length){int begin=r.pos,t=r.tag();switch(t){\n";
        for (const auto &f : e.fields) {
            const auto &source = b.fields.at(f.name);
            o << "        case " << static_cast<int32_t>(tag(f)) << ":value."
              << (source.setter.empty() ? member(f) + "=" : source.setter + "(");
            if (f.type == "string")
                o << "FqModelWire.text(r.b())";
            else if (f.type == "bytes")
                o << "r.b()";
            else if (f.type == "bool")
                o << "FqModelWire.bool(r.u())";
            else if (f.type == "sint32")
                o << "FqModelWire.i32(FqModelWire.unz(r.u()))";
            else if (f.type == "sint64")
                o << "FqModelWire.unz(r.u())";
            else if (f.type == "uint32")
                o << "FqModelWire.u32(r.u())";
            else if (f.type == "uint64")
                o << "r.u()";
            else
                o << (f.type == "float" ? "Float.intBitsToFloat((int)r.fixed(4))"
                                        : "Double.longBitsToDouble(r.fixed(8))");
            o << (source.setter.empty() ? ";break;\n" : ");break;\n");
        }
        o << "        default:r.skip(t,0);if(unknown==null)unknown=new "
             "ByteArrayOutputStream();unknown.write(data,begin,r.pos-begin);break;}}\n"
          << "        return new Record(value,unknown==null?EMPTY:unknown.toByteArray());\n  "
             "  }\n    public static byte[] key("
          << e.name << " value){return FqModelWire.key(PREFIX,";
        const auto &k = key(e);
        const auto &source = b.fields.at(k.name);
        auto v = "value." + (source.getter.empty() ? member(k) : source.getter + "()");
        if (k.type == "string")
            o << "FqModelWire.text(" << v << ")";
        else
            o << "Long." << (k.type == "uint64" ? "toUnsignedString" : "toString") << "(" << v
              << ").getBytes(StandardCharsets.US_ASCII)";
        o << ");}\n}\n";
        files[dir + n + ".java"] = o.str();
    }
}
} // namespace codegen
