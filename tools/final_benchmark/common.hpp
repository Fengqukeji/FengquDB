#pragma once
#include "fengqudb_store.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace final_bench {
constexpr size_t batch_size = 32;
using Clock = std::chrono::steady_clock;
inline void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
inline void fq_check(fqdb_status status) {
    if (status != FQDB_OK)
        throw std::runtime_error(std::string("Store: ") + fqdb_status_message(status));
}
struct Config {
    std::string engine, mode = "sync", workload = "get_copy", action = "run",
                        directory = "/data/db";
    unsigned threads = 1, fq_group_max = 64;
    size_t records = 131072, payload = 256, fixed_calls = 4096;
    double seconds = 3, warmup_seconds = .2;
    bool copy_reads() const {
        return workload != "get_borrow";
    }
};
inline std::string key(uint64_t id) {
    std::string value(9, '\0');
    value[0] = 'k';
    for (unsigned i = 0; i < 8; ++i)
        value[1 + i] = static_cast<char>(id >> (56 - 8 * i));
    return value;
}
inline uint64_t key_id(std::string_view value) {
    require(value.size() == 9 && value[0] == 'k', "unexpected key");
    uint64_t id = 0;
    for (size_t i = 1; i < 9; ++i)
        id = (id << 8) | static_cast<unsigned char>(value[i]);
    return id;
}
inline uint64_t get64(const char *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return value;
}
inline std::array<char, 8> revision_bytes(uint64_t revision) {
    std::array<char, 8> value{};
    for (unsigned i = 0; i < 8; ++i)
        value[i] = static_cast<char>(revision >> (8 * i));
    return value;
}
// A valid Protobuf message of exactly the requested size: fixed64 id/revision
// followed by one bytes field. Padding depends on id, so an 8-byte patch can
// update just revision without changing any other field or length prefix.
inline std::string payload(uint64_t id, uint64_t revision, size_t size) {
    require(size >= 32 && size <= 1048576, "payload bounds");
    size_t padding = size - 21;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        auto n = padding;
        size_t width = 1;
        while (n >= 128) {
            ++width;
            n >>= 7;
        }
        padding = size - 19 - width;
    }
    std::string result;
    result.reserve(size);
    result.push_back(9);
    auto id_bytes = revision_bytes(id), rev = revision_bytes(revision);
    result.append(id_bytes.data(), 8);
    result.push_back(17);
    result.append(rev.data(), 8);
    result.push_back(26);
    auto n = padding;
    while (n >= 128) {
        result.push_back(static_cast<char>((n & 127) | 128));
        n >>= 7;
    }
    result.push_back(static_cast<char>(n));
    result.append(padding, static_cast<char>('a' + id % 26));
    require(result.size() == size, "payload framing");
    return result;
}
inline uint64_t consume(const char *data, size_t size, uint64_t id, size_t expected) {
    require(data && size == expected && data[0] == 9 && data[9] == 17 && data[18] == 26 &&
                get64(data + 1) == id && data[size - 1] == static_cast<char>('a' + id % 26),
            "read payload mismatch");
    return get64(data + 10) + id + size + static_cast<unsigned char>(data[size - 1]);
}
struct Row {
    std::string key, value;
};
using Visitor = std::function<void(std::string_view, std::string_view)>;
class Connection {
  protected:
    uint64_t retries_ = 0;

  public:
    virtual ~Connection() = default;
    virtual uint64_t read(uint64_t id, bool copy, bool missing = false) = 0;
    virtual std::optional<std::string> raw(uint64_t) {
        throw std::runtime_error("raw read unsupported");
    }
    virtual std::array<std::string, batch_size> raw_batch(uint64_t) {
        throw std::runtime_error("raw batch unsupported");
    }
    virtual uint64_t retry_count() const {
        return retries_;
    }
    virtual void put(uint64_t id, std::string_view value) = 0;
    virtual void erase(uint64_t id) = 0;
    virtual void patch(uint64_t id, const std::array<char, 8> &value) = 0;
    virtual void put_batch(const std::array<Row, batch_size> &rows) = 0;
    virtual uint64_t read_batch(uint64_t first) = 0;
    virtual void scan(const Visitor &visitor) = 0;
};
class Engine {
  public:
    virtual ~Engine() = default;
    virtual std::unique_ptr<Connection> connect() = 0;
    virtual void sync() = 0;
    virtual void warm() {
        auto connection = connect();
        connection->scan([](std::string_view, std::string_view) {});
    }
    virtual void thread_cleanup() {}
    virtual std::string metrics() {
        return "{}";
    }
    virtual std::string version() const = 0;
};
std::unique_ptr<Engine> make_fq(const Config &);
std::unique_ptr<Engine> make_sqlite(const Config &);
std::unique_ptr<Engine> make_mysql(const Config &);
std::unique_ptr<Engine> make_redis(const Config &);
std::unique_ptr<Engine> make_hybrid(const Config &);
inline std::unique_ptr<Engine> make_engine(const Config &config) {
    if (config.engine == "fqdb")
        return make_fq(config);
    if (config.engine == "sqlite")
        return make_sqlite(config);
    if (config.engine == "mysql")
        return make_mysql(config);
    if (config.engine == "redis")
        return make_redis(config);
    if (config.engine == "hybrid")
        return make_hybrid(config);
    throw std::runtime_error("unknown engine");
}
inline fqdb_slice slice(std::string_view bytes) {
    return {bytes.data(), bytes.size()};
}
} // namespace final_bench
