#include "common.hpp"
#include <sqlite3.h>
#include <filesystem>

namespace final_bench {
class SqliteConnection final : public Connection {
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *get_ = nullptr, *put_ = nullptr, *erase_ = nullptr, *patch_ = nullptr,
                 *many_get_ = nullptr, *many_put_ = nullptr;
    size_t bytes_;
    void exec(const std::string &sql) {
        char *error = nullptr;
        int status = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
        std::string message = error ? error : "";
        sqlite3_free(error);
        require(status == SQLITE_OK, "SQLite exec: " + message);
    }
    void prepare(sqlite3_stmt **statement, const std::string &sql) {
        require(sqlite3_prepare_v2(db_, sql.c_str(), -1, statement, nullptr) == SQLITE_OK,
                sqlite3_errmsg(db_));
    }
    void bind(sqlite3_stmt *statement, int index, std::string_view value) {
        require(sqlite3_bind_blob64(statement, index, value.data(), value.size(),
                                    SQLITE_TRANSIENT) == SQLITE_OK,
                "SQLite bind");
    }
    void reset(sqlite3_stmt *statement) {
        require(sqlite3_reset(statement) == SQLITE_OK, "SQLite reset");
        require(sqlite3_clear_bindings(statement) == SQLITE_OK, "SQLite clear");
    }
    void done(sqlite3_stmt *statement) {
        auto status = sqlite3_step(statement);
        Clock::time_point deadline{};
        // A fixed-count workload can outlast busy_timeout while one autocommit
        // writer is starved. SQLite permits retrying BUSY in autocommit mode;
        // keep parameters and measure the wait, without retrying LOCKED or an
        // explicit transaction. No clock read is added to successful writes.
        while (status == SQLITE_BUSY && sqlite3_get_autocommit(db_)) {
            if (deadline == Clock::time_point{})
                deadline = Clock::now() + std::chrono::seconds(120);
            if (Clock::now() >= deadline)
                break;
            ++retries_;
            status = sqlite3_step(statement);
        }
        if (status != SQLITE_DONE)
            throw std::runtime_error("SQLite step: " + std::string(sqlite3_errmsg(db_)));
        reset(statement);
    }
    void close() noexcept {
        for (auto pointer : {&get_, &put_, &erase_, &patch_, &many_get_, &many_put_}) {
            sqlite3_finalize(*pointer);
            *pointer = nullptr;
        }
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

  public:
    explicit SqliteConnection(const Config &config) : bytes_(config.payload) {
        std::unique_ptr<SqliteConnection, void (*)(SqliteConnection *)> rollback(
            this, [](SqliteConnection *self) { self->close(); });
        require(sqlite3_open_v2((config.directory + "/main.db").c_str(), &db_,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                nullptr) == SQLITE_OK,
                "SQLite open");
        sqlite3_busy_timeout(db_, 30000);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=" +
             std::string(config.mode == "buffered" || config.action == "prepare" ? "NORMAL"
                                                                                 : "FULL"));
        {
            sqlite3_stmt *raw = nullptr;
            prepare(&raw, "PRAGMA synchronous");
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> check(raw, sqlite3_finalize);
            require(sqlite3_step(raw) == SQLITE_ROW &&
                        sqlite3_column_int(raw, 0) ==
                            (config.mode == "buffered" || config.action == "prepare" ? 1 : 2),
                    "SQLite synchronous setting");
        }
        {
            sqlite3_stmt *raw = nullptr;
            prepare(&raw, "PRAGMA journal_mode");
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> check(raw, sqlite3_finalize);
            require(sqlite3_step(raw) == SQLITE_ROW &&
                        std::string_view(
                            reinterpret_cast<const char *>(sqlite3_column_text(raw, 0))) == "wal",
                    "SQLite WAL setting");
        }
        exec("PRAGMA wal_autocheckpoint=1000");
        exec("PRAGMA mmap_size=2147483648");
        exec("PRAGMA temp_store=MEMORY");
        exec("PRAGMA cache_size=-" +
             std::to_string(std::max<size_t>(1024, (2ull << 20) / config.threads)));
        exec("CREATE TABLE IF NOT EXISTS kv(k BLOB NOT NULL PRIMARY KEY,v BLOB NOT NULL) WITHOUT "
             "ROWID");
        prepare(&get_, "SELECT v FROM kv WHERE k=?");
        prepare(&put_,
                "INSERT INTO kv(k,v) VALUES (?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v");
        prepare(&erase_, "DELETE FROM kv WHERE k=?");
        prepare(&patch_, "UPDATE kv SET v=CAST(substr(v,1,10)||?||substr(v,19) AS BLOB) WHERE k=?");
        std::string input, rows;
        for (size_t i = 0; i < batch_size; ++i) {
            if (i) {
                input += ",";
                rows += ",";
            }
            input += "?";
            rows += "(?,?)";
        }
        prepare(&many_get_, "SELECT v FROM kv WHERE k IN (" + input + ") ORDER BY k");
        prepare(&many_put_, "INSERT INTO kv(k,v) VALUES " + rows +
                                " ON CONFLICT(k) DO UPDATE SET v=excluded.v");
        rollback.release();
    }
    ~SqliteConnection() {
        close();
    }
    uint64_t read(uint64_t id, bool copy, bool missing) override {
        auto k = key(id);
        bind(get_, 1, k);
        auto status = sqlite3_step(get_);
        if (missing) {
            require(status == SQLITE_DONE, "SQLite expected miss");
            reset(get_);
            return id;
        }
        require(status == SQLITE_ROW, "SQLite expected row");
        auto data = static_cast<const char *>(sqlite3_column_blob(get_, 0));
        auto size = static_cast<size_t>(sqlite3_column_bytes(get_, 0));
        uint64_t score;
        if (copy) {
            std::string value(data, size);
            score = consume(value.data(), value.size(), id, bytes_);
        } else
            score = consume(data, size, id, bytes_);
        reset(get_);
        return score;
    }
    void put(uint64_t id, std::string_view value) override {
        auto k = key(id);
        bind(put_, 1, k);
        bind(put_, 2, value);
        done(put_);
    }
    void erase(uint64_t id) override {
        auto k = key(id);
        bind(erase_, 1, k);
        done(erase_);
        require(sqlite3_changes(db_) == 1, "SQLite delete missing");
    }
    void patch(uint64_t id, const std::array<char, 8> &value) override {
        auto k = key(id);
        bind(patch_, 1, {value.data(), value.size()});
        bind(patch_, 2, k);
        done(patch_);
        require(sqlite3_changes(db_) == 1, "SQLite patch missing");
    }
    void put_batch(const std::array<Row, batch_size> &rows) override {
        for (size_t i = 0; i < rows.size(); ++i) {
            bind(many_put_, static_cast<int>(2 * i + 1), rows[i].key);
            bind(many_put_, static_cast<int>(2 * i + 2), rows[i].value);
        }
        done(many_put_);
    }
    uint64_t read_batch(uint64_t first) override {
        for (size_t i = 0; i < batch_size; ++i) {
            auto k = key(first + i);
            bind(many_get_, static_cast<int>(i + 1), k);
        }
        std::array<std::string, batch_size> values;
        size_t count = 0;
        int status;
        while ((status = sqlite3_step(many_get_)) == SQLITE_ROW) {
            require(count < batch_size, "SQLite batch count");
            values[count++].assign(static_cast<const char *>(sqlite3_column_blob(many_get_, 0)),
                                   sqlite3_column_bytes(many_get_, 0));
        }
        require(status == SQLITE_DONE && count == batch_size, "SQLite batch missing");
        reset(many_get_);
        uint64_t score = 0;
        for (size_t i = 0; i < values.size(); ++i)
            score += consume(values[i].data(), values[i].size(), first + i, bytes_);
        return score;
    }
    void scan(const Visitor &visitor) override {
        sqlite3_stmt *raw = nullptr;
        prepare(&raw, "SELECT k,v FROM kv ORDER BY k");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> rows(raw, sqlite3_finalize);
        int status;
        while ((status = sqlite3_step(raw)) == SQLITE_ROW)
            visitor({static_cast<const char *>(sqlite3_column_blob(raw, 0)),
                     static_cast<size_t>(sqlite3_column_bytes(raw, 0))},
                    {static_cast<const char *>(sqlite3_column_blob(raw, 1)),
                     static_cast<size_t>(sqlite3_column_bytes(raw, 1))});
        require(status == SQLITE_DONE, "SQLite scan failed");
    }
    void sync() {
        int log = 0, done = 0;
        require(sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &log, &done) ==
                    SQLITE_OK,
                "SQLite final checkpoint busy/error");
    }
};
class SqliteEngine final : public Engine {
    Config config_;

  public:
    explicit SqliteEngine(const Config &config) : config_(config) {
        std::filesystem::create_directories(config.directory);
        SqliteConnection setup(config);
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<SqliteConnection>(config_);
    }
    void sync() override {
        SqliteConnection connection(config_);
        connection.sync();
    }
    std::string version() const override {
        return sqlite3_libversion();
    }
};
std::unique_ptr<Engine> make_sqlite(const Config &config) {
    return std::make_unique<SqliteEngine>(config);
}
} // namespace final_bench
