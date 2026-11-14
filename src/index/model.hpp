#pragma once
// Private bounded wire model. All persistent numbers use Protobuf little endian.
#include "fengqudb_index.h"
#include "fengqudb_wire.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fengqudb::index::impl {
namespace w = fengqudb::wire;
struct Error {
    fqdb_status status;
};
inline void require(bool value, fqdb_status status = FQDB_INVALID_ARGUMENT) {
    if (!value)
        throw Error{status};
}
inline fqdb_slice slice(std::string_view s) {
    return {s.data(), static_cast<uint64_t>(s.size())};
}
inline std::string_view view(fqdb_slice s) {
    require(s.size <= FQDB_MAX_VALUE_BYTES && (s.data || !s.size));
    return {s.size ? static_cast<const char *>(s.data) : "", static_cast<size_t>(s.size)};
}
inline uint64_t number(w::Reader &r, w::Field f) {
    uint64_t n = 0;
    require(f.type == w::Type::Varint && r.varint(n));
    return n;
}
inline std::string_view bytes(w::Reader &r, w::Field f) {
    std::string_view s;
    require(f.type == w::Type::Bytes && r.bytes(s));
    return s;
}
inline double real(w::Reader &r, w::Field f) {
    uint64_t bits = 0;
    double result = 0;
    require(f.type == w::Type::Fixed64 && r.fixed(bits, 8));
    std::memcpy(&result, &bits, 8);
    require(std::isfinite(result));
    return result;
}
template <class F> void fields(std::string_view s, F fn) {
    w::Reader r(s, FQDB_MAX_VALUE_BYTES);
    w::Field f;
    while (r.next(f))
        if (!fn(r, f))
            require(r.skip(f));
    require(r.done());
}
struct Scalar {
    uint32_t kind = 0;
    std::string data;
    int64_t integer = 0;
    uint64_t uint = 0;
    double value = 0;
    bool flag = false;
    bool operator==(const Scalar &) const = default;
};
inline Scalar scalar(std::string_view s) {
    Scalar v;
    uint32_t present = 0;
    fields(s, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1: {
            auto n = number(r, f);
            require(n <= 6);
            v.kind = static_cast<uint32_t>(n);
            break;
        }
        case 2:
            v.data = bytes(r, f);
            present |= 1;
            break;
        case 3:
            v.integer = w::unzigzag(number(r, f));
            present |= 2;
            break;
        case 4:
            v.uint = number(r, f);
            present |= 4;
            break;
        case 5:
            v.value = real(r, f);
            present |= 8;
            break;
        case 6: {
            auto n = number(r, f);
            require(n <= 1);
            v.flag = n != 0;
            present |= 16;
            break;
        }
        default:
            return false;
        }
        return true;
    });
    const uint32_t allowed[] = {0, 1, 2, 4, 8, 16, 1};
    require((present & ~allowed[v.kind]) == 0 && v.data.size() <= 65536);
    return v;
}
inline std::string encode(const Scalar &v) {
    w::Writer x;
    x.uint_field(1, v.kind);
    if (v.kind == 1 || v.kind == 6)
        x.bytes_field(2, v.data);
    else if (v.kind == 2)
        x.sint_field(3, v.integer);
    else if (v.kind == 3)
        x.uint_field(4, v.uint);
    else if (v.kind == 4)
        x.double_field(5, v.value);
    else if (v.kind == 5)
        x.uint_field(6, v.flag);
    return x.take();
}
struct Text {
    std::string name, text;
    double weight = 1;
    bool operator==(const Text &) const = default;
};
struct TermField {
    uint64_t frequency = 0, length = 0;
    double weight = 1;
    bool operator==(const TermField &) const = default;
};
using TermFields = std::map<std::string, TermField>;
using Terms = std::map<std::string, TermFields>;
struct Document {
    std::string id, space, locator, payload;
    uint64_t version = 0, domain = 0, generation = 0, source = 0;
    bool deleted = false, has_domain = false, has_source = false, replace_metadata = false,
         vector_present = false;
    std::vector<Text> texts;
    std::map<std::string, Scalar> metadata;
    std::vector<float> vector;
    std::vector<std::string> remove;
    Terms terms;
    std::map<std::string, uint64_t> lengths;
};
inline std::vector<float> floats(std::string_view b) {
    require(b.size() % 4 == 0 && b.size() <= 4096 * 4);
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    if constexpr (std::endian::native == std::endian::little) {
        std::vector<float> result(b.size() / 4);
        if (!b.empty())
            std::memcpy(result.data(), b.data(), b.size());
        for (float f : result)
            require(std::isfinite(f));
        return result;
    }
    std::vector<float> result;
    result.reserve(b.size() / 4);
    for (size_t i = 0; i < b.size(); i += 4) {
        uint32_t bits = 0;
        for (unsigned j = 0; j < 4; ++j)
            bits |= uint32_t(static_cast<uint8_t>(b[i + j])) << (8 * j);
        float f = 0;
        std::memcpy(&f, &bits, 4);
        require(std::isfinite(f));
        result.push_back(f);
    }
    return result;
}
inline std::string float_bytes(const std::vector<float> &v) {
    if constexpr (std::endian::native == std::endian::little) {
        return v.empty() ? std::string()
                         : std::string(reinterpret_cast<const char *>(v.data()), v.size() * 4);
    }
    std::string s;
    s.reserve(v.size() * 4);
    for (float f : v) {
        uint32_t n = 0;
        std::memcpy(&n, &f, 4);
        for (unsigned j = 0; j < 4; ++j)
            s.push_back(static_cast<char>(n >> (8 * j)));
    }
    return s;
}
inline void normalize(std::vector<float> &v) {
    if (v.empty())
        return;
    double maximum = 0;
    for (float f : v) {
        require(std::isfinite(f));
        maximum = std::max(maximum, std::abs(double(f)));
    }
    require(maximum > 0);
    double sum = 0;
    for (float f : v) {
        double n = f / maximum;
        sum += n * n;
    }
    double divisor = std::sqrt(sum);
    for (float &f : v)
        f = static_cast<float>((f / maximum) / divisor);
}
inline TermFields term_fields(std::string_view s, bool bounds = false) {
    TermFields out;
    fields(s, [&](w::Reader &r, w::Field f) {
        if (f.number != 2)
            return false;
        std::string name;
        TermField t;
        fields(bytes(r, f), [&](w::Reader &a, w::Field b) {
            switch (b.number) {
            case 1:
                name = bytes(a, b);
                break;
            case 2:
                t.frequency = number(a, b);
                break;
            case 3:
                t.length = number(a, b);
                break;
            case 4:
                t.weight = real(a, b);
                break;
            default:
                return false;
            }
            return true;
        });
        require(!name.empty() && name.size() <= 128 && t.frequency && t.length &&
                (bounds || t.frequency <= t.length) && t.weight > 0 && t.weight <= 1000000);
        require(out.emplace(std::move(name), t).second);
        return true;
    });
    return out;
}
// header_only avoids copying bodies/positions when filtering posting candidates.
inline Document document(std::string_view s, bool header_only = false, bool internal = false,
                         bool vector_data = true) {
    Document d;
    fields(s, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1:
            d.id = bytes(r, f);
            break;
        case 2:
            d.version = number(r, f);
            break;
        case 3:
            d.domain = number(r, f);
            d.has_domain = true;
            break;
        case 4:
            d.generation = number(r, f);
            break;
        case 5: {
            auto b = bytes(r, f);
            if (header_only)
                break;
            Text t;
            fields(b, [&](w::Reader &a, w::Field q) {
                switch (q.number) {
                case 1:
                    t.name = bytes(a, q);
                    break;
                case 2:
                    t.text = bytes(a, q);
                    break;
                case 3:
                    t.weight = real(a, q);
                    break;
                default:
                    return false;
                }
                return true;
            });
            require(!t.name.empty() && t.name.size() <= 128 && t.weight > 0 &&
                    t.weight <= 1000000 && d.texts.size() < 32);
            d.texts.push_back(std::move(t));
            break;
        }
        case 6: {
            std::string name;
            Scalar v;
            fields(bytes(r, f), [&](w::Reader &a, w::Field q) {
                if (q.number == 1)
                    name = bytes(a, q);
                else if (q.number == 2)
                    v = scalar(bytes(a, q));
                else
                    return false;
                return true;
            });
            require(!name.empty() && name.size() <= 128 && d.metadata.size() < 64);
            require(d.metadata.emplace(std::move(name), std::move(v)).second);
            break;
        }
        case 7: {
            auto b = bytes(r, f);
            d.vector_present = !b.empty();
            if (vector_data)
                d.vector = floats(b);
            else
                require(b.size() % 4 == 0 && b.size() <= 4096 * 4);
            break;
        }
        case 8:
            d.space = bytes(r, f);
            break;
        case 9:
            d.locator = bytes(r, f);
            break;
        case 10:
            d.source = number(r, f);
            d.has_source = true;
            break;
        case 11: {
            auto b = bytes(r, f);
            if (!header_only)
                d.payload = b;
            break;
        }
        case 12: {
            auto n = number(r, f);
            require(n <= 1);
            d.deleted = n != 0;
            break;
        }
        case 13: {
            auto name = bytes(r, f);
            require(d.remove.size() < 64 && !name.empty() && name.size() <= 128);
            d.remove.emplace_back(name);
            break;
        }
        case 14: {
            auto n = number(r, f);
            require(n <= 1);
            d.replace_metadata = n != 0;
            break;
        }
        case 100: {
            auto b = bytes(r, f);
            if (!internal)
                break;
            std::string term;
            fields(b, [&](w::Reader &a, w::Field q) {
                if (q.number == 1) {
                    term = bytes(a, q);
                    return true;
                }
                return false;
            });
            require(!term.empty() && term.size() <= 512 && d.terms.size() < 8192);
            require(d.terms.emplace(std::move(term), term_fields(b)).second);
            break;
        }
        case 101: {
            auto b = bytes(r, f);
            if (!internal && !header_only)
                break;
            std::string name;
            uint64_t n = 0;
            fields(b, [&](w::Reader &a, w::Field q) {
                if (q.number == 1)
                    name = bytes(a, q);
                else if (q.number == 2)
                    n = number(a, q);
                else
                    return false;
                return true;
            });
            require(d.lengths.emplace(std::move(name), n).second);
            break;
        }
        default:
            return false;
        }
        return true;
    });
    require(d.id.size() <= 1024 && d.space.size() <= 512 && d.locator.size() <= 65536 &&
            d.payload.size() <= 1048576);
    std::sort(d.texts.begin(), d.texts.end(),
              [](const Text &a, const Text &b) { return a.name < b.name; });
    for (size_t i = 1; i < d.texts.size(); ++i)
        require(d.texts[i - 1].name != d.texts[i].name);
    return d;
}
inline std::string encode_term(std::string_view term, const TermFields &fs) {
    w::Writer x;
    x.bytes_field(1, term);
    for (const auto &[name, f] : fs) {
        w::Writer a;
        a.bytes_field(1, name);
        a.uint_field(2, f.frequency);
        a.uint_field(3, f.length);
        a.double_field(4, f.weight);
        x.bytes_field(2, a.data());
    }
    return x.take();
}
inline std::string encode(const Document &d, bool internal = false) {
    w::Writer x;
    x.bytes_field(1, d.id);
    x.uint_field(2, d.version);
    x.uint_field(3, d.domain);
    x.uint_field(4, d.generation);
    for (const auto &t : d.texts) {
        w::Writer a;
        a.bytes_field(1, t.name);
        a.bytes_field(2, t.text);
        a.double_field(3, t.weight);
        x.bytes_field(5, a.data());
    }
    for (const auto &[name, v] : d.metadata) {
        w::Writer a;
        a.bytes_field(1, name);
        a.bytes_field(2, encode(v));
        x.bytes_field(6, a.data());
    }
    if (!d.vector.empty())
        x.bytes_field(7, float_bytes(d.vector));
    if (!d.space.empty())
        x.bytes_field(8, d.space);
    if (!d.locator.empty())
        x.bytes_field(9, d.locator);
    x.uint_field(10, d.source);
    if (!d.payload.empty())
        x.bytes_field(11, d.payload);
    if (d.deleted)
        x.uint_field(12, 1);
    if (internal) {
        for (const auto &[term, fs] : d.terms)
            x.bytes_field(100, encode_term(term, fs));
        for (const auto &[name, n] : d.lengths) {
            w::Writer a;
            a.bytes_field(1, name);
            a.uint_field(2, n);
            x.bytes_field(101, a.data());
        }
    }
    return x.take();
}
struct Predicate {
    uint32_t op = 0;
    std::string field;
    std::vector<Scalar> values;
    std::vector<Predicate> children;
};
inline Predicate predicate(std::string_view s, unsigned depth = 0, unsigned *count = nullptr) {
    unsigned local = 0;
    if (!count)
        count = &local;
    require(depth <= 24 && ++*count <= 256);
    Predicate p;
    fields(s, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1: {
            auto n = number(r, f);
            require(n <= 11);
            p.op = static_cast<uint32_t>(n);
            break;
        }
        case 2:
            p.field = bytes(r, f);
            break;
        case 3:
            require(p.values.size() < 256);
            p.values.push_back(scalar(bytes(r, f)));
            break;
        case 4:
            p.children.push_back(predicate(bytes(r, f), depth + 1, count));
            break;
        default:
            return false;
        }
        return true;
    });
    require(p.field.size() <= 128);
    if (p.op >= 9)
        require(p.values.empty() && p.field.empty() && (p.op != 11 || p.children.size() == 1));
    else {
        require(p.children.empty());
        if (p.op >= 1 && p.op <= 6)
            require(!p.field.empty() && p.values.size() == 1);
        else if (p.op == 7)
            require(!p.field.empty() && !p.values.empty());
        else if (p.op == 8)
            require(!p.field.empty() && p.values.empty());
        else
            require(p.values.empty() && p.field.empty());
    }
    return p;
}
inline int compare(const Scalar &a, const Scalar &b) {
    // Never round large integers through double. Cross-kind values are distinct.
    require(a.kind == b.kind);
    if (a.kind == 1 || a.kind == 6)
        return a.data.compare(b.data);
    if (a.kind == 2)
        return (a.integer > b.integer) - (a.integer < b.integer);
    if (a.kind == 3)
        return (a.uint > b.uint) - (a.uint < b.uint);
    if (a.kind == 4)
        return (a.value > b.value) - (a.value < b.value);
    if (a.kind == 5)
        return int(a.flag) - int(b.flag);
    return 0;
}
inline bool matches(const Predicate &p, const std::map<std::string, Scalar> &metadata) {
    if (p.op == 0)
        return true;
    if (p.op == 9)
        return std::all_of(p.children.begin(), p.children.end(),
                           [&](const Predicate &q) { return matches(q, metadata); });
    if (p.op == 10)
        return std::any_of(p.children.begin(), p.children.end(),
                           [&](const Predicate &q) { return matches(q, metadata); });
    if (p.op == 11)
        return !matches(p.children.front(), metadata);
    auto i = metadata.find(p.field);
    if (p.op == 8)
        return i != metadata.end();
    if (i == metadata.end())
        return false;
    if (p.op == 7)
        return std::any_of(p.values.begin(), p.values.end(), [&](const Scalar &v) {
            return i->second.kind == v.kind && compare(i->second, v) == 0;
        });
    if (i->second.kind != p.values.front().kind)
        return false;
    int c = compare(i->second, p.values.front());
    switch (p.op) {
    case 1:
        return c == 0;
    case 2:
        return c != 0;
    case 3:
        return c < 0;
    case 4:
        return c <= 0;
    case 5:
        return c > 0;
    case 6:
        return c >= 0;
    default:
        return false;
    }
}
} // namespace fengqudb::index::impl
