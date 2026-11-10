#include "common.hpp"
#include <cerrno>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

namespace final_bench {
class FqEngine;
class FqConnection final : public Connection {
    fqdb_store *db_;
    size_t bytes_;
    template <class F> void transaction(F &&stage) {
        auto deadline = Clock::now() + std::chrono::seconds(30);
        for (;;) {
            fqdb_store_txn *raw = nullptr;
            fq_check(fqdb_store_txn_begin(db_, 0, &raw));
            std::unique_ptr<fqdb_store_txn, decltype(&fqdb_store_txn_close)> tx(
                raw, fqdb_store_txn_close);
            fq_check(stage(raw));
            uint64_t version = 0;
            auto status = fqdb_store_txn_commit(raw, &version);
            if (status == FQDB_CONFLICT && Clock::now() < deadline) {
                ++retries_;
                continue;
            }
            fq_check(status);
            return;
        }
    }

  public:
    FqConnection(fqdb_store *db, size_t bytes) : db_(db), bytes_(bytes) {}
    uint64_t read(uint64_t id, bool copy, bool missing) override {
        auto k = key(id);
        if (copy) {
            struct Buffer {
                fqdb_buffer value{};
                ~Buffer() {
                    fqdb_buffer_free(&value);
                }
            } owned;
            auto &out = owned.value;
            auto status = fqdb_store_get(db_, slice(k), &out);
            require(missing ? status == FQDB_NOT_FOUND : status == FQDB_OK, "Store read status");
            if (missing)
                return id;
            return consume(static_cast<const char *>(out.data), out.size, id, bytes_);
        }
        fqdb_store_value *out = nullptr;
        auto status = fqdb_store_get_view(db_, slice(k), &out);
        require(missing ? status == FQDB_NOT_FOUND : status == FQDB_OK, "Store view status");
        if (missing)
            return id;
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owned(
            out, fqdb_store_value_close);
        auto data = fqdb_store_value_data(out);
        return consume(static_cast<const char *>(data.data), data.size, id, bytes_);
    }
    void put(uint64_t id, std::string_view value) override {
        auto k = key(id);
        fqdb_store_atomic_info info{};
        fq_check(fqdb_store_put_if_changed(db_, slice(k), slice(value), 0, 0, &info));
        require(info.changed, "unexpected unchanged Store write");
    }
    void erase(uint64_t id) override {
        auto k = key(id);
        transaction([&](fqdb_store_txn *tx) { return fqdb_store_txn_delete(tx, slice(k)); });
    }
    void patch(uint64_t id, const std::array<char, 8> &value) override {
        auto k = key(id);
        transaction([&](fqdb_store_txn *tx) {
            return fqdb_store_txn_patch(tx, slice(k), 10, 8, {value.data(), value.size()});
        });
    }
    void put_batch(const std::array<Row, batch_size> &rows) override {
        std::array<fqdb_store_put_item, batch_size> items{};
        for (size_t i = 0; i < rows.size(); ++i)
            items[i] = {slice(rows[i].key), slice(rows[i].value), 0};
        transaction([&](fqdb_store_txn *tx) {
            return fqdb_store_txn_put_many(tx, items.data(), items.size());
        });
    }
    uint64_t read_batch(uint64_t first) override {
        std::array<std::string, batch_size> names;
        std::array<fqdb_slice, batch_size> keys;
        for (size_t i = 0; i < keys.size(); ++i) {
            names[i] = key(first + i);
            keys[i] = slice(names[i]);
        }
        fqdb_store_results *raw = nullptr;
        fq_check(fqdb_store_get_many(db_, keys.data(), keys.size(), &raw));
        std::unique_ptr<fqdb_store_results, decltype(&fqdb_store_results_close)> result(
            raw, fqdb_store_results_close);
        struct Copies {
            std::array<fqdb_buffer, batch_size> values{};
            ~Copies() {
                for (auto &v : values)
                    fqdb_buffer_free(&v);
            }
        } copies;
        auto rows = fqdb_store_results_data(raw);
        for (size_t i = 0; i < keys.size(); ++i) {
            require(rows[i].found, "Store batch missing");
            fq_check(fqdb_buffer_copy(rows[i].value, &copies.values[i]));
        }
        uint64_t score = 0;
        for (size_t i = 0; i < keys.size(); ++i)
            score += consume(static_cast<const char *>(copies.values[i].data),
                             copies.values[i].size, first + i, bytes_);
        return score;
    }
    void scan(const Visitor &visitor) override {
        fqdb_store_snapshot *snapshot = nullptr;
        fq_check(fqdb_store_snapshot_create(db_, &snapshot));
        std::unique_ptr<fqdb_store_snapshot, decltype(&fqdb_store_snapshot_close)> snap(
            snapshot, fqdb_store_snapshot_close);
        fqdb_store_cursor *cursor = nullptr;
        fq_check(fqdb_store_cursor_open(snapshot, {nullptr, 0}, {nullptr, 0}, 0, &cursor));
        std::unique_ptr<fqdb_store_cursor, decltype(&fqdb_store_cursor_close)> rows(
            cursor, fqdb_store_cursor_close);
        for (;;) {
            fqdb_store_row row{};
            auto status = fqdb_store_cursor_next(cursor, &row);
            if (status == FQDB_END)
                break;
            fq_check(status);
            visitor(
                {static_cast<const char *>(row.key.data), static_cast<size_t>(row.key.size)},
                {static_cast<const char *>(row.value.data), static_cast<size_t>(row.value.size)});
        }
    }
};
class FqEngine final : public Engine {
    fqdb_store *db_ = nullptr;
    Config config_;

  public:
    explicit FqEngine(const Config &config) : config_(config) {
        std::filesystem::create_directories(config.directory);
        fqdb_store_options settings{sizeof(settings),
                                    1,
                                    config.mode == "buffered" || config.action == "prepare" ? 1u
                                                                                            : 0u,
                                    1,
                                    2ull << 30,
                                    16ull << 20,
                                    32ull << 20,
                                    64ull << 20,
                                    256ull << 20,
                                    2ull << 30,
                                    1024};
        fq_check(fqdb_store_open_at(config.directory.c_str(), "main", &settings, &db_));
        if (config.action == "run") {
            fqdb_store_runtime_options runtime{
                sizeof(runtime), 1, 1, 1000, 256, config.fq_group_max, 0, 30, 64ull << 20, 64ull << 20, 1000, 0, 0};
            fq_check(fqdb_store_configure(db_, &runtime));
        }
    }
    ~FqEngine() {
        fqdb_store_close(db_);
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<FqConnection>(db_, config_.payload);
    }
    void sync() override {
        fq_check(fqdb_store_sync(db_));
        if (config_.action == "prepare")
            fq_check(fqdb_store_checkpoint(db_));
    }
    std::string metrics() override {
        fqdb_store_info info{};
        fqdb_store_runtime_info runtime{};
        fq_check(fqdb_store_get_info(db_, &info));
        fq_check(fqdb_store_runtime_info_get(db_, &runtime));
        uint64_t allocated = 0, logical = 0;
        for (const auto &entry : std::filesystem::directory_iterator(
                 std::filesystem::path(config_.directory) / "main")) {
            const auto name = entry.path().filename().string();
            if (name.size() < 5 || name.substr(name.size() - 4) != ".fdb")
                continue;
            struct stat info{};
            if (::stat(entry.path().c_str(), &info) != 0) {
                if (errno == ENOENT)
                    continue; // A retired generation can disappear during enumeration.
                throw std::runtime_error("Store file allocation query");
            }
            require(info.st_blocks >= 0 && info.st_size >= 0, "Store file allocation query");
            if (!S_ISREG(info.st_mode))
                continue;
            allocated += static_cast<uint64_t>(info.st_blocks) * 512;
            logical += static_cast<uint64_t>(info.st_size);
        }
        std::ostringstream out;
        out << "{\"version\":" << info.version << ",\"durable_version\":" << info.durable_version
            << ",\"file_bytes\":" << info.file_bytes << ",\"delta_bytes\":" << info.delta_bytes
            << ",\"allocated_data_bytes\":" << allocated << ",\"logical_data_bytes\":" << logical
            << ",\"cache_bytes\":" << info.cache_bytes << ",\"pinned_bytes\":" << info.pinned_bytes
            << ",\"configured_group_max\":" << config_.fq_group_max
              << ",\"group_flushes\":" << runtime.group_flushes
            << ",\"group_transactions\":" << runtime.group_transactions
            << ",\"background_compactions\":" << runtime.compactions
            << ",\"background_status\":" << runtime.last_status << "}";
        return out.str();
    }
    std::string version() const override {
        return "Store source ABI " + std::to_string(fqdb_abi_version());
    }
};
std::unique_ptr<Engine> make_fq(const Config &c) {
    return std::make_unique<FqEngine>(c);
}
} // namespace final_bench
