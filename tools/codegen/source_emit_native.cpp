#include "source.hpp"
#include <filesystem>
#include <sstream>
namespace codegen {
void source_native(const SourceGroup &g, Files &files, const std::string &base) {
    auto lang = g.entities.begin()->second.language;
    auto dir = lang + "/" + replace(g.model.ns, ".", "/") + "/";
    Files raw;
    emit_native(g.model, raw, false);
    files[dir + "fq_wire_models.h"] = raw.at("c/fq_models.h");
    std::ostringstream o;
    o << banner << "#pragma once\n#include \"fq_wire_models.h\"\n";
    std::set<std::string> includes;
    for (const auto &entry : g.entities)
        includes.insert(
            std::filesystem::relative(std::filesystem::u8path(entry.second.include_path),
                                      std::filesystem::u8path(base) / std::filesystem::u8path(dir))
                .generic_u8string());
    for (const auto &path : includes)
        o << "#include " << quote(path) << "\n";
    if (lang == "cpp")
        o << "#include <string>\n#include <string_view>\n#include <stdexcept>\nnamespace "
             "fqdb_generated::"
          << replace(g.model.ns, ".", "::") << " {\n";
    auto prefix = replace(g.model.ns, ".", "_") + "_";
    for (const auto &e : g.model.entities) {
        const auto &b = g.entities.at(e.name);
        auto rawtype = prefix + e.name, codec = e.name + "FqCodec", record = e.name + "FqRecord";
        auto member = [&](const Field &f) { return b.fields.at(f.name).member; };
        if (lang == "cpp") {
            o << "struct " << record << " {" << b.qualified
              << " value;std::string unknown_source;};\nstruct " << codec << " {\n"
              << "    static ::" << rawtype << " view(const " << b.qualified
              << " &value,std::string_view unknown={}) {::" << rawtype << " v{};\n";
            for (const auto &f : e.fields) {
                o << "        v." << f.name << "=";
                if (text(f))
                    o << "{reinterpret_cast<const uint8_t*>(value." << member(f)
                      << ".data()),value." << member(f) << ".size()}";
                else
                    o << "value." << member(f);
                o << ";\n";
            }
            o << "        v.fq_unknown_source={reinterpret_cast<const "
                 "uint8_t*>(unknown.data()),unknown.size()};return v;}\n"
              << "    static size_t encode_into(const " << b.qualified
              << " &value,void *out,size_t capacity,std::string_view unknown={}) {auto "
                 "v=view(value,unknown);size_t n;if(!::"
              << rawtype
              << "_fq_encode(&v,out,capacity,&n))throw std::invalid_argument(\"Invalid "
                 "entity/buffer\");return n;}\n"
              << "    static void encode_into(const " << b.qualified
              << " &value,std::string &out,std::string_view unknown={}) {size_t "
                 "n=encode_into(value,nullptr,SIZE_MAX,unknown);out.resize(n);encode_into(value,"
                 "out.data(),n,unknown);}\n"
              << "    static std::string encode(const " << b.qualified
              << " &value,std::string_view unknown={}) {std::string "
                 "out;encode_into(value,out,unknown);return out;}\n"
              << "    static std::string encode(const " << record
              << " &record){return encode(record.value,record.unknown_source);}\n"
              << "    static " << record << " decode(std::string_view data){::" << rawtype
              << " v{};if(!::" << rawtype
              << "_fq_decode(data.data(),data.size(),&v))throw std::invalid_argument(\"Invalid "
                 "entity bytes\");"
              << record << " record{};\n";
            for (const auto &f : e.fields) {
                o << "        ";
                if (text(f)) {
                    auto ptr = "v." + f.name + ".data", size = "v." + f.name + ".size";
                    o << "if(" << size << ")record.value." << member(f) << ".assign(";
                    if (f.type == "bytes")
                        o << ptr << "," << ptr << "+" << size;
                    else
                        o << "reinterpret_cast<const char*>(" << ptr << ")," << size;
                    o << ");";
                } else
                    o << "record.value." << member(f) << "=v." << f.name << ";";
                o << "\n";
            }
            o << "        if(v.fq_unknown_source.size)record.unknown_source.assign(data);return "
                 "record;}\n"
              << "    static std::string key(const " << b.qualified
              << " &value){auto v=view(value);size_t n;if(!::" << rawtype
              << "_fq_key(&v,nullptr,SIZE_MAX,&n))throw std::invalid_argument(\"Invalid "
                 "key\");std::string out(n,'\\0');::"
              << rawtype << "_fq_key(&v,out.data(),n,&n);return out;}\n};\n";
        } else {
            auto n = prefix + codec, r = prefix + record;
            o << "typedef struct {" << b.qualified << " value;fqg_slice unknown_source;} " << r
              << ";\n"
              << "static inline " << rawtype << " " << n << "_view(const " << b.qualified
              << " *value,fqg_slice unknown){" << rawtype << " v;memset(&v,0,sizeof(v));\n";
            for (const auto &f : e.fields) {
                if (text(f))
                    o << "    v." << f.name << ".data=(const uint8_t*)value->" << member(f)
                      << ".data;v." << f.name << ".size=value->" << member(f) << ".size;\n";
                else
                    o << "    v." << f.name << "=value->" << member(f) << ";\n";
            }
            o << "    v.fq_unknown_source=unknown;return v;}\n"
              << "static inline int " << n << "_encode(const " << b.qualified
              << " *value,fqg_slice unknown,void *out,size_t capacity,size_t *written){" << rawtype
              << " v=" << n << "_view(value,unknown);return " << rawtype
              << "_fq_encode(&v,out,capacity,written);}\n"
              << "static inline int " << n << "_decode(const void *data,size_t size," << r
              << " *out){" << rawtype << " v;" << r << " r;if(!" << rawtype
              << "_fq_decode(data,size,&v))return 0;memset(&r,0,sizeof(r));\n";
            for (const auto &f : e.fields) {
                if (text(f))
                    o << "    r.value." << member(f) << ".data=("
                      << (f.type == "string" ? "const char*" : "const uint8_t*") << ")v." << f.name
                      << ".data;r.value." << member(f) << ".size=v." << f.name << ".size;\n";
                else
                    o << "    r.value." << member(f) << "=v." << f.name << ";\n";
            }
            o << "    r.unknown_source=v.fq_unknown_source;*out=r;return 1;}\n"
              << "static inline int " << n << "_key(const " << b.qualified
              << " *value,void *out,size_t capacity,size_t *written){fqg_slice empty={NULL,0};"
              << rawtype << " v=" << n << "_view(value,empty);return " << rawtype
              << "_fq_key(&v,out,capacity,written);}\n";
        }
    }
    if (lang == "cpp")
        o << "}\n";
    files[dir + "fq_codecs." + (lang == "cpp" ? "hpp" : "h")] = o.str();
}
void emit_source(const SourceInput &source, Files &files) {
    for (const auto &group : source.groups) {
        const auto &binding = group.entities.begin()->second;
        if (binding.language == "java" || binding.language == "csharp")
            source_managed(group, files, "");
        else if (binding.language == "python" || binding.language == "node")
            source_dynamic(group, files, source.output_root);
        else if (binding.language == "go" || binding.language == "rust")
            source_systems(group, files, "");
        else
            source_native(group, files, source.output_root);
    }
    // Expose inferred numbers as compile-time metadata, so query code never
    // needs to read or manually copy the generated JSON.
    for (const auto &g : source.groups) {
        const auto lang = g.entities.begin()->second.language;
        const auto dir = lang + "/" + (lang == "python" ? "fqdb_generated/" : "") +
                         replace(g.model.ns, ".", "/") + "/";
        for (const auto &e : g.model.entities) {
            const auto &binding = g.entities.at(e.name);
            auto codec = e.name + "FqCodec";
            std::ostringstream fields;
            std::string path, marker;
            if (lang == "python") {
                path = dir + "fq_codecs.py";
                marker = "class " + codec + ":\n";
                fields << "    class Fields:\n";
            } else if (lang == "node") {
                path = dir + "fq_codecs.js";
                marker = "class " + codec + " {\n";
                fields << "  static fields=Object.freeze({";
            } else if (lang == "java") {
                path = dir + codec + ".java";
                marker = "public final class " + codec + " {\n";
            } else if (lang == "csharp") {
                path = dir + "fq_codecs.cs";
                marker = "public static class " + codec + " {\n";
            } else if (lang == "cpp") {
                path = dir + "fq_codecs.hpp";
                marker = "struct " + codec + " {\n";
            } else if (lang == "rust") {
                path = dir + "fq_codecs.rs";
                marker = "impl " + codec + " {\n";
            } else if (lang == "go") {
                path = dir + "fq_codecs.go";
                marker = "type " + e.name + "FqRecord struct";
            } else {
                path = dir + "fq_codecs.h";
                marker = "";
            }
            std::set<std::string> names;
            for (const auto &f : e.fields) {
                auto name = binding.fields.at(f.name).member;
                require(names.insert(lower(name)).second,
                        "Source fields cannot differ only by case");
                if (lang == "python")
                    fields << "        " << name << " = " << f.number << "\n";
                else if (lang == "node")
                    fields << quote(name) << ":" << f.number << ",";
                else if (lang == "java")
                    fields << "    public static final int Field_" << name << "=" << f.number
                           << ";\n";
                else if (lang == "csharp")
                    fields << "    public const uint Field_" << name << "=" << f.number << "u;\n";
                else if (lang == "cpp")
                    fields << "    static constexpr uint32_t Field_" << name << "=" << f.number
                           << "u;\n";
                else if (lang == "go")
                    fields << "const " << e.name << "Field" << upper(name) << " uint32=" << f.number
                           << "\n";
                else if (lang == "rust") {
                    for (auto &c : name)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    fields << "    pub const FIELD_" << name << ":u32=" << f.number << ";\n";
                } else
                    fields << "enum {" << replace(g.model.ns, ".", "_") << "_" << e.name << "Field_"
                           << name << "=" << f.number << "};\n";
            }
            if (lang == "node") {
                fields << "});\n";
                std::string declaration = "  static readonly fields: {";
                for (const auto &f : e.fields)
                    declaration += "readonly " + binding.fields.at(f.name).member + ":" +
                                   std::to_string(f.number) + ";";
                declaration += "};";
                auto &types = files.at(dir + "fq_codecs.d.ts");
                auto point = types.find("export class " + codec + " {");
                require(point != std::string::npos, "Missing codec declaration");
                types.insert(point + ("export class " + codec + " {").size(), declaration);
            }
            auto &code = files.at(path);
            if (marker.empty())
                code += fields.str();
            else {
                auto point = code.find(marker);
                require(point != std::string::npos,
                        "Missing generated codec metadata insertion point");
                code.insert(point + (lang == "go" ? 0 : marker.size()), fields.str());
            }
        }
    }
    std::ostringstream cpp, c, rust;
    cpp << banner << "#pragma once\n";
    c << banner << "#pragma once\n";
    rust << banner;
    bool have_cpp = false, have_c = false, have_rust = false;
    for (const auto &g : source.groups) {
        auto lang = g.entities.begin()->second.language;
        auto ns = replace(g.model.ns, ".", "/");
        if (lang == "cpp") {
            have_cpp = true;
            cpp << "#include " << quote(ns + "/fq_codecs.hpp") << "\n";
        }
        if (lang == "c") {
            have_c = true;
            c << "#include " << quote(ns + "/fq_codecs.h") << "\n";
        }
        if (lang == "rust") {
            have_rust = true;
            rust << "pub mod " << replace(g.model.ns, ".", "_") << " {include!("
                 << quote(ns + "/fq_codecs.rs") << ");}\n";
        }
        if (lang == "python") {
            std::string dir = "python/fqdb_generated";
            files[dir + "/__init__.py"] =
                "# Generated by FengquDB native codegen v1. DO NOT EDIT.\n";
            std::istringstream parts(ns);
            std::string part;
            while (std::getline(parts, part, '/')) {
                dir += "/" + part;
                files[dir + "/__init__.py"] =
                    "# Generated by FengquDB native codegen v1. DO NOT EDIT.\n";
            }
        }
    }
    if (have_cpp)
        files["cpp/fq_codecs.hpp"] = cpp.str();
    if (have_c)
        files["c/fq_codecs.h"] = c.str();
    if (have_rust)
        files["rust/fq_codecs.rs"] = rust.str();
}
} // namespace codegen
