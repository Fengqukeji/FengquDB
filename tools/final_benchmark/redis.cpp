#include "common.hpp"
#include <hiredis/hiredis.h>

namespace final_bench {
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
class RedisWire {
    std::unique_ptr<redisContext, decltype(&redisFree)> context_{nullptr, redisFree};
    template <class Range> Reply invoke(const Range &args, bool buffered) {
        std::array<const char *, 1 + batch_size * 2> small_argv;
        std::array<size_t, 1 + batch_size * 2> small_lengths;
        std::vector<const char *> large_argv;
        std::vector<size_t> large_lengths;
        auto argv = small_argv.data();
        auto lengths = small_lengths.data();
        if (args.size() > small_argv.size()) {
            large_argv.resize(args.size());
            large_lengths.resize(args.size());
            argv = large_argv.data();
            lengths = large_lengths.data();
        }
        size_t at = 0;
        for (auto value : args) {
            argv[at] = value.data();
            lengths[at++] = value.size();
        }
        if (buffered) {
            require(redisAppendCommandArgv(context_.get(), args.size(), argv, lengths) == REDIS_OK,
                    "Redis append failed");
            return {nullptr, freeReplyObject};
        }
        Reply result(
            static_cast<redisReply *>(redisCommandArgv(context_.get(), args.size(), argv, lengths)),
            freeReplyObject);
        if (!result)
            throw std::runtime_error("Redis command: " + std::string(context_->errstr));
        if (result->type == REDIS_REPLY_ERROR)
            throw std::runtime_error("Redis error: " + std::string(result->str, result->len));
        return result;
    }

  public:
    RedisWire() {
        timeval timeout{30, 0};
        context_.reset(redisConnectWithTimeout("127.0.0.1", 6379, timeout));
        require(context_ && !context_->err, "Redis connect");
        require(redisSetTimeout(context_.get(), timeout) == REDIS_OK, "Redis timeout");
    }
    Reply command(const std::vector<std::string_view> &args) {
        return invoke(args, false);
    }
    template <size_t N> Reply command(const std::array<std::string_view, N> &args) {
        return invoke(args, false);
    }
    Reply command(std::initializer_list<std::string_view> args) {
        return invoke(args, false);
    }
    void append(std::initializer_list<std::string_view> args) {
        (void)invoke(args, true);
    }
    Reply receive() {
        void *raw = nullptr;
        require(redisGetReply(context_.get(), &raw) == REDIS_OK && raw, "Redis receive failed");
        Reply result(static_cast<redisReply *>(raw), freeReplyObject);
        require(result->type != REDIS_REPLY_ERROR, "Redis pipelined error");
        return result;
    }
    static void status(const redisReply &reply, std::string_view expected = "OK") {
        require(reply.type == REDIS_REPLY_STATUS &&
                    std::string_view(reply.str, reply.len) == expected,
                "Redis status mismatch");
    }
    void seed_cache(const std::vector<Row> &rows) {
        for (auto &row : rows)
            append({"SET", row.key, row.value});
        for (size_t i = 0; i < rows.size(); ++i)
            status(*receive());
    }
};
class RedisConnection final : public Connection {
    RedisWire wire_;
    size_t bytes_;

  public:
    explicit RedisConnection(size_t bytes) : bytes_(bytes) {}
    uint64_t read(uint64_t id, bool, bool missing) override {
        auto k = key(id);
        auto value = wire_.command({"GET", k});
        if (missing) {
            require(value->type == REDIS_REPLY_NIL, "Redis expected miss");
            return id;
        }
        require(value->type == REDIS_REPLY_STRING, "Redis expected string");
        return consume(value->str, value->len, id, bytes_);
    }
    void put(uint64_t id, std::string_view value) override {
        auto k = key(id);
        RedisWire::status(*wire_.command({"SET", k, value}));
    }
    void erase(uint64_t id) override {
        auto k = key(id);
        auto result = wire_.command({"DEL", k});
        require(result->type == REDIS_REPLY_INTEGER && result->integer == 1,
                "Redis delete missing");
    }
    void patch(uint64_t id, const std::array<char, 8> &value) override {
        auto k = key(id);
        auto result = wire_.command({"SETRANGE", k, "10", {value.data(), value.size()}});
        require(result->type == REDIS_REPLY_INTEGER &&
                    result->integer == static_cast<long long>(bytes_),
                "Redis patch length");
    }
    void put_batch(const std::array<Row, batch_size> &rows) override {
        std::array<std::string_view, 1 + batch_size * 2> args;
        args[0] = "MSET";
        for (size_t i = 0; i < rows.size(); ++i) {
            args[1 + i * 2] = rows[i].key;
            args[2 + i * 2] = rows[i].value;
        }
        RedisWire::status(*wire_.command(args));
    }
    uint64_t read_batch(uint64_t first) override {
        std::array<std::string, batch_size> keys;
        std::vector<std::string_view> args{"MGET"};
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = key(first + i);
            args.push_back(keys[i]);
        }
        auto result = wire_.command(args);
        require(result->type == REDIS_REPLY_ARRAY && result->elements == batch_size,
                "Redis MGET count");
        uint64_t score = 0;
        for (size_t i = 0; i < batch_size; ++i) {
            auto value = result->element[i];
            require(value->type == REDIS_REPLY_STRING, "Redis MGET missing");
            score += consume(value->str, value->len, first + i, bytes_);
        }
        return score;
    }
    void scan(const Visitor &visitor) override {
        std::string cursor = "0";
        do {
            auto reply = wire_.command({"SCAN", cursor, "MATCH", "k*", "COUNT", "512"});
            require(reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
                        reply->element[0]->type == REDIS_REPLY_STRING &&
                        reply->element[1]->type == REDIS_REPLY_ARRAY,
                    "Redis SCAN shape");
            cursor.assign(reply->element[0]->str, reply->element[0]->len);
            auto names = reply->element[1];
            if (names->elements) {
                std::vector<std::string_view> args{"MGET"};
                for (size_t i = 0; i < names->elements; ++i) {
                    auto name = names->element[i];
                    require(name->type == REDIS_REPLY_STRING, "Redis SCAN key");
                    args.emplace_back(name->str, name->len);
                }
                auto values = wire_.command(args);
                require(values->type == REDIS_REPLY_ARRAY && values->elements == names->elements,
                        "Redis scan values");
                for (size_t i = 0; i < names->elements; ++i) {
                    auto value = values->element[i], name = names->element[i];
                    require(value->type == REDIS_REPLY_STRING, "Redis scan missing");
                    visitor({name->str, name->len}, {value->str, value->len});
                }
            }
        } while (cursor != "0");
    }
};
class RedisEngine final : public Engine {
    Config config_;
    RedisWire control_;
    std::string version_;

  public:
    explicit RedisEngine(const Config &config) : config_(config) {
        RedisWire::status(*control_.command(
            {"CONFIG", "SET", "appendfsync",
             config.mode == "buffered" || config.action == "prepare" ? "no" : "always"}));
        auto info = control_.command({"INFO", "server"});
        require(info->type == REDIS_REPLY_STRING, "Redis version INFO");
        std::string_view text(info->str, info->len);
        auto at = text.find("redis_version:");
        require(at != text.npos, "Redis version field");
        at += 14;
        version_ = text.substr(at, text.find('\r', at) - at);
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<RedisConnection>(config_.payload);
    }
    void sync() override {
        RedisWire::status(*control_.command({"CONFIG", "SET", "appendfsync", "always"}));
        auto stamp = std::to_string(Clock::now().time_since_epoch().count());
        RedisWire::status(*control_.command({"SET", "__sync__", stamp}));
        auto reply = control_.command({"WAITAOF", "1", "0", "10000"});
        require(reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
                    reply->element[0]->type == REDIS_REPLY_INTEGER &&
                    reply->element[0]->integer == 1,
                "Redis final AOF sync");
        RedisWire::status(*control_.command(
            {"CONFIG", "SET", "appendfsync",
             config_.mode == "buffered" || config_.action == "prepare" ? "no" : "always"}));
    }
    std::string version() const override {
        return version_;
    }
};
struct HybridState {
    std::array<std::shared_mutex, 1024> locks;
};
inline std::string cache_key(uint64_t id) {
    return "c" + key(id);
}
class HybridConnection final : public Connection {
    std::unique_ptr<Connection> backend_;
    RedisWire cache_;
    HybridState &state_;
    size_t bytes_;
    std::vector<size_t> shards(uint64_t first) const {
        std::vector<size_t> result;
        for (size_t i = 0; i < batch_size; ++i)
            result.push_back((first + i) % state_.locks.size());
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

  public:
    HybridConnection(std::unique_ptr<Connection> backend, HybridState &state, size_t bytes)
        : backend_(std::move(backend)), state_(state), bytes_(bytes) {}
    uint64_t read(uint64_t id, bool, bool missing) override {
        std::shared_lock<std::shared_mutex> lock(state_.locks[id % state_.locks.size()]);
        // Cache replies already own a byte copy. Avoid another payload copy on
        // hits; the fallback materializes the authoritative MySQL row once.
        auto k = cache_key(id);
        auto reply = cache_.command({"GET", k});
        if (reply->type == REDIS_REPLY_STRING) {
            require(reply->len >= 1, "cache response length");
            if (reply->str[0] == 0) {
                require(missing && reply->len == 1, "unexpected negative cache");
                return id;
            }
            require(!missing && reply->str[0] == 1, "cache presence mismatch");
            return consume(reply->str + 1, reply->len - 1, id, bytes_);
        }
        require(reply->type == REDIS_REPLY_NIL, "cache response type");
        auto value = backend_->raw(id);
        require(bool(value) != missing, "hybrid visibility");
        std::string encoded(1, value ? 1 : 0);
        if (value)
            encoded += *value;
        RedisWire::status(*cache_.command({"SET", k, encoded}));
        return missing ? id : consume(value->data(), value->size(), id, bytes_);
    }
    void put(uint64_t id, std::string_view value) override {
        std::unique_lock<std::shared_mutex> lock(state_.locks[id % state_.locks.size()]);
        backend_->put(id, value);
        auto k = cache_key(id);
        require(cache_.command({"DEL", k})->type == REDIS_REPLY_INTEGER, "hybrid invalidation");
    }
    void erase(uint64_t id) override {
        std::unique_lock<std::shared_mutex> lock(state_.locks[id % state_.locks.size()]);
        backend_->erase(id);
        auto k = cache_key(id);
        require(cache_.command({"DEL", k})->type == REDIS_REPLY_INTEGER, "hybrid invalidation");
    }
    void patch(uint64_t id, const std::array<char, 8> &value) override {
        std::unique_lock<std::shared_mutex> lock(state_.locks[id % state_.locks.size()]);
        backend_->patch(id, value);
        auto k = cache_key(id);
        require(cache_.command({"DEL", k})->type == REDIS_REPLY_INTEGER, "hybrid invalidation");
    }
    void put_batch(const std::array<Row, batch_size> &rows) override {
        std::vector<size_t> indices;
        for (auto &row : rows)
            indices.push_back(key_id(row.key) % state_.locks.size());
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        std::vector<std::unique_lock<std::shared_mutex>> locks;
        for (auto i : indices)
            locks.emplace_back(state_.locks[i]);
        backend_->put_batch(rows);
        std::array<std::string, batch_size> keys;
        std::vector<std::string_view> args{"DEL"};
        for (size_t i = 0; i < rows.size(); ++i) {
            keys[i] = cache_key(key_id(rows[i].key));
            args.push_back(keys[i]);
        }
        require(cache_.command(args)->type == REDIS_REPLY_INTEGER, "hybrid batch invalidation");
    }
    uint64_t read_batch(uint64_t first) override {
        std::vector<std::shared_lock<std::shared_mutex>> locks;
        for (auto i : shards(first))
            locks.emplace_back(state_.locks[i]);
        std::array<std::string, batch_size> keys;
        std::vector<std::string_view> args{"MGET"};
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = cache_key(first + i);
            args.push_back(keys[i]);
        }
        auto reply = cache_.command(args);
        require(reply->type == REDIS_REPLY_ARRAY && reply->elements == batch_size, "hybrid MGET");
        size_t missing = 0;
        for (size_t i = 0; i < keys.size(); ++i)
            missing += reply->element[i]->type == REDIS_REPLY_NIL;
        std::array<std::string, batch_size> loaded;
        std::vector<Row> fill;
        if (missing) {
            if (missing > 1)
                loaded = backend_->raw_batch(first);
            for (size_t i = 0; i < keys.size(); ++i)
                if (reply->element[i]->type == REDIS_REPLY_NIL) {
                    if (missing == 1) {
                        auto value = backend_->raw(first + i);
                        require(bool(value), "hybrid batch missing");
                        loaded[i] = std::move(*value);
                    }
                    fill.push_back({keys[i], std::string(1, 1) + loaded[i]});
                }
            cache_.seed_cache(fill);
        }
        uint64_t score = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            auto value = reply->element[i];
            if (value->type == REDIS_REPLY_NIL) {
                score += consume(loaded[i].data(), loaded[i].size(), first + i, bytes_);
            } else {
                require(value->type == REDIS_REPLY_STRING && value->len > 1 && value->str[0] == 1,
                        "hybrid batch flag");
                score += consume(value->str + 1, value->len - 1, first + i, bytes_);
            }
        }
        return score;
    }
    void scan(const Visitor &visitor) override {
        backend_->scan(visitor);
    }
    uint64_t retry_count() const override {
        return backend_->retry_count();
    }
};
class HybridEngine final : public Engine {
    Config config_;
    std::unique_ptr<Engine> backend_;
    HybridState state_;

  public:
    explicit HybridEngine(const Config &config) : config_(config), backend_(make_mysql(config)) {}
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<HybridConnection>(backend_->connect(), state_, config_.payload);
    }
    void sync() override {
        backend_->sync();
    }
    void warm() override {
        auto connection = backend_->connect();
        RedisWire cache;
        std::vector<Row> rows;
        rows.reserve(batch_size);
        connection->scan([&](std::string_view k, std::string_view v) {
            rows.push_back({cache_key(key_id(k)), std::string(1, 1) + std::string(v)});
            if (rows.size() == batch_size) {
                cache.seed_cache(rows);
                rows.clear();
            }
        });
        if (!rows.empty())
            cache.seed_cache(rows);
    }
    void thread_cleanup() override {
        backend_->thread_cleanup();
    }
    std::string version() const override {
        return "MySQL " + backend_->version() + " + Redis 8.10.3";
    }
};
std::unique_ptr<Engine> make_redis(const Config &config) {
    return std::make_unique<RedisEngine>(config);
}
std::unique_ptr<Engine> make_hybrid(const Config &config) {
    return std::make_unique<HybridEngine>(config);
}
} // namespace final_bench
