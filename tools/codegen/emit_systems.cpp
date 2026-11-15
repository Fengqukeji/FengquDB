#include "model.hpp"
#include "runtimes.hpp"
#include <sstream>
namespace codegen {
static std::string gotype(const Field &f) {
    if (f.type == "sint32") return "int32";
    if (f.type == "sint64") return "int64";
    if (f.type == "float") return "float32";
    if (f.type == "double") return "float64";
    return f.type == "bytes" ? "[]byte" : f.type;
}
void emit_go(const Model &m, Files &files) {
    std::ostringstream o;
    o << banner << "package " << m.ns.substr(m.ns.find_last_of('.') == std::string::npos ? 0 : m.ns.find_last_of('.') + 1) << "\n" << runtime_go << "\n";
    for (const auto &e : m.entities) {
        o << "\ntype " << e.name << " struct {\n";
        for (const auto &f : e.fields) o << "    " << upper(f.name) << " " << gotype(f) << "\n";
        o << "    FqUnknown []byte\n}\nfunc (v *" << e.name << ") FqEncode() ([]byte,error) {return v.FqEncodeInto(nil)}\n"
          << "func (v *" << e.name << ") FqEncodeInto(dst []byte) ([]byte,error) {\n    begin:=len(dst)\n";
        for (const auto &f : e.fields) {
            auto v = "v." + upper(f.name);
            if (text(f)) o << "    if len(" << v << ")>16777216" << (f.type == "string" ? " || !utf8.ValidString(" + v + ")" : "") << " {return nil,fqModelError}\n";
            o << "    dst=fqAppendU(dst," << tag(f) << ")\n";
            if (text(f)) o << "    if fqBytesSize(len(" << v << "))>16777216-(len(dst)-begin) {return nil,fqModelError}\n";
            o << "    dst=";
            if (f.type == "string") o << "fqAppendS(dst," << v << ")";
            else if (f.type == "bytes") o << "fqAppendB(dst," << v << ")";
            else if (f.type == "bool") o << "fqAppendU(dst,fqBool(" << v << "))";
            else if (signed_int(f)) o << "fqAppendU(dst,fqZ(int64(" << v << ")))";
            else if (wire(f) == 0) o << "fqAppendU(dst,uint64(" << v << "))";
            else o << "fqAppendFixed(dst," << (f.type == "float" ? "fqFloat32(" : "fqFloat64(") << v << ")," << (f.type == "float" ? 4 : 8) << ")";
            o << "\n";
        }
        o << "    if len(v.FqUnknown)>16777216-(len(dst)-begin){return nil,fqModelError}\n    dst=append(dst,v.FqUnknown...)\n    if len(dst)-begin>16777216{return nil,fqModelError};return dst,nil\n}\n"
          << "func " << e.name << "FqDecode(source []byte) (" << e.name << ",error) {\n    var v " << e.name << "\n    if len(source)>16777216{return v,fqModelError}\n    r:=fqModelReader{data:source}\n"
          << "    for r.err==nil && r.pos<len(source) {begin:=r.pos;t:=r.tag();switch t {\n";
        for (const auto &f : e.fields) {
            o << "    case " << tag(f) << ":v." << upper(f.name) << "=";
            if (f.type == "string") o << "r.str()";
            else if (f.type == "bytes") o << "append([]byte(nil),r.b()...)";
            else if (f.type == "bool") o << "r.boolean()";
            else if (f.type == "sint32") o << "r.i32()";
            else if (f.type == "sint64") o << "fqUnz(r.u())";
            else if (f.type == "uint32") o << "r.u32()";
            else if (f.type == "uint64") o << "r.u()";
            else o << (f.type == "float" ? "math.Float32frombits(uint32(r.fixed(4)))" : "math.Float64frombits(r.fixed(8))");
            o << "\n";
        }
        o << "    default:r.skip(t,0);if r.err==nil{v.FqUnknown=append(v.FqUnknown,source[begin:r.pos]...)}\n    }}\n"
          << "    if r.err!=nil{return " << e.name << "{},r.err};return v,nil\n}\n"
          << "func (v *" << e.name << ") FqKey() ([]byte,error) {return fqKey(" << quote(e.prefix) << ",";
        const auto &k = key(e);
        if (k.type == "string") o << "v." << upper(k.name);
        else o << (signed_int(k) ? "fqSignedKey(int64(v." : "fqUnsignedKey(uint64(v.") << upper(k.name) << "))";
        o << ")}\n";
    }
    files["go/fq_models.go"] = o.str();
}
static std::string rstype(const Field &f) {
    if (f.type == "sint32") return "i32";
    if (f.type == "sint64") return "i64";
    if (f.type == "uint32") return "u32";
    if (f.type == "uint64") return "u64";
    if (f.type == "float") return "f32";
    if (f.type == "double") return "f64";
    if (f.type == "string") return "String";
    return f.type == "bytes" ? "Vec<u8>" : f.type;
}
void emit_rust(const Model &m, Files &files) {
    std::ostringstream o;
    o << banner << "#![allow(dead_code)]\n" << runtime_rust << "\n";
    for (const auto &e : m.entities) {
        o << "\n#[derive(Clone, Debug, Default, PartialEq)]\npub struct " << e.name << " {\n";
        for (const auto &f : e.fields) o << "    pub " << f.name << ": " << rstype(f) << ",\n";
        o << "    pub fq_unknown: Vec<u8>,\n}\nimpl " << e.name << " {\n"
          << "    pub fn fq_encode(&self)->FqResult<Vec<u8>> {let mut out=Vec::new();self.fq_encode_into(&mut out)?;Ok(out)}\n"
          << "    pub fn fq_encode_into(&self,out:&mut Vec<u8>)->FqResult<()> {\n        let start=out.len();let mut w=FqWriter{data:out,start};\n";
        for (const auto &f : e.fields) {
            auto v = "self." + f.name;
            o << "        w.u(" << tag(f) << ")?;";
            if (f.type == "string") o << "w.b(" << v << ".as_bytes())?;";
            else if (f.type == "bytes") o << "w.b(&" << v << ")?;";
            else if (signed_int(f)) o << "w.u(fq_z(" << v << " as i64))?;";
            else if (wire(f) == 0) o << "w.u(" << v << " as u64)?;";
            else o << "w.raw(&" << v << ".to_bits().to_le_bytes())?;";
            o << "\n";
        }
        o << "        w.raw(&self.fq_unknown)?;Ok(())\n    }\n"
          << "    pub fn fq_decode(source:&[u8])->FqResult<Self> {let mut r=FqReader::new(source)?;let mut v=Self::default();\n"
          << "        while r.pos<source.len(){let begin=r.pos;let t=r.tag()?;match t {\n";
        for (const auto &f : e.fields) {
            o << "        " << tag(f) << "=>v." << f.name << "=";
            if (f.type == "string") o << "std::str::from_utf8(r.b()?).map_err(|_|FqModelError)?.to_owned()";
            else if (f.type == "bytes") o << "r.b()?.to_vec()";
            else if (f.type == "bool") o << "match r.u()? {0=>false,1=>true,_=>return Err(FqModelError)}";
            else if (f.type == "sint32") o << "i32::try_from(fq_unz(r.u()?)).map_err(|_|FqModelError)?";
            else if (f.type == "sint64") o << "fq_unz(r.u()?)";
            else if (f.type == "uint32") o << "u32::try_from(r.u()?).map_err(|_|FqModelError)?";
            else if (f.type == "uint64") o << "r.u()?";
            else o << (f.type == "float" ? "f32::from_bits(r.fixed32()?)" : "f64::from_bits(r.fixed64()?)");
            o << ",\n";
        }
        o << "        _=>{r.skip(t,0)?;v.fq_unknown.extend_from_slice(&source[begin..r.pos]);}\n        }}Ok(v)\n    }\n"
          << "    pub fn fq_key(&self)->FqResult<Vec<u8>> {fq_key(" << quote(e.prefix) << ",&self." << key(e).name << (key(e).type == "string" ? "" : ".to_string()") << ")}\n}\n";
    }
    files["rust/fq_models.rs"] = o.str();
}
}
