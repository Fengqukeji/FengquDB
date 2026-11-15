#pragma once
#include "fengqudb_store.h"
#include "fengqudb_wire.hpp"
#include <yyjson.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

namespace business {
using Clock = std::chrono::steady_clock;
inline void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}
inline void fq(fqdb_status status) {
    if (status)
        throw std::runtime_error(fqdb_status_message(status));
}
inline fqdb_slice slice(std::string_view v) {
    return {v.data(), v.size()};
}
constexpr size_t batch_size = 32;
constexpr int64_t initial_credit = 1000000000;
struct Config {
    std::string engine = "fqdb", mode = "buffered", workload = "C", action = "run",
                data = "/data/client";
    unsigned records = 10000, body = 128, threads = 1;
    double seconds = 2;
    uint64_t threshold_bytes = 1048576;
    uint32_t period_ms = 50;
};
struct Profile {
    uint64_t id = 0, revision = 0;
    int64_t credit = 0;
    uint64_t level = 0, status = 0;
    std::string name, email, bio;
    uint64_t region = 0, last_login = 0;
    bool operator==(const Profile &b) const {
        return id == b.id && revision == b.revision && credit == b.credit && level == b.level &&
               status == b.status && name == b.name && email == b.email && bio == b.bio &&
               region == b.region && last_login == b.last_login;
    }
};
struct Projection {
    uint64_t revision;
    int64_t credit;
};
inline Profile fixture(uint64_t id, size_t body) {
    Profile p;
    p.id = id;
    p.credit = initial_credit + static_cast<int64_t>(id);
    p.level = 1 + id % 100;
    p.status = id % 3;
    p.region = id % 16;
    p.last_login = 1700000000 + id;
    p.name = "user/" + std::to_string(id) + "/玩家";
    p.email = "u" + std::to_string(id) + "@example.test";
    p.bio.resize(body);
    for (size_t i = 0; i < body; ++i)
        p.bio[i] = char('a' + (id + i) % 26);
    if (body >= 8) {
        p.bio[3] = '"';
        p.bio[5] = '\n';
        p.bio[7] = '\\';
    }
    return p;
}
inline uint64_t score(const Profile &p) {
    uint64_t out = p.id + p.revision + static_cast<uint64_t>(p.credit) + p.level + p.status +
                   p.region + p.last_login;
    for (const auto *s : {&p.name, &p.email, &p.bio}) {
        out += s->size();
        if (!s->empty())
            out += static_cast<unsigned char>(s->front()) + static_cast<unsigned char>(s->back());
    }
    return out;
}
inline std::string encode_pb(const Profile &p) {
    fengqudb::wire::Writer w;
    w.reserve(p.bio.size() + 160);
    w.fixed64_field(1, p.id);
    w.fixed64_field(2, p.revision);
    w.fixed64_field(3, static_cast<uint64_t>(p.credit));
    w.uint_field(4, p.level);
    w.uint_field(5, p.status);
    w.bytes_field(6, p.name);
    w.bytes_field(7, p.email);
    w.bytes_field(8, p.bio);
    w.uint_field(9, p.region);
    w.fixed64_field(10, p.last_login);
    return w.take();
}
inline Profile decode_pb(std::string_view bytes) {
    fengqudb::wire::Reader r(bytes);
    fengqudb::wire::Field f;
    Profile p;
    unsigned fields = 0;
    while (r.next(f)) {
        uint64_t v = 0;
        std::string_view s;
        if (f.number == 1 || f.number == 2 || f.number == 3 || f.number == 10) {
            require(r.fixed(v, 8), "bad protobuf fixed");
            if (f.number == 1)
                p.id = v;
            else if (f.number == 2)
                p.revision = v;
            else if (f.number == 3)
                p.credit = static_cast<int64_t>(v);
            else
                p.last_login = v;
        } else if (f.number == 4 || f.number == 5 || f.number == 9) {
            require(r.varint(v), "bad protobuf integer");
            if (f.number == 4)
                p.level = v;
            else if (f.number == 5)
                p.status = v;
            else
                p.region = v;
        } else if (f.number >= 6 && f.number <= 8) {
            require(r.bytes(s), "bad protobuf string");
            (f.number == 6 ? p.name : f.number == 7 ? p.email : p.bio).assign(s);
        } else {
            require(r.skip(f), "bad protobuf unknown");
            continue;
        }
        ++fields;
    }
    require(r.done() && fields == 10, "incomplete protobuf profile");
    return p;
}
inline Projection project_pb(std::string_view bytes) {
    // The fixed schema header is validated before the lazy field projection.
    require(bytes.size() >= 27 && bytes[0] == 9 && bytes[9] == 17 && bytes[18] == 25,
            "bad protobuf header");
    uint64_t rev = 0, credit = 0;
    for (unsigned i = 0; i < 8; ++i) {
        rev |= uint64_t(static_cast<unsigned char>(bytes[10 + i])) << (8 * i);
        credit |= uint64_t(static_cast<unsigned char>(bytes[19 + i])) << (8 * i);
    }
    return {rev, static_cast<int64_t>(credit)};
}
inline std::array<char, 8> fixed64(uint64_t value) {
    std::array<char, 8> out{};
    for (unsigned i = 0; i < 8; ++i)
        out[i] = static_cast<char>(value >> (8 * i));
    return out;
}
inline std::string encode_json(const Profile &p) {
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr),
                                                                        yyjson_mut_doc_free);
    require(bool(doc), "JSON allocation");
    auto root = yyjson_mut_obj(doc.get());
    yyjson_mut_doc_set_root(doc.get(), root);
    bool ok =
        yyjson_mut_obj_add_uint(doc.get(), root, "id", p.id) &&
        yyjson_mut_obj_add_uint(doc.get(), root, "revision", p.revision) &&
        yyjson_mut_obj_add_sint(doc.get(), root, "credit", p.credit) &&
        yyjson_mut_obj_add_uint(doc.get(), root, "level", p.level) &&
        yyjson_mut_obj_add_uint(doc.get(), root, "status", p.status) &&
        yyjson_mut_obj_add_strncpy(doc.get(), root, "name", p.name.data(), p.name.size()) &&
        yyjson_mut_obj_add_strncpy(doc.get(), root, "email", p.email.data(), p.email.size()) &&
        yyjson_mut_obj_add_strncpy(doc.get(), root, "bio", p.bio.data(), p.bio.size()) &&
        yyjson_mut_obj_add_uint(doc.get(), root, "region", p.region) &&
        yyjson_mut_obj_add_uint(doc.get(), root, "last_login", p.last_login);
    require(ok, "JSON construction");
    size_t count = 0;
    std::unique_ptr<char, decltype(&std::free)> text(yyjson_mut_write(doc.get(), 0, &count),
                                                     std::free);
    require(bool(text), "JSON serialization");
    return {text.get(), count};
}
inline Profile decode_json(std::string_view text) {
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(
        yyjson_read(text.data(), text.size(), 0), yyjson_doc_free);
    require(bool(doc), "JSON parse");
    auto root = yyjson_doc_get_root(doc.get());
    Profile p;
    auto u = [&](const char *k) {
        auto v = yyjson_obj_get(root, k);
        require(yyjson_is_uint(v), "bad JSON integer");
        return yyjson_get_uint(v);
    };
    auto s = [&](const char *k) {
        auto v = yyjson_obj_get(root, k);
        require(yyjson_is_str(v), "bad JSON string");
        return std::string(yyjson_get_str(v), yyjson_get_len(v));
    };
    p.id = u("id");
    p.revision = u("revision");
    auto credit = yyjson_obj_get(root, "credit");
    require(yyjson_is_int(credit), "bad JSON credit");
    p.credit = yyjson_get_sint(credit);
    p.level = u("level");
    p.status = u("status");
    p.name = s("name");
    p.email = s("email");
    p.bio = s("bio");
    p.region = u("region");
    p.last_login = u("last_login");
    return p;
}
inline std::string key(uint64_t id) {
    return "user/" + std::to_string(id);
}
struct Metrics {
    uint64_t retries = 0, hits = 0, misses = 0;
};
class Connection {
  public:
    Metrics metrics;
    virtual ~Connection() = default;
    virtual Profile get(uint64_t id) = 0;
    virtual Projection project(uint64_t id) {
        auto p = get(id);
        return {p.revision, p.credit};
    }
    virtual void revision(uint64_t id, uint64_t value) = 0;
    virtual void increment(uint64_t id) = 0;
    virtual void transfer(uint64_t from, uint64_t to) = 0;
    virtual void put(const Profile &p) = 0;
    virtual void erase(uint64_t id) = 0;
    virtual uint64_t count() = 0;
    virtual void replace(const Profile &p) {
        put(p);
    }
    virtual std::vector<Profile> many(uint64_t first) {
        std::vector<Profile> out;
        out.reserve(batch_size);
        for (size_t i = 0; i < batch_size; ++i)
            out.push_back(get(first + i));
        return out;
    }
    virtual std::vector<Profile> select(const std::vector<uint64_t> &ids) {
        std::vector<Profile> out;
        out.reserve(ids.size());
        for (auto id : ids)
            out.push_back(get(id));
        return out;
    }
    virtual void seed_begin() {}
    virtual void seed_end() {}
};
class Engine {
  public:
    Config config;
    explicit Engine(Config c) : config(std::move(c)) {}
    virtual ~Engine() = default;
    virtual std::unique_ptr<Connection> connect() = 0;
    virtual void reset() = 0;
    virtual void sync() = 0;
    virtual std::string info() {
        return "{}";
    }
};
std::unique_ptr<Engine> make_fq(const Config &);
std::unique_ptr<Engine> make_sqlite(const Config &);
std::unique_ptr<Engine> make_mysql(const Config &);
std::unique_ptr<Engine> make_redis(const Config &);
std::unique_ptr<Engine> make_hybrid(const Config &);
} // namespace business
