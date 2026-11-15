#include "model.hpp"
#include "runtimes.hpp"
#include <sstream>
namespace codegen {
static std::string ctype(const Field &f) {
    if (f.type == "sint32") return "int32_t";
    if (f.type == "sint64") return "int64_t";
    if (f.type == "uint32") return "uint32_t";
    if (f.type == "uint64") return "uint64_t";
    if (f.type == "bool") return "uint8_t";
    return text(f) ? "fqg_slice" : f.type;
}
void emit_native(const Model &m, Files &files, bool cpp) {
    std::ostringstream c;
    const auto guard = "FQDB_MODELS_" + replace(m.ns, ".", "_") + "_H";
    c << banner << "#ifndef " << guard << "\n#define " << guard << "\n" << runtime_c << "\n";
    const auto pre = replace(m.ns, ".", "_") + "_";
    for (const auto &e : m.entities) {
        auto n = pre + e.name;
        c << "typedef struct " << n << " {\n";
        for (const auto &f : e.fields) c << "    " << (wire(f) == 1 || wire(f) == 5 ? ctype(f) : "FQG_TYPE(" + ctype(f) + ")") << " " << f.name << ";\n";
        c << "    fqg_slice fq_unknown_source;\n} " << n << ";\n";
        c << "static inline int " << n << "_fq_known(uint32_t t) { return ";
        for (size_t i = 0; i < e.fields.size(); ++i) c << (i ? " || " : "") << "t == " << tag(e.fields[i]) << "u";
        c << "; }\nstatic inline int " << n << "_fq_encode(const " << n << " *v, void *out, size_t cap, size_t *written) {\n"
          << "    fqg_writer w = {(uint8_t*)out, 0, cap, 1};\n";
        for (const auto &f : e.fields) {
            auto v = "v->" + f.name;
            c << "    fqg_u(&w, " << tag(f) << "u);\n";
            if (text(f)) {
                if (f.type == "string") c << "    if (!fqg_utf8(" << v << ")) w.ok = 0;\n";
                c << "    fqg_u(&w, " << v << ".size); fqg_raw(&w, " << v << ".data, " << v << ".size);\n";
            } else if (wire(f) == 0) {
                if (f.type == "bool") c << "    if (" << v << " > 1) w.ok = 0;\n";
                c << "    fqg_u(&w, " << (signed_int(f) ? "fqg_z(" + v + ")" : v) << ");\n";
            } else {
                c << "    { " << (f.type == "float" ? "uint32_t" : "uint64_t") << " bits; memcpy(&bits, &" << v
                  << ", " << (f.type == "float" ? 4 : 8) << "); fqg_fixed(&w, bits, " << (f.type == "float" ? 4 : 8) << "); }\n";
            }
        }
        c << "    fqg_unknown(&w, v->fq_unknown_source, " << n << "_fq_known);\n"
          << "    *written = w.size; return w.ok && w.size <= 16777216;\n}\n"
          << "static inline int " << n << "_fq_decode(const void *data, size_t size, " << n << " *out) {\n"
          << "    " << n << " v; fqg_reader r = {(const uint8_t*)data, size, 0, size <= 16777216 && (size == 0 || data != NULL)};\n"
          << "    memset(&v, 0, sizeof(v));\n    while (r.ok && r.pos < r.size) {\n        uint32_t t = fqg_tag(&r);\n        switch (t) {\n";
        for (const auto &f : e.fields) {
            c << "        case " << tag(f) << "u: ";
            auto v = "v." + f.name;
            if (text(f)) {
                c << v << " = fqg_read_b(&r);";
                if (f.type == "string") c << " if (!fqg_utf8(" << v << ")) r.ok = 0;";
            } else if (wire(f) == 0) {
                c << "{ uint64_t x = fqg_read_u(&r); ";
                if (f.type == "sint32") c << "int64_t s = fqg_unz(x); if (s < INT32_MIN || s > INT32_MAX) r.ok = 0; ";
                if (f.type == "uint32") c << "if (x > UINT32_MAX) r.ok = 0; ";
                if (f.type == "bool") c << "if (x > 1) r.ok = 0; ";
                c << v << " = (" << ctype(f) << ")" << (signed_int(f) ? "fqg_unz(x)" : "x") << "; }";
            } else {
                c << "{ " << (f.type == "float" ? "uint32_t" : "uint64_t") << " bits = (" << (f.type == "float" ? "uint32_t" : "uint64_t")
                  << ")fqg_read_fixed(&r, " << (f.type == "float" ? 4 : 8) << "); memcpy(&" << v << ", &bits, " << (f.type == "float" ? 4 : 8) << "); }";
            }
            c << " break;\n";
        }
        c << "        default: fqg_skip(&r, t, 0); v.fq_unknown_source.data = r.data; v.fq_unknown_source.size = size; break;\n"
          << "        }\n    }\n    if (!r.ok) return 0;\n    *out = v; return 1;\n}\n";
        auto k = key(e);
        c << "static inline int " << n << "_fq_key(const " << n << " *v, void *out, size_t cap, size_t *written) {\n"
          << "    fqg_writer w = {(uint8_t*)out, 0, cap, 1};\n"
          << "    fqg_raw(&w, " << quote(e.prefix) << ", " << e.prefix.size() << ");\n";
        if (k.type == "string") c << "    if (!fqg_utf8(v->" << k.name << ")) w.ok = 0;\n    fqg_raw(&w, v->" << k.name << ".data, v->" << k.name << ".size);\n";
        else if (signed_int(k)) c << "    { int64_t x = v->" << k.name << "; fqg_key_number(&w, x < 0 ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x, x < 0); }\n";
        else c << "    fqg_key_number(&w, v->" << k.name << ", 0);\n";
        c << "    *written = w.size; return w.ok && w.size <= 65536;\n}\n";
    }
    c << "#endif\n";
    files[cpp ? "cpp/fq_models.h" : "c/fq_models.h"] = c.str();
    if (!cpp) return;
    std::ostringstream o;
    o << banner << "#pragma once\n#include \"fq_models.h\"\n#include <string>\n#include <string_view>\n#include <stdexcept>\n#include <limits>\n"
      << "static_assert(sizeof(float) == 4 && sizeof(double) == 8 && std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559, \"IEEE floats required\");\n"
      << "namespace " << replace(m.ns, ".", "::") << " {\n";
    for (const auto &e : m.entities) {
        auto n = pre + e.name;
        o << "struct " << e.name << " {\n";
        for (const auto &f : e.fields) o << "    " << (text(f) ? "std::string" : (wire(f) == 0 ? "::" : "") + ctype(f)) << " " << f.name << "{};\n";
        o << "private:\n    std::string fq_unknown_source;\n    ::" << n << " fq_view() const {\n        ::" << n << " v{};\n";
        for (const auto &f : e.fields) {
            o << "        v." << f.name << " = ";
            if (text(f)) o << "{reinterpret_cast<const ::uint8_t*>(this->" << f.name << ".data()), this->" << f.name << ".size()}";
            else o << "this->" << f.name;
            o << ";\n";
        }
        o << "        v.fq_unknown_source = {reinterpret_cast<const ::uint8_t*>(fq_unknown_source.data()), fq_unknown_source.size()}; return v;\n    }\npublic:\n"
          << "    ::size_t fq_encode_into(void *data, ::size_t capacity) const {\n        auto v = fq_view(); ::size_t n; if (!::" << n << "_fq_encode(&v, data, capacity, &n)) throw std::invalid_argument(\"Invalid entity or insufficient buffer\"); return n;\n    }\n"
          << "    ::size_t fq_encoded_size() const { return fq_encode_into(nullptr, SIZE_MAX); }\n"
          << "    void fq_encode_into(std::string &out) const { out.resize(fq_encoded_size()); fq_encode_into(out.data(), out.size()); }\n"
          << "    std::string fq_encode() const { std::string out; fq_encode_into(out); return out; }\n"
          << "    std::string fq_key() const { auto v = fq_view(); ::size_t n; if (!::" << n << "_fq_key(&v, nullptr, SIZE_MAX, &n)) throw std::invalid_argument(\"Invalid key\"); std::string out(n, '\\0'); ::" << n << "_fq_key(&v, out.data(), out.size(), &n); return out; }\n"
          << "    static " << e.name << " fq_decode(std::string_view data) {\n        ::" << n << " v{}; if (!::" << n << "_fq_decode(data.data(), data.size(), &v)) throw std::invalid_argument(\"Invalid entity wire data\");\n        " << e.name << " out;\n";
        for (const auto &f : e.fields) {
            if (text(f)) o << "        if (v." << f.name << ".size) out." << f.name << ".assign(reinterpret_cast<const char*>(v." << f.name << ".data), v." << f.name << ".size);\n";
            else o << "        out." << f.name << " = v." << f.name << ";\n";
        }
        o << "        if (v.fq_unknown_source.size) out.fq_unknown_source.assign(data);\n        return out;\n    }\n};\n";
    }
    o << "}\n"; files["cpp/fq_models.hpp"] = o.str();
}
}
