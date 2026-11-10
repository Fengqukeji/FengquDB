#include "common.hpp"
#include <mysql.h>

namespace final_bench {
class MyConnection final : public Connection {
    using Db = std::unique_ptr<MYSQL, decltype(&mysql_close)>;
    using Statement = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;
    Db db_{mysql_init(nullptr), mysql_close};
    Statement get_{nullptr, mysql_stmt_close}, put_{nullptr, mysql_stmt_close},
        erase_{nullptr, mysql_stmt_close}, patch_{nullptr, mysql_stmt_close},
        many_get_{nullptr, mysql_stmt_close}, many_put_{nullptr, mysql_stmt_close};
    size_t bytes_;
    std::string reuse_;
    unsigned long output_length_ = 0;
    bool output_null_ = false, output_error_ = false;
    void prepare(Statement &statement, const std::string &sql) {
        statement.reset(mysql_stmt_init(db_.get()));
        require(bool(statement), "MySQL statement allocation");
        require(mysql_stmt_prepare(statement.get(), sql.data(), sql.size()) == 0,
                "MySQL prepare: " + std::string(mysql_stmt_error(statement.get())));
    }
    static MYSQL_BIND blob(std::string_view value, unsigned long *length) {
        MYSQL_BIND out{};
        out.buffer_type = MYSQL_TYPE_BLOB;
        out.buffer = const_cast<char *>(value.data());
        out.buffer_length = value.size();
        *length = value.size();
        out.length = length;
        return out;
    }
    void execute(Statement &statement, MYSQL_BIND *input) {
        require(mysql_stmt_bind_param(statement.get(), input) == 0, "MySQL bind parameters");
        Clock::time_point deadline{};
        while (mysql_stmt_execute(statement.get()) != 0) {
            auto error = mysql_stmt_errno(statement.get());
            if (deadline == Clock::time_point{})
                deadline = Clock::now() + std::chrono::seconds(30);
            if ((error == 1213 || error == 1205) && Clock::now() < deadline) {
                ++retries_;
                require(mysql_stmt_reset(statement.get()) == 0, "MySQL retry reset");
                continue;
            }
            throw std::runtime_error("MySQL execute: " +
                                     std::string(mysql_stmt_error(statement.get())));
        }
    }
    void finish(Statement &statement) {
        mysql_stmt_free_result(statement.get());
    }
    bool fetch(Statement &statement, std::string &output) {
        MYSQL_BIND result{};
        output_length_ = 0;
        output_null_ = output_error_ = false;
        result.buffer_type = MYSQL_TYPE_BLOB;
        result.buffer = output.data();
        result.buffer_length = output.size();
        result.length = &output_length_;
        result.is_null = &output_null_;
        result.error = &output_error_;
        require(mysql_stmt_bind_result(statement.get(), &result) == 0, "MySQL bind result");
        int status = mysql_stmt_fetch(statement.get());
        if (status == MYSQL_NO_DATA)
            return false;
        require(status == 0 && !output_null_ && !output_error_ && output_length_ == bytes_,
                "MySQL result length/fetch");
        return true;
    }

  public:
    explicit MyConnection(const Config &config) : bytes_(config.payload), reuse_(bytes_, '\0') {
        require(bool(db_), "MySQL allocation");
        unsigned timeout = 30, connect_timeout = 5;
        mysql_ssl_mode ssl = SSL_MODE_DISABLED;
        bool public_key = true;
        mysql_options(db_.get(), MYSQL_OPT_SSL_MODE, &ssl);
        mysql_options(db_.get(), MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
        mysql_options(db_.get(), MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &public_key);
        mysql_options(db_.get(), MYSQL_OPT_READ_TIMEOUT, &timeout);
        mysql_options(db_.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout);
        require(mysql_real_connect(db_.get(), "127.0.0.1", "bench", "bench", "bench", 3306, nullptr,
                                   0) != nullptr,
                "MySQL connect: " + std::string(mysql_error(db_.get())));
        require(mysql_autocommit(db_.get(), 1) == 0, "MySQL autocommit");
        prepare(get_, "SELECT v FROM kv WHERE k=?");
        prepare(put_, "INSERT INTO kv(k,v) VALUES (?,?) AS new ON DUPLICATE KEY UPDATE v=new.v");
        prepare(erase_, "DELETE FROM kv WHERE k=?");
        prepare(patch_, "UPDATE kv SET v=INSERT(v,11,8,?) WHERE k=?");
        std::string input, rows;
        for (size_t i = 0; i < batch_size; ++i) {
            if (i) {
                input += ",";
                rows += ",";
            }
            input += "?";
            rows += "(?,?)";
        }
        prepare(many_get_, "SELECT v FROM kv WHERE k IN (" + input + ") ORDER BY k");
        prepare(many_put_,
                "INSERT INTO kv(k,v) VALUES " + rows + " AS new ON DUPLICATE KEY UPDATE v=new.v");
    }
    uint64_t read(uint64_t id, bool copy, bool missing) override {
        auto k = key(id);
        unsigned long length;
        auto input = blob(k, &length);
        execute(get_, &input);
        uint64_t score = 0;
        if (copy) {
            std::string owned(bytes_, '\0');
            auto found = fetch(get_, owned);
            require(found != missing, "MySQL row visibility");
            if (found)
                score = consume(owned.data(), owned.size(), id, bytes_);
            finish(get_);
        } else {
            auto found = fetch(get_, reuse_);
            require(found != missing, "MySQL row visibility");
            if (found)
                score = consume(reuse_.data(), reuse_.size(), id, bytes_);
            finish(get_);
        }
        return missing ? id : score;
    }
    std::optional<std::string> raw(uint64_t id) override {
        auto k = key(id);
        unsigned long length;
        auto input = blob(k, &length);
        execute(get_, &input);
        std::optional<std::string> value;
        std::string owned(bytes_, '\0');
        if (fetch(get_, owned))
            value = std::move(owned);
        finish(get_);
        return value;
    }
    void put(uint64_t id, std::string_view value) override {
        auto k = key(id);
        unsigned long lengths[2];
        MYSQL_BIND input[]{blob(k, &lengths[0]), blob(value, &lengths[1])};
        execute(put_, input);
        finish(put_);
    }
    void erase(uint64_t id) override {
        auto k = key(id);
        unsigned long length;
        auto input = blob(k, &length);
        execute(erase_, &input);
        require(mysql_stmt_affected_rows(erase_.get()) == 1, "MySQL delete missing");
        finish(erase_);
    }
    void patch(uint64_t id, const std::array<char, 8> &value) override {
        auto k = key(id);
        unsigned long lengths[2];
        MYSQL_BIND input[]{blob({value.data(), value.size()}, &lengths[0]), blob(k, &lengths[1])};
        execute(patch_, input);
        require(mysql_stmt_affected_rows(patch_.get()) == 1, "MySQL patch missing");
        finish(patch_);
    }
    void put_batch(const std::array<Row, batch_size> &rows) override {
        std::array<unsigned long, batch_size * 2> lengths{};
        std::array<MYSQL_BIND, batch_size * 2> input{};
        for (size_t i = 0; i < rows.size(); ++i) {
            input[2 * i] = blob(rows[i].key, &lengths[2 * i]);
            input[2 * i + 1] = blob(rows[i].value, &lengths[2 * i + 1]);
        }
        execute(many_put_, input.data());
        finish(many_put_);
    }
    std::array<std::string, batch_size> raw_batch(uint64_t first) override {
        std::array<std::string, batch_size> keys, values;
        std::array<unsigned long, batch_size> lengths{};
        std::array<MYSQL_BIND, batch_size> input{};
        for (size_t i = 0; i < keys.size(); ++i) {
            keys[i] = key(first + i);
            values[i].resize(bytes_);
            input[i] = blob(keys[i], &lengths[i]);
        }
        execute(many_get_, input.data());
        for (auto &value : values)
            require(fetch(many_get_, value), "MySQL batch missing");
        require(!fetch(many_get_, reuse_), "MySQL batch extra row");
        finish(many_get_);
        return values;
    }
    uint64_t read_batch(uint64_t first) override {
        auto values = raw_batch(first);
        uint64_t score = 0;
        for (size_t i = 0; i < values.size(); ++i)
            score += consume(values[i].data(), values[i].size(), first + i, bytes_);
        return score;
    }
    void scan(const Visitor &visitor) override {
        require(mysql_query(db_.get(), "SELECT k,v FROM kv ORDER BY k") == 0, "MySQL scan query");
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_use_result(db_.get()),
                                                                        mysql_free_result);
        require(bool(result), "MySQL scan result");
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result.get()))) {
            auto lengths = mysql_fetch_lengths(result.get());
            require(row[0] && row[1] && lengths, "MySQL scan null");
            visitor({row[0], lengths[0]}, {row[1], lengths[1]});
        }
        require(mysql_errno(db_.get()) == 0, "MySQL scan failure");
    }
};
class MyEngine final : public Engine {
    Config config_;
    std::string version_;
    using Db = std::unique_ptr<MYSQL, decltype(&mysql_close)>;
    Db control_{mysql_init(nullptr), mysql_close};
    void exec(const std::string &sql) {
        require(mysql_query(control_.get(), sql.c_str()) == 0,
                "MySQL control: " + std::string(mysql_error(control_.get())));
    }

  public:
    explicit MyEngine(const Config &config) : config_(config) {
        require(bool(control_), "MySQL control allocation");
        mysql_ssl_mode ssl = SSL_MODE_DISABLED;
        bool public_key = true;
        mysql_options(control_.get(), MYSQL_OPT_SSL_MODE, &ssl);
        mysql_options(control_.get(), MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &public_key);
        require(mysql_real_connect(control_.get(), "127.0.0.1", "bench", "bench", "bench", 3306,
                                   nullptr, 0) != nullptr,
                "MySQL control connect");
        version_ = mysql_get_server_info(control_.get());
        exec("CREATE TABLE IF NOT EXISTS kv(k VARBINARY(9) NOT NULL PRIMARY KEY,v LONGBLOB NOT "
             "NULL) ENGINE=InnoDB ROW_FORMAT=DYNAMIC");
        exec("SET GLOBAL innodb_flush_log_at_trx_commit=" +
             std::string(config.mode == "buffered" || config.action == "prepare" ? "2" : "1"));
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<MyConnection>(config_);
    }
    void sync() override {
        exec("FLUSH ENGINE LOGS");
    }
    void thread_cleanup() override {
        mysql_thread_end();
    }
    std::string version() const override {
        return version_;
    }
};
std::unique_ptr<Engine> make_mysql(const Config &config) {
    return std::make_unique<MyEngine>(config);
}
} // namespace final_bench
