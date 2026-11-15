#include "runtimes.hpp"
#include "source.hpp"
#include <sstream>
namespace codegen {
void source_systems(const SourceGroup &g, Files &files, const std::string &) {
    auto lang = g.entities.begin()->second.language;
    std::ostringstream o;
    if (lang == "go") {
        o << banner << "package fqmodels\n";
        auto runtime = std::string(runtime_go);
        auto p = runtime.find("import (") + 8;
        runtime.insert(p, "\n    fqsrc " + quote(g.entities.begin()->second.module));
        o << runtime << "\n";
        for (const auto &e : g.model.entities) {
            const auto &b = g.entities.at(e.name);
            auto type = "fqsrc." + e.name, rec = e.name + "FqRecord";
            auto member = [&](const Field &f) { return b.fields.at(f.name).member; };
            o << "type " << rec << " struct {Value " << type << ";Unknown []byte}\n"
              << "func (r *" << rec << ") FqEncode() ([]byte,error){return " << e.name
              << "FqEncodeInto(&r.Value,nil,r.Unknown)}\n"
              << "func (r *" << rec << ") FqKey() ([]byte,error){return " << e.name
              << "FqKey(&r.Value)}\n"
              << "func " << e.name << "FqEncode(v *" << type << ")([]byte,error){return " << e.name
              << "FqEncodeInto(v,nil,nil)}\n"
              << "func " << e.name << "FqEncodeInto(v *" << type
              << ",dst,unknown []byte)([]byte,error){begin:=len(dst)\n";
            for (const auto &f : e.fields) {
                auto v = "v." + member(f);
                o << "    dst=fqAppendU(dst," << tag(f) << ")\n";
                if (text(f))
                    o << "    if len(" << v << ")>16777216 || fqBytesSize(len(" << v
                      << "))>16777216-(len(dst)-begin)"
                      << (f.type == "string" ? " || !utf8.ValidString(" + v + ")" : "")
                      << " {return nil,fqModelError}\n";
                o << "    dst=";
                if (f.type == "string")
                    o << "fqAppendS(dst," << v << ")";
                else if (f.type == "bytes")
                    o << "fqAppendB(dst," << v << ")";
                else if (f.type == "bool")
                    o << "fqAppendU(dst,fqBool(" << v << "))";
                else if (signed_int(f))
                    o << "fqAppendU(dst,fqZ(int64(" << v << ")))";
                else if (wire(f) == 0)
                    o << "fqAppendU(dst,uint64(" << v << "))";
                else
                    o << "fqAppendFixed(dst," << (f.type == "float" ? "fqFloat32(" : "fqFloat64(")
                      << v << ")," << (f.type == "float" ? 4 : 8) << ")";
                o << "\n";
            }
            o << "    if len(unknown)>16777216-(len(dst)-begin){return "
                 "nil,fqModelError};dst=append(dst,unknown...);return dst,nil\n}\n"
              << "func " << e.name << "FqDecode(data []byte)(" << rec << ",error){var out " << rec
              << ";if len(data)>16777216{return out,fqModelError};r:=fqModelReader{data:data};\n"
              << "    for r.err==nil && r.pos<len(data){begin:=r.pos;t:=r.tag();switch t {\n";
            for (const auto &f : e.fields) {
                o << "    case " << tag(f) << ":out.Value." << member(f) << "=";
                if (f.type == "string")
                    o << "r.str()";
                else if (f.type == "bytes")
                    o << "append([]byte(nil),r.b()...)";
                else if (f.type == "bool")
                    o << "r.boolean()";
                else if (f.type == "sint32")
                    o << "r.i32()";
                else if (f.type == "sint64")
                    o << "fqUnz(r.u())";
                else if (f.type == "uint32")
                    o << "r.u32()";
                else if (f.type == "uint64")
                    o << "r.u()";
                else
                    o << (f.type == "float" ? "math.Float32frombits(uint32(r.fixed(4)))"
                                            : "math.Float64frombits(r.fixed(8))");
                o << "\n";
            }
            o << "    default:r.skip(t,0);if "
                 "r.err==nil{out.Unknown=append(out.Unknown,data[begin:r.pos]...)}\n    }}\n    if "
                 "r.err!=nil{return "
              << rec << "{},r.err};return out,nil}\n"
              << "func " << e.name << "FqKey(v *" << type << ")([]byte,error){return fqKey("
              << quote(e.prefix) << ",";
            const auto &k = key(e);
            if (k.type == "string")
                o << "v." << member(k);
            else
                o << (signed_int(k) ? "fqSignedKey(int64(v." : "fqUnsignedKey(uint64(v.")
                  << member(k) << "))";
            o << ")}\n";
        }
    } else {
        // Keep helpers at the same scope as their generated use, without exposing
        // runtime internals to the application.
        o << banner << "// Include this file in its own module.\n" << runtime_rust << "\n";
        for (const auto &e : g.model.entities) {
            const auto &b = g.entities.at(e.name);
            auto type = b.qualified, rec = e.name + "FqRecord", codec = e.name + "FqCodec";
            auto member = [&](const Field &f) { return b.fields.at(f.name).member; };
            o << "pub struct " << rec << " {pub value:" << type << ",unknown:Vec<u8>}\nimpl " << rec
              << " {pub fn fq_encode(&self)->FqResult<Vec<u8>>{let mut b=Vec::new();" << codec
              << "::encode_into(&self.value,&mut b,&self.unknown)?;Ok(b)}pub fn "
                 "fq_key(&self)->FqResult<Vec<u8>>{"
              << codec << "::key(&self.value)}}\n"
              << "pub struct " << codec << ";\nimpl " << codec << " {\n    pub fn encode(v:&"
              << type
              << ")->FqResult<Vec<u8>>{let mut b=Vec::new();Self::encode_into(v,&mut "
                 "b,&[])?;Ok(b)}\n"
              << "    pub fn encode_into(v:&" << type
              << ",out:&mut Vec<u8>,unknown:&[u8])->FqResult<()> {let start=out.len();let mut "
                 "w=FqWriter{data:out,start};\n";
            for (const auto &f : e.fields) {
                auto v = "v." + member(f);
                o << "        w.u(" << tag(f) << ")?;";
                if (f.type == "string")
                    o << "w.b(" << v << ".as_bytes())?;";
                else if (f.type == "bytes")
                    o << "w.b(&" << v << ")?;";
                else if (signed_int(f))
                    o << "w.u(fq_z(" << v << " as i64))?;";
                else if (wire(f) == 0)
                    o << "w.u(" << v << " as u64)?;";
                else
                    o << "w.raw(&" << v << ".to_bits().to_le_bytes())?;";
                o << "\n";
            }
            o << "        w.raw(unknown)?;Ok(())}\n    pub fn decode(data:&[u8])->FqResult<" << rec
              << ">{let mut r=FqReader::new(data)?;let mut v=" << type
              << "::default();let mut unknown=Vec::new();\n"
              << "        while r.pos<data.len(){let begin=r.pos;let t=r.tag()?;match t {\n";
            for (const auto &f : e.fields) {
                o << "        " << tag(f) << "=>v." << member(f) << "=";
                if (f.type == "string")
                    o << "std::str::from_utf8(r.b()?).map_err(|_|FqModelError)?.to_owned()";
                else if (f.type == "bytes")
                    o << "r.b()?.to_vec()";
                else if (f.type == "bool")
                    o << "match r.u()?{0=>false,1=>true,_=>return Err(FqModelError)}";
                else if (f.type == "sint32")
                    o << "i32::try_from(fq_unz(r.u()?)).map_err(|_|FqModelError)?";
                else if (f.type == "sint64")
                    o << "fq_unz(r.u()?)";
                else if (f.type == "uint32")
                    o << "u32::try_from(r.u()?).map_err(|_|FqModelError)?";
                else if (f.type == "uint64")
                    o << "r.u()?";
                else
                    o << (f.type == "float" ? "f32::from_bits(r.fixed32()?)"
                                            : "f64::from_bits(r.fixed64()?)");
                o << ",\n";
            }
            o << "        _=>{r.skip(t,0)?;unknown.extend_from_slice(&data[begin..r.pos]);}}}Ok("
              << rec << "{value:v,unknown})}\n"
              << "    pub fn key(v:&" << type << ")->FqResult<Vec<u8>>{fq_key(" << quote(e.prefix)
              << ",&v." << member(key(e)) << (key(e).type == "string" ? "" : ".to_string()")
              << ")}\n}\n";
        }
    }
    files[lang + "/" + replace(g.model.ns, ".", "/") + "/fq_codecs." +
          (lang == "go" ? "go" : "rs")] = o.str();
}
} // namespace codegen
