#include "source.hpp"
#include <fstream>
#include <sstream>
namespace fs = std::filesystem;
namespace codegen {
namespace {
struct Token {
    std::string text;
    size_t line, column;
    bool string = false, comment = false;
};
struct Lexer {
    std::string file, data;
    std::vector<Token> tokens;
    [[noreturn]] void error(size_t line, const std::string &message) const {
        throw std::runtime_error(file + ":" + std::to_string(line) + ": " + message);
    }
    explicit Lexer(const fs::path &path) : file(path.u8string()) {
        require(fs::file_size(path) <= 4194304, "Source exceeds 4 MiB: " + file);
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        require(static_cast<bool>(in) && !in.bad(), "Cannot read source: " + file);
        data = out.str();
        size_t line = 1, column = 0;
        for (size_t p = 0; p < data.size();) {
            char c = data[p];
            if (c == '\n') {
                ++p;
                ++line;
                column = 0;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++p;
                column += c == '\t' ? 4 : 1;
                continue;
            }
            size_t begin = p, col = column, start_line = line;
            if (c == '#' || (c == '/' && p + 1 < data.size() && data[p + 1] == '/')) {
                while (p < data.size() && data[p] != '\n')
                    ++p;
                tokens.push_back({data.substr(begin, p - begin), line, col, false, true});
                column += p - begin;
                continue;
            }
            if (c == '/' && p + 1 < data.size() && data[p + 1] == '*') {
                p += 2;
                while (p + 1 < data.size() && !(data[p] == '*' && data[p + 1] == '/')) {
                    if (data[p++] == '\n') {
                        ++line;
                        column = 0;
                    }
                }
                if (p + 1 >= data.size())
                    error(start_line, "Unterminated comment");
                p += 2;
                tokens.push_back({data.substr(begin, p - begin), start_line, col, false, true});
                column += p - begin;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                bool triple = p + 2 < data.size() && data[p + 1] == c && data[p + 2] == c;
                p += triple ? 3 : 1;
                bool closed = false;
                while (p < data.size()) {
                    if (data[p] == '\\' && c != '`') {
                        p += 2;
                        continue;
                    }
                    if (data[p] == c && (!triple || (p + 2 < data.size() && data[p + 1] == c &&
                                                     data[p + 2] == c))) {
                        p += triple ? 3 : 1;
                        closed = true;
                        break;
                    }
                    if (data[p++] == '\n') {
                        ++line;
                        column = 0;
                    }
                }
                if (!closed)
                    error(start_line, "Unterminated string literal");
                tokens.push_back({data.substr(begin, p - begin), start_line, col, true, false});
                column += p - begin;
                continue;
            }
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                ++p;
                while (p < data.size() &&
                       (std::isalnum(static_cast<unsigned char>(data[p])) || data[p] == '_'))
                    ++p;
            } else if (p + 1 < data.size() && data.substr(p, 2) == "::")
                p += 2;
            else
                ++p;
            tokens.push_back({data.substr(begin, p - begin), line, col, false, false});
            column += p - begin;
        }
    }
};
struct RawField {
    std::string name, native_type, type, getter, setter;
    uint32_t number = 0;
    size_t line;
};
struct RawEntity {
    SourceEntity binding;
    std::string ns, prefix, key_name;
    std::vector<RawField> fields;
    size_t line;
    bool public_default = true;
};
std::string join(const std::vector<Token> &tokens, size_t a, size_t b) {
    std::string out;
    for (size_t i = a; i < b; ++i)
        if (!tokens[i].comment)
            out += tokens[i].text;
    return out;
}
size_t matching(const Lexer &l, size_t open, const std::string &left, const std::string &right) {
    unsigned depth = 0;
    for (size_t i = open; i < l.tokens.size(); ++i)
        if (!l.tokens[i].string && !l.tokens[i].comment) {
            if (l.tokens[i].text == left)
                ++depth;
            if (l.tokens[i].text == right && --depth == 0)
                return i;
        }
    l.error(l.tokens[open].line, "Unbalanced " + left);
}
std::map<std::string, std::string> metadata(const std::vector<Token> &tokens, size_t a, size_t b) {
    std::map<std::string, std::string> result;
    for (size_t i = a; i < b; ++i)
        if (tokens[i].comment) {
            auto p = tokens[i].text.find("fqdb:");
            if (p == std::string::npos)
                continue;
            std::istringstream in(tokens[i].text.substr(p + 5));
            std::string word;
            in >> word;
            result[word] = "true";
            while (in >> word) {
                auto eq = word.find('=');
                if (eq != std::string::npos)
                    result[word.substr(0, eq)] = word.substr(eq + 1);
            }
        }
    return result;
}
std::string language_for(const fs::path &path) {
    auto e = lower(path.extension().string());
    if (e == ".java")
        return "java";
    if (e == ".py")
        return "python";
    if (e == ".go")
        return "go";
    if (e == ".cs")
        return "csharp";
    if (e == ".rs")
        return "rust";
    if (e == ".ts" || e == ".js" || e == ".mjs" || e == ".cjs")
        return "node";
    if (e == ".hpp" || e == ".hh" || e == ".hxx" || e == ".cpp")
        return "cpp";
    if (e == ".h" || e == ".c")
        return "c";
    return {};
}
std::string type_for(const std::string &lang, std::string native) {
    static const std::map<std::string, std::map<std::string, std::string>> types = {
        {"java",
         {{"int", "sint32"},
          {"long", "sint64"},
          {"boolean", "bool"},
          {"float", "float"},
          {"double", "double"},
          {"String", "string"},
          {"java.lang.String", "string"},
          {"byte[]", "bytes"}}},
        {"csharp",
         {{"int", "sint32"},
          {"long", "sint64"},
          {"uint", "uint32"},
          {"ulong", "uint64"},
          {"bool", "bool"},
          {"float", "float"},
          {"double", "double"},
          {"string", "string"},
          {"byte[]", "bytes"}}},
        {"python",
         {{"int", "sint64"},
          {"str", "string"},
          {"bool", "bool"},
          {"float", "double"},
          {"bytes", "bytes"}}},
        {"node",
         {{"number", "double"},
          {"bigint", "sint64"},
          {"string", "string"},
          {"boolean", "bool"},
          {"Uint8Array", "bytes"}}},
        {"go",
         {{"int32", "sint32"},
          {"int64", "sint64"},
          {"uint32", "uint32"},
          {"uint64", "uint64"},
          {"bool", "bool"},
          {"float32", "float"},
          {"float64", "double"},
          {"string", "string"},
          {"[]byte", "bytes"}}},
        {"rust",
         {{"i32", "sint32"},
          {"i64", "sint64"},
          {"u32", "uint32"},
          {"u64", "uint64"},
          {"bool", "bool"},
          {"f32", "float"},
          {"f64", "double"},
          {"String", "string"},
          {"Vec<u8>", "bytes"}}},
        {"cpp",
         {{"int", "sint32"},
          {"int32_t", "sint32"},
          {"int64_t", "sint64"},
          {"uint32_t", "uint32"},
          {"uint64_t", "uint64"},
          {"std::int32_t", "sint32"},
          {"std::int64_t", "sint64"},
          {"std::uint32_t", "uint32"},
          {"std::uint64_t", "uint64"},
          {"bool", "bool"},
          {"float", "float"},
          {"double", "double"},
          {"std::string", "string"},
          {"std::vector<uint8_t>", "bytes"},
          {"std::vector<std::uint8_t>", "bytes"}}},
        {"c",
         {{"int", "sint32"},
          {"int32_t", "sint32"},
          {"int64_t", "sint64"},
          {"uint32_t", "uint32"},
          {"uint64_t", "uint64"},
          {"bool", "bool"},
          {"float", "float"},
          {"double", "double"},
          {"fqdb_text", "string"},
          {"fqdb_bytes", "bytes"}}}};
    auto found = types.at(lang).find(native);
    return found == types.at(lang).end() ? "" : found->second;
}
std::string literal_type(const std::vector<Token> &t, size_t a, size_t b, const std::string &lang) {
    while (a < b && t[a].comment)
        ++a;
    if (a >= b)
        return {};
    auto value = join(t, a, b);
    if (lang == "node" && a + 3 < b && t[a].text == "new" && t[a + 1].text == "Uint8Array" &&
        t[a + 2].text == "(") {
        unsigned depth = 0;
        for (size_t i = a + 2; i < b; ++i)
            if (!t[i].string && !t[i].comment) {
                if (t[i].text == "(")
                    ++depth;
                if (t[i].text == ")" && --depth == 0)
                    return i + 1 == b ? "Uint8Array" : "";
            }
        return {};
    }
    if (value == "True" || value == "False" || value == "true" || value == "false")
        return lang == "python" ? "bool" : "boolean";
    if (t[a].string && a + 1 == b)
        return lang == "python" ? "str" : "string";
    if (lang == "python" && t[a].text == "b" && a + 2 == b && t[a + 1].string)
        return "bytes";
    size_t p = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    bool digit = false, dot = false, exponent = false;
    for (; p < value.size(); ++p) {
        char c = value[p];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
            continue;
        }
        if (c == '_')
            continue;
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        if ((c == 'e' || c == 'E') && !exponent) {
            exponent = true;
            continue;
        }
        if ((c == '+' || c == '-') && p && (value[p - 1] == 'e' || value[p - 1] == 'E'))
            continue;
        if (lang == "node" && c == 'n' && digit && p + 1 == value.size() && !dot && !exponent)
            return "bigint";
        return {};
    }
    return digit ? (lang == "python" ? (dot || exponent ? "float" : "int") : "number") : "";
}
void add_field(const Lexer &l, RawEntity &entity, RawField f,
               const std::map<std::string, std::string> &meta) {
    for (const auto &m : meta)
        if (m.first != "field" && m.first != "ignore" && m.first != "type" && m.first != "number")
            l.error(f.line, "Unknown fqdb field option: " + m.first);
    if (meta.count("ignore"))
        return;
    if (meta.count("type")) {
        f.type = meta.at("type");
        const auto inferred = type_for(entity.binding.language, f.native_type);
        bool compatible = f.type == inferred;
        if (entity.binding.language == "python" && f.native_type == "int")
            compatible = f.type == "sint32" || f.type == "sint64" || f.type == "uint32" ||
                         f.type == "uint64";
        if (entity.binding.language == "node" && f.native_type == "number")
            compatible = f.type == "sint32" || f.type == "sint64" || f.type == "uint32" ||
                         f.type == "uint64" || f.type == "float" || f.type == "double";
        if (entity.binding.language == "node" && f.native_type == "bigint")
            compatible = f.type == "sint64" || f.type == "uint64";
        if (entity.binding.language == "python" && f.native_type == "float")
            compatible = f.type == "float" || f.type == "double";
        if (!compatible)
            l.error(f.line, "Explicit wire type is incompatible with source type: " + f.name);
    } else
        f.type = type_for(entity.binding.language, f.native_type);
    if (f.type.empty())
        l.error(f.line, "Cannot infer persisted field '" + f.name + "' (" + f.native_type +
                            "). Use a supported scalar type/type hint, or fqdb:ignore.");
    if (meta.count("number")) {
        const auto &v = meta.at("number");
        require(!v.empty() && std::all_of(v.begin(), v.end(),
                                          [](unsigned char c) { return std::isdigit(c) != 0; }),
                "Invalid explicit field number");
        auto n = std::stoull(v);
        require(n > 0 && n <= 0x1fffffff, "Invalid explicit field number");
        f.number = static_cast<uint32_t>(n);
    }
    for (const auto &other : entity.fields)
        if (other.name == f.name)
            l.error(f.line, "Duplicate persisted field " + f.name);
    entity.fields.push_back(std::move(f));
}
std::string source_namespace(const Lexer &l, const std::string &lang) {
    const auto &t = l.tokens;
    if (lang == "java" || lang == "go" || lang == "csharp")
        for (size_t i = 0; i + 1 < t.size(); ++i)
            if (t[i].text == (lang == "csharp" ? "namespace" : "package")) {
                size_t b = i + 1;
                while (b < t.size() && t[b].text != ";" && t[b].text != "{" &&
                       t[b].line == t[i].line)
                    ++b;
                auto ns = join(t, i + 1, b);
                if (!ns.empty())
                    return ns;
            }
    return lang == "csharp" || lang == "java" ? "" : "models";
}
void fields_braced(const Lexer &l, RawEntity &e, size_t begin, size_t end) {
    const auto &t = l.tokens;
    const auto &lang = e.binding.language;
    size_t p = begin;
    std::string access = lang == "cpp" ? (e.public_default ? "public" : "private") : "";
    std::map<std::string, size_t> java_methods;
    const bool cs_attributes =
        lang == "csharp" && std::any_of(t.begin() + static_cast<std::ptrdiff_t>(begin),
                                        t.begin() + static_cast<std::ptrdiff_t>(end),
                                        [](const Token &v) { return v.text == "FqField"; });
    if (lang == "java")
        for (size_t i = begin; i + 2 < end; ++i)
            if (t[i].text == "public") {
                for (size_t j = i + 1; j + 1 < end && j < i + 10; ++j) {
                    if (t[j].text == "static" || t[j].text == ";" || t[j].text == "{")
                        break;
                    if (t[j + 1].text == "(") {
                        auto close = matching(l, j + 1, "(", ")");
                        size_t count = close == j + 2 ? 0 : 1;
                        for (size_t k = j + 2; k < close; ++k)
                            if (t[k].text == ",")
                                ++count;
                        java_methods[t[j].text] = count;
                        break;
                    }
                }
            }
    while (p < end) {
        size_t a = p;
        while (p < end && t[p].comment)
            ++p;
        if (p == end)
            break;
        if (lang == "java")
            while (p < end && t[p].text == "@") {
                ++p;
                if (p < end && identifier(t[p].text))
                    ++p;
                while (p + 1 < end && t[p].text == "." && identifier(t[p + 1].text))
                    p += 2;
                if (p < end && t[p].text == "(")
                    p = matching(l, p, "(", ")") + 1;
                while (p < end && t[p].comment)
                    ++p;
            }
        if (lang == "csharp")
            while (p < end && t[p].text == "[") {
                p = matching(l, p, "[", "]") + 1;
                while (p < end && t[p].comment)
                    ++p;
            }
        if (lang == "cpp" && p + 1 < end && t[p + 1].text == ":" &&
            (t[p].text == "public" || t[p].text == "private" || t[p].text == "protected")) {
            access = t[p].text;
            p += 2;
            continue;
        }
        size_t b = p;
        int paren = 0, angle = 0, square = 0;
        bool equal = false, method = false;
        for (; b < end; ++b) {
            if (lang == "go" && b > p && t[b].line > t[b - 1].line)
                break;
            if (t[b].comment || t[b].string)
                continue;
            auto s = t[b].text;
            if (s == "=" && !paren)
                equal = true;
            if (s == "(") {
                if (!equal && !paren)
                    method = true;
                ++paren;
            }
            if (s == ")")
                --paren;
            if (s == "<")
                ++angle;
            if (s == ">" && angle)
                --angle;
            if (s == "[")
                ++square;
            if (s == "]")
                --square;
            if (!paren && !square && s == "{") {
                auto close = matching(l, b, "{", "}");
                if (method) {
                    b = close + 1;
                    break;
                }
                if (lang == "csharp" && !equal && (close + 1 == end || t[close + 1].text != "=")) {
                    b = close + 1;
                    break;
                }
                b = close;
                continue;
            }
            if (!paren && !square && !angle &&
                (s == ";" || (lang == "rust" && s == ",") ||
                 (lang == "go" && b > p && t[b].line > t[b - 1].line)))
                break;
        }
        size_t next = b < end && (t[b].text == ";" || t[b].text == ",") ? b + 1 : b;
        if (next <= p)
            l.error(t[p].line, "Cannot parse field declaration");
        auto meta = metadata(t, a, b);
        bool skip = method || meta.count("ignore");
        if (lang == "csharp")
            for (size_t i = a; i + 2 < p; ++i)
                if (t[i].text == "FqField")
                    meta["number"] = t[i + 2].text;
        if (cs_attributes && !meta.count("number"))
            skip = true;
        for (size_t i = p; i < b; ++i)
            if (t[i].text == "static" || t[i].text == "transient" || t[i].text == "function")
                skip = true;
        if (skip) {
            p = next;
            continue;
        }
        for (size_t i = p; i < b; ++i)
            if (t[i].text == "final" || t[i].text == "readonly")
                l.error(t[i].line,
                        "Persisted source fields must be mutable, or explicitly ignored");
        std::vector<Token> part;
        for (size_t i = p; i < b; ++i)
            if (!t[i].comment)
                part.push_back(t[i]);
        if (part.empty()) {
            p = next;
            continue;
        }
        if (lang == "csharp") {
            while (!part.empty() && part[0].text == "[") {
                size_t end_attribute = 0;
                while (end_attribute < part.size() && part[end_attribute].text != "]")
                    ++end_attribute;
                if (end_attribute == part.size())
                    l.error(t[p].line, "Invalid field attribute");
                for (size_t i = 1; i < end_attribute; ++i) {
                    if (part[i].text == "FqField" && i + 2 < end_attribute)
                        meta["number"] = part[i + 2].text;
                }
                part.erase(part.begin(),
                           part.begin() + static_cast<std::ptrdiff_t>(end_attribute + 1));
            }
            size_t property = 0;
            while (property < part.size() && part[property].text != "{")
                ++property;
            if (property < part.size()) {
                bool get = false, set = false;
                for (size_t i = property; i < part.size(); ++i) {
                    if (part[i].text == "get")
                        get = true;
                    if (part[i].text == "set")
                        set = true;
                }
                if (!get || !set)
                    l.error(t[p].line, "Native source mode requires mutable C# auto-properties");
                part.resize(property);
            }
        }
        size_t eq = part.size();
        for (size_t i = 0; i < part.size(); ++i)
            if (part[i].text == "=") {
                eq = i;
                break;
            }
        RawField f;
        f.line = t[p].line;
        static const std::set<std::string> modifiers = {"public",   "private", "protected", "final",
                                                        "readonly", "export",  "declare",   "pub",
                                                        "volatile", "mutable"};
        size_t first = 0;
        while (first < eq && modifiers.count(part[first].text))
            ++first;
        if (lang == "rust" || lang == "node") {
            size_t colon = first;
            while (colon < eq && part[colon].text != ":")
                ++colon;
            f.name = part[first].text;
            if (colon < eq) {
                if (colon != first + 1)
                    l.error(f.line, "Optional/destructured fields are not supported");
                f.native_type = join(part, colon + 1, eq);
            } else if (lang == "node" && eq < part.size())
                f.native_type = literal_type(part, eq + 1, part.size(), lang);
            if (lang == "rust" && (first == 0 || part[0].text != "pub"))
                l.error(f.line, "Persisted Rust fields must be pub");
        } else if (lang == "go") {
            f.name = part[first].text;
            f.native_type = join(part, first + 1, eq);
            if (!std::isupper(static_cast<unsigned char>(f.name[0])))
                l.error(f.line, "Persisted Go fields must be exported or marked fqdb:ignore");
        } else {
            if (eq < first + 2)
                l.error(f.line, "Unsupported field declaration");
            f.name = part[eq - 1].text;
            f.native_type = join(part, first, eq - 1);
            if (lang == "java" && (part[0].text == "private" || part[0].text == "protected")) {
                auto get = "get" + upper(f.name), set = "set" + upper(f.name);
                if (f.native_type == "boolean" && !java_methods.count(get))
                    get = "is" + upper(f.name);
                if (!java_methods.count(get) || java_methods.at(get) != 0 ||
                    !java_methods.count(set) || java_methods.at(set) != 1)
                    l.error(f.line, "Private Java fields need explicit public get/is and set "
                                    "methods, or fqdb:ignore");
                f.getter = get;
                f.setter = set;
            }
            if (((lang == "csharp") &&
                 (part[0].text == "private" || part[0].text == "protected")) ||
                (lang == "cpp" && access != "public"))
                l.error(f.line, "Use accessible fields for source codecs; private/getter-only "
                                "models require an explicit adapter");
        }
        if (f.name.empty() || f.name == "}" || f.native_type.empty())
            l.error(f.line, "Missing or ambiguous field type; add an explicit scalar type");
        add_field(l, e, std::move(f), meta);
        p = next;
    }
}
void fields_python(const Lexer &l, RawEntity &e, size_t begin, size_t end, size_t indent) {
    const auto &t = l.tokens;
    size_t p = begin;
    std::map<std::string, std::string> pending;
    while (p < end) {
        size_t a = p, line = t[p].line;
        while (p < end && t[p].line == line)
            ++p;
        if (t[a].column != indent)
            continue;
        if (t[a].comment) {
            auto m = metadata(t, a, p);
            pending.insert(m.begin(), m.end());
            continue;
        }
        auto meta = metadata(t, a, p);
        meta.insert(pending.begin(), pending.end());
        pending.clear();
        if (meta.count("ignore"))
            continue;
        if (t[a].string)
            continue;
        if (t[a].text == "def" || t[a].text == "async" || t[a].text == "@" || t[a].text == "pass")
            continue;
        size_t colon = a, eq = a;
        while (colon < p && t[colon].text != ":")
            ++colon;
        while (eq < p && t[eq].text != "=")
            ++eq;
        if (colon == p && eq == p)
            l.error(line, "Unsupported Python class statement");
        RawField f;
        f.name = t[a].text;
        f.line = line;
        size_t stop = p;
        while (stop > a && t[stop - 1].comment)
            --stop;
        if (colon < p) {
            if (colon != a + 1)
                l.error(line, "Unsupported Python field declaration");
            f.native_type = join(t, colon + 1, std::min(eq, stop));
        } else {
            if (eq != a + 1)
                l.error(line, "Unsupported Python assignment");
            f.native_type = literal_type(t, eq + 1, stop, "python");
        }
        add_field(l, e, std::move(f), meta);
    }
}
std::vector<RawEntity> extract(const fs::path &path, const fs::path &root, const fs::path &out,
                               const std::string &forced) {
    Lexer l(path);
    const auto &t = l.tokens;
    auto lang = forced.empty() ? language_for(path) : forced;
    std::vector<RawEntity> entities;
    for (size_t marker = 0; marker < t.size(); ++marker) {
        if (!t[marker].comment || t[marker].text.find("fqdb:entity") == std::string::npos)
            continue;
        auto meta = metadata(t, marker, marker + 1);
        for (const auto &m : meta)
            if (m.first != "entity" && m.first != "prefix" && m.first != "key" &&
                m.first != "module")
                l.error(t[marker].line, "Unknown fqdb entity option: " + m.first);
        size_t p = marker + 1;
        while (p < t.size() && t[p].text != "class" && t[p].text != "struct" &&
               t[p].text != "interface") {
            if (t[p].text == "record" || t[p].text == "enum" || t[p].text == "def" ||
                t[p].text == "function")
                l.error(t[p].line, "fqdb:entity must mark a supported concrete class/struct");
            ++p;
        }
        if (p + 1 >= t.size())
            l.error(t[marker].line, "fqdb:entity must precede a class or struct");
        if (t[p].text == "interface")
            l.error(t[p].line, "Use a concrete class with a public no-argument constructor");
        RawEntity e;
        e.line = t[p].line;
        e.ns = source_namespace(l, lang);
        e.binding.language = lang;
        e.binding.name = t[p + 1].text;
        e.public_default = t[p].text != "class";
        if (lang == "go") {
            if (p < 1 || t[p - 1].text == "type")
                l.error(t[p].line, "Expected type Name struct");
            e.binding.name = t[p - 1].text;
        }
        if (lang == "cpp") {
            std::vector<std::pair<size_t, std::string>> scopes;
            for (size_t i = 0; i < p; ++i)
                if (t[i].text == "namespace") {
                    size_t b = i + 1;
                    while (b < p && t[b].text != "{")
                        ++b;
                    if (b < p && matching(l, b, "{", "}") > p)
                        scopes.emplace_back(i, join(t, i + 1, b));
                }
            if (!scopes.empty()) {
                e.ns.clear();
                for (const auto &s : scopes) {
                    if (!e.ns.empty())
                        e.ns += ".";
                    e.ns += replace(s.second, "::", ".");
                }
                e.binding.qualified = "::" + replace(e.ns, ".", "::") + "::" + e.binding.name;
            }
        }
        e.prefix = meta.count("prefix") ? meta.at("prefix") : lower(e.binding.name) + "/";
        e.key_name = meta.count("key") ? meta.at("key") : "";
        size_t body = p + (lang == "go" ? 1 : 2), last;
        if (lang == "python") {
            if (body < t.size() && t[body].text == "(")
                l.error(e.line, "Python inherited entities need an explicit adapter");
            for (size_t i = marker + 1; i + 1 < p; ++i)
                if (t[i].text == "@" && t[i + 1].text == "dataclass")
                    e.binding.keyword_constructor = true;
            while (body < t.size() && t[body].text != ":")
                ++body;
            if (body == t.size())
                l.error(e.line, "Missing Python class body");
            ++body;
            last = body;
            while (last < t.size() &&
                   ((t[last].comment && t[last].text.find("fqdb:entity") == std::string::npos) ||
                    t[last].column > t[p].column))
                ++last;
            size_t indent = body < last ? t[body].column : t[p].column + 4;
            while (body < last && t[body].comment)
                ++body;
            fields_python(l, e, body, last, indent);
            auto python_root = root;
            auto relative = fs::relative(path, root);
            if (relative.generic_u8string().rfind("src/", 0) == 0)
                python_root = root / "src";
            relative = fs::relative(path, python_root);
            relative.replace_extension();
            if (relative.filename() == "__init__")
                relative = relative.parent_path();
            if (relative.empty())
                relative = root.filename();
            e.binding.module = replace(relative.generic_u8string(), "/", ".");
            e.ns = e.binding.module;
        } else {
            while (body < t.size() && t[body].text != "{") {
                if (t[body].text == "extends" || t[body].text == ":" || t[body].text == "<")
                    l.error(e.line, "Inheritance and generic entities require an explicit adapter");
                ++body;
            }
            if (body == t.size())
                l.error(e.line, "Missing entity body");
            last = matching(l, body, "{", "}");
            fields_braced(l, e, body + 1, last);
        }
        if (lang == "cpp") {
            if (e.binding.qualified.empty())
                e.binding.qualified = "::" + e.binding.name;
        } else if (lang == "c")
            e.binding.qualified = "struct " + e.binding.name;
        else if (lang == "csharp" || lang == "java") {
            e.binding.module = e.ns;
            e.binding.qualified =
                lang == "csharp" ? "global::" + (e.ns.empty() ? "" : e.ns + ".") + e.binding.name
                                 : e.binding.name;
            if (e.ns.empty())
                e.ns = "global_entities";
        } else if (lang == "rust") {
            auto rust_root = path.parent_path();
            while (rust_root.filename() != "src" && rust_root != rust_root.parent_path())
                rust_root = rust_root.parent_path();
            if (rust_root.filename() != "src")
                rust_root = root;
            auto rel = fs::relative(path, rust_root).generic_u8string();
            auto module = fs::u8path(rel);
            module.replace_extension();
            auto s = replace(module.generic_u8string(), "/", "::");
            if (s == "main" || s == "lib" || s == "mod")
                s = "";
            else if (s.size() > 5 && s.substr(s.size() - 5) == "::mod")
                s.resize(s.size() - 5);
            e.binding.qualified = "crate::" + (s.empty() ? "" : s + "::") + e.binding.name;
            e.ns = s.empty() ? "models" : replace(s, "::", ".");
        } else
            e.binding.qualified = e.binding.name;
        if (meta.count("module"))
            e.binding.module = meta.at("module");
        if (lang == "python") {
            std::istringstream parts(e.binding.module);
            std::string part;
            while (std::getline(parts, part, '.'))
                if (!identifier(part))
                    l.error(e.line, "Invalid Python import module; use module=package.name");
        }
        if (lang == "node") {
            auto rel = fs::relative(path, out / "node");
            rel.replace_extension(".js");
            e.binding.module = rel.generic_u8string();
            if (e.binding.module[0] != '.')
                e.binding.module = "./" + e.binding.module;
        }
        e.binding.include_path = path.generic_u8string();
        if (lang == "go") {
            auto project = path.parent_path();
            while (!fs::exists(project / "go.mod") && project != project.parent_path())
                project = project.parent_path();
            require(fs::exists(project / "go.mod"),
                    "Go source generation needs go.mod to resolve its package import path");
            std::ifstream mod(project / "go.mod");
            std::string line, module;
            while (std::getline(mod, line)) {
                std::istringstream words(line);
                std::string key;
                words >> key;
                if (key == "module") {
                    words >> module;
                    break;
                }
            }
            require(!module.empty() && e.ns != "main",
                    "Go entities need an importable named package (not package main)");
            auto relative = fs::relative(path.parent_path(), project).generic_u8string();
            e.binding.module = module + (relative == "." ? "" : "/" + relative);
        }
        if (e.fields.empty())
            l.error(e.line, "No supported persisted fields");
        if (e.key_name.empty()) {
            for (const auto &f : e.fields)
                if (lower(f.name) == "id")
                    e.key_name = f.name;
        }
        if (e.key_name.empty())
            l.error(e.line, "No id field; set key=fieldName in fqdb:entity");
        for (auto &f : e.fields)
            if (f.name == e.key_name && lang == "node" && f.native_type == "number" &&
                f.type == "double")
                f.type = "sint64";
        entities.push_back(std::move(e));
        marker = lang == "python" ? last - 1 : last;
    }
    return entities;
}
Json read_json(const fs::path &path) {
    require(fs::file_size(path) <= 4194304, "Schema lock exceeds 4 MiB");
    std::ifstream in(path);
    Json j;
    in >> j;
    require(static_cast<bool>(in), "Invalid schema lock");
    return j;
}
} // namespace
SourceInput read_sources(const fs::path &input, const fs::path &output, const fs::path &registry,
                         const std::string &language) {
    auto root = fs::is_directory(input) ? fs::absolute(input) : fs::absolute(input).parent_path();
    std::vector<fs::path> paths;
    if (fs::is_regular_file(input))
        paths.push_back(fs::absolute(input));
    else {
        static const std::set<std::string> ignored = {
            ".git",      ".agents",      ".fqdb",       "build", "bin",
            "obj",       "node_modules", "target",      "dist",  "vendor",
            "Generated", "generated",    "__pycache__", ".venv", "venv"};
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            if (it->is_symlink()) {
                if (it->is_directory())
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory() &&
                (ignored.count(it->path().filename().u8string()) ||
                 fs::is_regular_file(it->path() / ".fqdb-sdk") ||
                 it->path().lexically_normal() == output.lexically_normal())) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file() && !language_for(it->path()).empty())
                paths.push_back(it->path());
            require(paths.size() <= 10000,
                    "Source scan exceeded 10000 files; select a smaller model directory");
        }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<RawEntity> raw;
    for (const auto &path : paths) {
        if (!language.empty() && language_for(path) != language &&
            !(language == "cpp" && path.extension() == ".h"))
            continue;
        require(fs::file_size(path) <= 4194304,
                "Source file exceeds 4 MiB; narrow the model source root: " + path.u8string());
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        auto text = ss.str();
        if (text.find("fqdb:entity") == std::string::npos ||
            text.rfind("// Generated by FengquDB", 0) == 0 ||
            text.rfind("# Generated by FengquDB", 0) == 0)
            continue;
        auto found = extract(path, root, output, language);
        raw.insert(raw.end(), found.begin(), found.end());
    }
    require(fs::exists(registry) || !fs::exists(output / ".fqdb-models.manifest"),
            "Schema lock is missing but generated output already exists. Restore the versioned "
            "FengquDB.schema.lock.json before regenerating.");
    Json old = fs::exists(registry)
                   ? read_json(registry)
                   : Json{{"format", "fqdb-source-lock-1"}, {"entities", Json::object()}};
    require(old.value("format", "") == "fqdb-source-lock-1" && old.at("entities").is_object(),
            "Not a FengquDB source schema lock");
    require(!raw.empty() || !old.at("entities").empty(),
            "No marked entities found. Add // fqdb:entity (Python: # fqdb:entity) above a "
            "class/struct.");
    SourceInput result;
    result.output_root = output.generic_u8string();
    result.registry = old;
    std::map<std::string, Json> schemas;
    std::map<std::string, SourceGroup> groups;
    std::set<std::string> identities;
    for (const auto &r : raw) {
        auto group_id = r.binding.language + ":" + r.ns;
        if (r.binding.language == "go" && groups.count(group_id) &&
            !groups[group_id].entities.empty())
            require(groups[group_id].entities.begin()->second.module == r.binding.module,
                    "Generate Go packages with the same package name but different import paths "
                    "separately");
        if (!schemas.count(group_id))
            schemas[group_id] = {{"version", 1}, {"namespace", r.ns}, {"entities", Json::array()}};
        auto identity = r.binding.language + ":" + r.ns + ":" + r.binding.name;
        require(identities.insert(identity).second, "Duplicate source entity: " + identity);
        Json history =
            old["entities"].contains(identity)
                ? old["entities"][identity]
                : Json{{"prefix", r.prefix}, {"key", r.key_name}, {"fields", Json::object()}};
        require(history.at("prefix") == r.prefix && history.at("key") == r.key_name,
                "Stored entity key/prefix changed: " + identity +
                    ". Use a new entity or an explicit data migration.");
        Json entity = {{"name", r.binding.name},
                       {"prefix", r.prefix},
                       {"fields", Json::array()},
                       {"reserved", Json::array()}};
        std::set<uint32_t> assigned, active;
        uint32_t next = 1;
        for (const auto &field : history["fields"].items()) {
            auto n = field.value().at("number").get<uint32_t>();
            require(n > 0 && n <= 0x1fffffff, "Invalid number in schema lock");
            assigned.insert(n);
            next = std::max(next, n + 1);
        }
        auto binding = r.binding;
        for (const auto &f : r.fields) {
            uint32_t number = f.number;
            if (history["fields"].contains(f.name)) {
                const auto &previous = history["fields"][f.name];
                require(previous.at("type") == f.type, "Persisted field type changed: " + f.name +
                                                           "; use a new field or migrate data");
                auto n = previous.at("number").get<uint32_t>();
                require(!number || number == n, "Persisted field number changed: " + f.name);
                number = n;
            } else {
                if (number) {
                    for (const auto &h : history["fields"].items())
                        if (h.value().at("number") == number)
                            require(h.value().at("type") == f.type,
                                    "Renamed field has incompatible type");
                } else {
                    while (assigned.count(next) || (next >= 19000 && next <= 19999))
                        ++next;
                    number = next++;
                }
                history["fields"][f.name] = {{"number", number}, {"type", f.type}};
                assigned.insert(number);
            }
            require(active.insert(number).second, "Duplicate field number in source entity");
            auto canonical = lower(f.name.substr(0, 1)) + f.name.substr(1);
            entity["fields"].push_back({{"name", canonical},
                                        {"type", f.type},
                                        {"number", number},
                                        {"key", f.name == r.key_name}});
            binding.fields[canonical] = {f.name, f.native_type, f.getter, f.setter};
        }
        for (auto n : assigned)
            if (!active.count(n))
                entity["reserved"].push_back(n);
        result.registry["entities"][identity] = std::move(history);
        schemas[group_id]["entities"].push_back(entity);
        groups[group_id].entities[r.binding.name] = std::move(binding);
    }
    result.schema = Json::array();
    for (auto &entry : groups) {
        entry.second.model = parse(schemas.at(entry.first));
        result.schema.push_back(schemas.at(entry.first));
        result.groups.push_back(std::move(entry.second));
    }
    return result;
}
} // namespace codegen
