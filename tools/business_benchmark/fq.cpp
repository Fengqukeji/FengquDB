#include "common.hpp"
namespace business {
class FqConnection final : public Connection {
    fqdb_store *db_;
    fqdb_store_txn *seed_ = nullptr;

  public:
    explicit FqConnection(fqdb_store *db) : db_(db) {}
    ~FqConnection() {
        if (seed_)
            fqdb_store_txn_close(seed_);
    }
    Profile get(uint64_t id) override {
        auto name = key(id);
        fqdb_store_shared_value out{};
        fq(fqdb_store_get_shared(db_, slice(name), &out));
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owner(
            out.owner, fqdb_store_value_close);
        return decode_pb({static_cast<const char *>(out.row.value.data),
                          static_cast<size_t>(out.row.value.size)});
    }
    Projection project(uint64_t id) override {
        auto name = key(id);
        fqdb_store_shared_value out{};
        fq(fqdb_store_get_shared(db_, slice(name), &out));
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owner(
            out.owner, fqdb_store_value_close);
        return project_pb({static_cast<const char *>(out.row.value.data),
                           static_cast<size_t>(out.row.value.size)});
    }
    void revision(uint64_t id, uint64_t value) override {
        auto name = key(id);
        auto bytes = fixed64(value);
        fqdb_store_atomic_info out{};
        fq(fqdb_store_patch(db_, slice(name), 10, 8, {bytes.data(), bytes.size()}, &out));
    }
    void increment(uint64_t id) override {
        auto name = key(id);
        fqdb_store_i64_field_update item{slice(name), 19, 1, 0, INT64_MAX};
        fqdb_store_atomic_info out{};
        fq(fqdb_store_add_i64_fields(db_, &item, 1, 0, &out));
    }
    void transfer(uint64_t from, uint64_t to) override {
        auto a = key(from), b = key(to);
        fqdb_store_i64_field_update items[]{{slice(a), 19, -1, 0, INT64_MAX},
                                            {slice(b), 19, 1, 0, INT64_MAX}};
        fqdb_store_atomic_info out{};
        fq(fqdb_store_add_i64_fields(db_, items, 2, 0, &out));
    }
    void put(const Profile &p) override {
        auto name = key(p.id), bytes = encode_pb(p);
        if (seed_)
            fq(fqdb_store_txn_put(seed_, slice(name), slice(bytes), 0));
        else {
            fqdb_store_atomic_info out{};
            fq(fqdb_store_put(db_, slice(name), slice(bytes), 0, &out));
        }
    }
    std::vector<Profile> many(uint64_t first) override {
        std::array<std::string, batch_size> names;
        std::array<fqdb_slice, batch_size> keys;
        for (size_t i = 0; i < batch_size; ++i) {
            names[i] = key(first + i);
            keys[i] = slice(names[i]);
        }
        fqdb_store_results *raw = nullptr;
        fq(fqdb_store_get_many(db_, keys.data(), batch_size, &raw));
        std::unique_ptr<fqdb_store_results, decltype(&fqdb_store_results_close)> result(
            raw, fqdb_store_results_close);
        const auto rows = fqdb_store_results_data(raw);
        std::vector<Profile> out;
        out.reserve(batch_size);
        for (size_t i = 0; i < batch_size; ++i) {
            require(rows[i].found, "FQ batch missing");
            out.push_back(decode_pb({static_cast<const char *>(rows[i].value.data),
                                     static_cast<size_t>(rows[i].value.size)}));
        }
        return out;
    }
    void seed_begin() override {
        fq(fqdb_store_txn_begin(db_, 0, &seed_));
    }
    void seed_end() override {
        uint64_t version = 0;
        fq(fqdb_store_txn_commit(seed_, &version));
        fqdb_store_txn_close(seed_);
        seed_ = nullptr;
    }
};
class FqEngine final : public Engine {
    fqdb_store *db_ = nullptr;

  public:
    explicit FqEngine(const Config &c) : Engine(c) {
        std::filesystem::create_directories(c.data);
        fqdb_store_options o{sizeof(o),
                             1,
                             c.action != "prepare" && c.mode != "buffered" ? 0u : 1u,
                             1,
                             512ull << 20,
                             16ull << 20,
                             32ull << 20,
                             64ull << 20,
                             256ull << 20,
                             512ull << 20,
                             1024};
        fq(fqdb_store_open_at(c.data.c_str(), "main", &o, &db_));
        if (c.action == "run") {
            // Keep one synchronous call in flight per producer. Performance
            // mode acknowledges memory visibility; the driver times the final sync.
            fqdb_store_write_options write{sizeof(write), 1,
                                           c.mode == "performance" ? FQDB_WRITE_PERFORMANCE
                                                                   : FQDB_WRITE_RELIABLE,
                                           c.period_ms, c.threshold_bytes};
            fq(fqdb_store_write_configure(db_, &write));
            fqdb_store_runtime_options rt{sizeof(rt), 1,           1,           200,  256, 128, 0,
                                          30,         32ull << 20, 32ull << 20, 1000, 0,   0};
            fq(fqdb_store_configure(db_, &rt));
        }
    }
    ~FqEngine() {
        fqdb_store_close(db_);
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<FqConnection>(db_);
    }
    void reset() override {
        fqdb_store_info i{};
        fq(fqdb_store_get_info(db_, &i));
        require(i.indexed_keys == 0, "FQ seed must use fresh owned directory");
    }
    void sync() override {
        fq(fqdb_store_sync(db_));
        if (config.action == "prepare")
            fq(fqdb_store_checkpoint(db_));
    }
    std::string info() override {
        fqdb_store_info i{};
        fq(fqdb_store_get_info(db_, &i));
        fqdb_store_runtime_info runtime{};
        fq(fqdb_store_runtime_info_get(db_, &runtime));
        auto extra = ",\"group_flushes\":" + std::to_string(runtime.group_flushes) +
                     ",\"group_transactions\":" + std::to_string(runtime.group_transactions) +
                     ",\"version\":" + std::to_string(i.version) +
                     ",\"durable_version\":" + std::to_string(i.durable_version);
        return "{\"file_bytes\":" + std::to_string(i.file_bytes) +
               ",\"cache_bytes\":" + std::to_string(i.cache_bytes) +
               ",\"pinned_bytes\":" + std::to_string(i.pinned_bytes) + extra + "}";
    }
};
std::unique_ptr<Engine> make_fq(const Config &c) {
    return std::make_unique<FqEngine>(c);
}
} // namespace business
