#include "common.hpp"
#include <hiredis/hiredis.h>
namespace business {
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
class Wire {
    std::unique_ptr<redisContext, decltype(&redisFree)> db_{nullptr, redisFree};

  public:
    Wire() {
        timeval timeout{30, 0};
        db_.reset(redisConnectWithTimeout("127.0.0.1", 6379, timeout));
        require(db_ && !db_->err, "Redis connect");
        require(redisSetTimeout(db_.get(), timeout) == REDIS_OK, "Redis timeout");
    }
    void append(const std::vector<std::string_view> &args) {
        std::array<const char *, 80> argv{};
        std::array<size_t, 80> lengths{};
        require(args.size() <= argv.size(), "Redis arguments");
        for (size_t i = 0; i < args.size(); ++i) {
            argv[i] = args[i].data();
            lengths[i] = args[i].size();
        }
        require(redisAppendCommandArgv(db_.get(), args.size(), argv.data(), lengths.data()) ==
                    REDIS_OK,
                "Redis append");
    }
    Reply next() {
        void *raw = nullptr;
        require(redisGetReply(db_.get(), &raw) == REDIS_OK && raw, "Redis reply");
        Reply out(static_cast<redisReply *>(raw), freeReplyObject);
        if (out->type == REDIS_REPLY_ERROR)
            throw std::runtime_error("Redis: " + std::string(out->str, out->len));
        return out;
    }
    Reply command(const std::vector<std::string_view> &args) {
        append(args);
        return next();
    }
};
static const std::array<std::string_view, 10> field_names{
    "id", "revision", "credit", "level", "status", "name", "email", "bio", "region", "last_login"};
static std::string_view bytes(const redisReply *r) {
    require(r->type == REDIS_REPLY_STRING, "Redis missing/string type");
    return {r->str, r->len};
}
static uint64_t unsigned_number(const redisReply *r) {
    auto s = bytes(r);
    uint64_t n = 0;
    require(!s.empty(), "empty Redis integer");
    for (char c : s) {
        require(c >= '0' && c <= '9', "Redis integer parse");
        n = n * 10 + unsigned(c - '0');
    }
    return n;
}
static int64_t signed_number(const redisReply *r) {
    auto s = bytes(r);
    if (!s.empty() && s[0] == '-') {
        uint64_t n = 0;
        for (size_t i = 1; i < s.size(); ++i) {
            require(s[i] >= '0' && s[i] <= '9', "Redis signed integer");
            n = n * 10 + unsigned(s[i] - '0');
        }
        return -static_cast<int64_t>(n);
    }
    return static_cast<int64_t>(unsigned_number(r));
}
static Profile profile(const redisReply *r) {
    require(r->type == REDIS_REPLY_ARRAY && r->elements == 10, "Redis profile fields");
    auto v = r->element;
    Profile p;
    p.id = unsigned_number(v[0]);
    p.revision = unsigned_number(v[1]);
    p.credit = signed_number(v[2]);
    p.level = unsigned_number(v[3]);
    p.status = unsigned_number(v[4]);
    p.name = bytes(v[5]);
    p.email = bytes(v[6]);
    p.bio = bytes(v[7]);
    p.region = unsigned_number(v[8]);
    p.last_login = unsigned_number(v[9]);
    return p;
}
class RedisConnection final : public Connection {
    Wire db_;
    std::string transfer_sha_;
    bool seed_ = false;
    size_t pending_ = 0;
    std::vector<std::string_view> read_args(const std::string &name) {
        std::vector<std::string_view> out{"HMGET", name};
        out.insert(out.end(), field_names.begin(), field_names.end());
        return out;
    }

  public:
    RedisConnection() {
        const char *script = "local a=redis.call('HGET',KEYS[1],'credit');local "
                             "b=redis.call('HGET',KEYS[2],'credit');if not a or not b or "
                             "tonumber(a)<1 then return 0 "
                             "end;redis.call('HINCRBY',KEYS[1],'credit',-1);redis.call('HINCRBY',"
                             "KEYS[2],'credit',1);return 1";
        auto reply = db_.command({"SCRIPT", "LOAD", script});
        transfer_sha_ = bytes(reply.get());
    }
    Profile get(uint64_t id) override {
        auto name = key(id);
        auto r = db_.command(read_args(name));
        return profile(r.get());
    }
    Projection project(uint64_t id) override {
        auto name = key(id);
        auto r = db_.command({"HMGET", name, "revision", "credit"});
        require(r->type == REDIS_REPLY_ARRAY && r->elements == 2, "Redis projection fields");
        return {unsigned_number(r->element[0]), signed_number(r->element[1])};
    }
    void revision(uint64_t id, uint64_t value) override {
        auto name = key(id), v = std::to_string(value);
        auto r = db_.command({"HSET", name, "revision", v});
        require(r->type == REDIS_REPLY_INTEGER, "Redis HSET result");
    }
    void increment(uint64_t id) override {
        auto name = key(id);
        auto r = db_.command({"HINCRBY", name, "credit", "1"});
        require(r->type == REDIS_REPLY_INTEGER, "Redis HINCRBY result");
    }
    void transfer(uint64_t from, uint64_t to) override {
        auto a = key(from), b = key(to);
        auto r = db_.command({"EVALSHA", transfer_sha_, "2", a, b});
        require(r->type == REDIS_REPLY_INTEGER && r->integer == 1, "Redis transfer failed");
    }
    void put(const Profile &p) override {
        auto name = key(p.id);
        std::array<std::string, 10> values{std::to_string(p.id),
                                           std::to_string(p.revision),
                                           std::to_string(p.credit),
                                           std::to_string(p.level),
                                           std::to_string(p.status),
                                           p.name,
                                           p.email,
                                           p.bio,
                                           std::to_string(p.region),
                                           std::to_string(p.last_login)};
        std::vector<std::string_view> args{"HSET", name};
        for (unsigned i = 0; i < 10; ++i) {
            args.push_back(field_names[i]);
            args.push_back(values[i]);
        }
        if (seed_) {
            db_.append(args);
            ++pending_;
        } else {
            auto r = db_.command(args);
            require(r->type == REDIS_REPLY_INTEGER, "Redis put result");
        }
    }
    std::vector<Profile> many(uint64_t first) override {
        std::array<std::string, batch_size> names;
        for (unsigned i = 0; i < batch_size; ++i) {
            names[i] = key(first + i);
            db_.append(read_args(names[i]));
        }
        std::vector<Profile> out;
        out.reserve(batch_size);
        for (unsigned i = 0; i < batch_size; ++i) {
            auto r = db_.next();
            out.push_back(profile(r.get()));
        }
        return out;
    }
    void seed_begin() override {
        seed_ = true;
        pending_ = 0;
    }
    void seed_end() override {
        for (size_t i = 0; i < pending_; ++i) {
            auto r = db_.next();
            require(r->type == REDIS_REPLY_INTEGER, "Redis seed failed");
        }
        pending_ = 0;
        seed_ = false;
    }
};
class RedisEngine final : public Engine {
    Wire control_;

  public:
    explicit RedisEngine(const Config &c) : Engine(c) {}
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<RedisConnection>();
    }
    void reset() override {
        control_.command({"FLUSHDB"});
    }
    void sync() override {
        control_.command({"CONFIG", "SET", "appendfsync", "always"});
        control_.command({"SET", "__benchmark_sync", "1"});
        auto r = control_.command({"WAITAOF", "1", "0", "30000"});
        require(r->type == REDIS_REPLY_ARRAY && r->elements == 2 && r->element[0]->integer == 1,
                "Redis AOF sync barrier");
        if (config.mode != "sync")
            control_.command({"CONFIG", "SET", "appendfsync", "everysec"});
    }
};
std::unique_ptr<Engine> make_redis(const Config &c) {
    return std::make_unique<RedisEngine>(c);
}

struct CacheLocks {
    std::array<std::shared_mutex, 4096> locks;
};
class HybridConnection final : public Connection {
    std::unique_ptr<Connection> sql_;
    Wire cache_;
    std::shared_ptr<CacheLocks> locks_;
    static size_t stripe(uint64_t id) {
        return id % 4096;
    }
    static std::string cache_key(uint64_t id) {
        return "cache/" + key(id);
    }
    void invalidate(uint64_t id) {
        auto k = cache_key(id);
        cache_.command({"DEL", k});
    }
    void fill(const Profile &p, bool buffered = false) {
        auto k = cache_key(p.id), json = encode_json(p);
        std::vector<std::string_view> args{"SET", k, json, "EX", "300"};
        if (buffered)
            cache_.append(args);
        else
            cache_.command(args);
    }

  public:
    HybridConnection(std::unique_ptr<Connection> sql, std::shared_ptr<CacheLocks> locks)
        : sql_(std::move(sql)), locks_(std::move(locks)) {}
    Profile get(uint64_t id) override {
        std::shared_lock<std::shared_mutex> guard(locks_->locks[stripe(id)]);
        auto k = cache_key(id);
        auto r = cache_.command({"GET", k});
        if (r->type == REDIS_REPLY_STRING) {
            ++metrics.hits;
            return decode_json(bytes(r.get()));
        }
        require(r->type == REDIS_REPLY_NIL, "Cache GET result");
        ++metrics.misses;
        auto p = sql_->get(id);
        fill(p);
        return p;
    }
    void revision(uint64_t id, uint64_t value) override {
        std::unique_lock<std::shared_mutex> guard(locks_->locks[stripe(id)]);
        sql_->revision(id, value);
        invalidate(id);
    }
    void increment(uint64_t id) override {
        std::unique_lock<std::shared_mutex> guard(locks_->locks[stripe(id)]);
        sql_->increment(id);
        invalidate(id);
    }
    void transfer(uint64_t from, uint64_t to) override {
        auto a = stripe(from), b = stripe(to);
        if (a > b)
            std::swap(a, b);
        std::unique_lock<std::shared_mutex> first(locks_->locks[a]);
        std::unique_lock<std::shared_mutex> second;
        if (a != b)
            second = std::unique_lock<std::shared_mutex>(locks_->locks[b]);
        sql_->transfer(from, to);
        auto x = cache_key(from), y = cache_key(to);
        cache_.command({"DEL", x, y});
    }
    void replace(const Profile &p) override {
        std::unique_lock<std::shared_mutex> guard(locks_->locks[stripe(p.id)]);
        sql_->replace(p);
        invalidate(p.id);
    }
    void put(const Profile &p) override {
        sql_->put(p);
    }
    void seed_begin() override {
        sql_->seed_begin();
    }
    void seed_end() override {
        sql_->seed_end();
    }
    std::vector<Profile> many(uint64_t first) override {
        std::vector<size_t> stripes;
        for (unsigned i = 0; i < batch_size; ++i)
            stripes.push_back(stripe(first + i));
        std::sort(stripes.begin(), stripes.end());
        stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());
        std::vector<std::shared_lock<std::shared_mutex>> guards;
        for (auto s : stripes)
            guards.emplace_back(locks_->locks[s]);
        std::array<std::string, batch_size> names;
        std::vector<std::string_view> args{"MGET"};
        for (unsigned i = 0; i < batch_size; ++i) {
            names[i] = cache_key(first + i);
            args.push_back(names[i]);
        }
        auto r = cache_.command(args);
        require(r->type == REDIS_REPLY_ARRAY && r->elements == batch_size, "Cache MGET");
        std::vector<Profile> out(batch_size);
        std::vector<uint64_t> missing;
        for (unsigned i = 0; i < batch_size; ++i) {
            if (r->element[i]->type == REDIS_REPLY_NIL) {
                ++metrics.misses;
                missing.push_back(first + i);
            } else {
                ++metrics.hits;
                out[i] = decode_json(bytes(r->element[i]));
            }
        }
        if (!missing.empty()) {
            auto loaded = sql_->select(missing);
            for (auto &p : loaded) {
                fill(p, true);
                out[p.id - first] = std::move(p);
            }
            for (size_t i = 0; i < loaded.size(); ++i)
                cache_.next();
        }
        return out;
    }
};
class HybridEngine final : public Engine {
    std::unique_ptr<Engine> sql_;
    std::shared_ptr<CacheLocks> locks_ = std::make_shared<CacheLocks>();
    Wire cache_;

  public:
    explicit HybridEngine(const Config &c) : Engine(c), sql_(make_mysql(c)) {}
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<HybridConnection>(sql_->connect(), locks_);
    }
    void reset() override {
        sql_->reset();
        cache_.command({"FLUSHDB"});
    }
    void sync() override {
        sql_->sync();
    }
};
std::unique_ptr<Engine> make_hybrid(const Config &c) {
    return std::make_unique<HybridEngine>(c);
}
} // namespace business
