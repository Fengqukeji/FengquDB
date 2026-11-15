#include "common.hpp"
#include <mysql.h>
namespace business {
struct SqlFailure : std::runtime_error {
    unsigned code;
    SqlFailure(unsigned c, std::string m) : std::runtime_error(std::move(m)), code(c) {}
};
class MySQLConnection final : public Connection {
    std::unique_ptr<MYSQL, decltype(&mysql_close)> db_{mysql_init(nullptr), mysql_close};
    using Stmt = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;
    Stmt get_{nullptr, mysql_stmt_close}, replace_{nullptr, mysql_stmt_close},
        project_{nullptr, mysql_stmt_close}, put_{nullptr, mysql_stmt_close},
        rev_{nullptr, mysql_stmt_close}, inc_{nullptr, mysql_stmt_close},
        many_{nullptr, mysql_stmt_close}, lock_{nullptr, mysql_stmt_close},
        move_{nullptr, mysql_stmt_close}, erase_{nullptr, mysql_stmt_close};
    std::array<uint64_t, 10> nums_{};
    std::array<std::vector<char>, 3> strings_;
    std::array<unsigned long, 10> lengths_{};
    std::array<bool, 10> nulls_{}, errors_{};
    static MYSQL_BIND number(uint64_t *p, bool sign = false) {
        MYSQL_BIND b{};
        b.buffer_type = MYSQL_TYPE_LONGLONG;
        b.buffer = p;
        b.is_unsigned = !sign;
        return b;
    }
    static MYSQL_BIND text(const std::string &s, unsigned long *n) {
        MYSQL_BIND b{};
        b.buffer_type = MYSQL_TYPE_STRING;
        b.buffer = const_cast<char *>(s.data());
        b.buffer_length = s.size();
        *n = s.size();
        b.length = n;
        return b;
    }
    void prepare(Stmt &s, const std::string &sql) {
        s.reset(mysql_stmt_init(db_.get()));
        require(bool(s), "MySQL statement allocation");
        require(mysql_stmt_prepare(s.get(), sql.data(), sql.size()) == 0,
                "MySQL prepare: " + std::string(mysql_stmt_error(s.get())));
    }
    void execute(Stmt &s, MYSQL_BIND *args) {
        require(mysql_stmt_bind_param(s.get(), args) == 0, "MySQL bind");
        if (mysql_stmt_execute(s.get()) != 0)
            throw SqlFailure(mysql_stmt_errno(s.get()), mysql_stmt_error(s.get()));
    }
    int fetch(Stmt &s) {
        auto rc = mysql_stmt_fetch(s.get());
        if (rc != 0 && rc != MYSQL_NO_DATA)
            throw SqlFailure(mysql_stmt_errno(s.get()),
                             "MySQL fetch: " + std::string(mysql_stmt_error(s.get())));
        return rc;
    }
    void bind_full(Stmt &s) {
        std::array<MYSQL_BIND, 10> out{};
        for (unsigned i = 0; i < 10; ++i) {
            if (i >= 5 && i <= 7) {
                out[i].buffer_type = MYSQL_TYPE_STRING;
                out[i].buffer = strings_[i - 5].data();
                out[i].buffer_length = strings_[i - 5].size();
            } else
                out[i] = number(&nums_[i], i == 2);
            out[i].length = &lengths_[i];
            out[i].is_null = &nulls_[i];
            out[i].error = &errors_[i];
        }
        require(mysql_stmt_bind_result(s.get(), out.data()) == 0, "MySQL output bind");
    }
    Profile full() const {
        for (unsigned i = 0; i < 10; ++i)
            require(!nulls_[i] && !errors_[i], "MySQL null/truncated field");
        Profile p;
        p.id = nums_[0];
        p.revision = nums_[1];
        p.credit = static_cast<int64_t>(nums_[2]);
        p.level = nums_[3];
        p.status = nums_[4];
        p.name.assign(strings_[0].data(), lengths_[5]);
        p.email.assign(strings_[1].data(), lengths_[6]);
        p.bio.assign(strings_[2].data(), lengths_[7]);
        p.region = nums_[8];
        p.last_login = nums_[9];
        return p;
    }

  public:
    explicit MySQLConnection(const Config &c) {
        require(bool(db_), "MySQL init");
        mysql_ssl_mode ssl = SSL_MODE_DISABLED;
        unsigned timeout = 30;
        bool public_key = true;
        mysql_options(db_.get(), MYSQL_OPT_SSL_MODE, &ssl);
        mysql_options(db_.get(), MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &public_key);
        mysql_options(db_.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_options(db_.get(), MYSQL_OPT_READ_TIMEOUT, &timeout);
        mysql_options(db_.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout);
        require(mysql_real_connect(db_.get(), "127.0.0.1", "bench", "bench", "bench", 3306, nullptr,
                                   0) != nullptr,
                "MySQL connect: " + std::string(mysql_error(db_.get())));
        require(mysql_set_character_set(db_.get(), "utf8mb4") == 0, "MySQL charset");
        strings_[0].resize(256);
        strings_[1].resize(256);
        strings_[2].resize(c.body + 64);
        std::string fields = "id,revision,credit,level,status,name,email,bio,region,last_login";
        prepare(get_, "SELECT " + fields + " FROM profiles WHERE id=?");
        prepare(project_, "SELECT revision,credit FROM profiles WHERE id=?");
        prepare(erase_, "DELETE FROM profiles WHERE id=?");
        prepare(replace_, "UPDATE profiles SET "
                          "revision=?,credit=?,level=?,status=?,name=?,email=?,bio=?,region=?,last_"
                          "login=? WHERE id=?");
        prepare(put_, "INSERT INTO profiles(" + fields + ") VALUES(?,?,?,?,?,?,?,?,?,?)");
        prepare(rev_, "UPDATE profiles SET revision=? WHERE id=?");
        prepare(inc_, "UPDATE profiles SET credit=credit+1 WHERE id=?");
        std::string marks;
        for (unsigned i = 0; i < batch_size; ++i)
            marks += (i ? ",?" : "?");
        prepare(many_,
                "SELECT " + fields + " FROM profiles WHERE id IN (" + marks + ") ORDER BY id");
        prepare(lock_, "SELECT id,credit FROM profiles WHERE id IN (?,?) ORDER BY id FOR UPDATE");
        prepare(move_, "UPDATE profiles SET credit=CASE WHEN id=? THEN credit-1 ELSE credit+1 END "
                       "WHERE id IN (?,?)");
    }
    void query(const std::string &sql) {
        if (mysql_real_query(db_.get(), sql.data(), sql.size()))
            throw SqlFailure(mysql_errno(db_.get()), mysql_error(db_.get()));
    }
    void erase(uint64_t id) override {
        auto b = number(&id);
        execute(erase_, &b);
        require(mysql_stmt_affected_rows(erase_.get()) == 1, "MySQL delete did not remove one profile");
    }
    uint64_t count() override {
        query("SELECT COUNT(*) FROM profiles");
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(db_.get()), mysql_free_result);
        require(bool(result), "MySQL count result");
        auto row = mysql_fetch_row(result.get());
        require(row && row[0], "MySQL count row");
        return std::stoull(row[0]);
    }
    Profile get(uint64_t id) override {
        auto b = number(&id);
        execute(get_, &b);
        bind_full(get_);
        require(fetch(get_) == 0, "MySQL missing");
        auto p = full();
        mysql_stmt_free_result(get_.get());
        return p;
    }
    Projection project(uint64_t id) override {
        auto b = number(&id);
        execute(project_, &b);
        uint64_t rev = 0, credit = 0;
        std::array<MYSQL_BIND, 2> out{number(&rev), number(&credit, true)};
        require(mysql_stmt_bind_result(project_.get(), out.data()) == 0, "MySQL projection bind");
        require(fetch(project_) == 0, "MySQL projection missing");
        mysql_stmt_free_result(project_.get());
        return {rev, static_cast<int64_t>(credit)};
    }
    void revision(uint64_t id, uint64_t v) override {
        std::array<MYSQL_BIND, 2> b{number(&v), number(&id)};
        execute(rev_, b.data());
        require(mysql_stmt_affected_rows(rev_.get()) == 1, "MySQL revision unchanged");
    }
    void increment(uint64_t id) override {
        auto b = number(&id);
        execute(inc_, &b);
        require(mysql_stmt_affected_rows(inc_.get()) == 1, "MySQL increment missing");
    }
    void transfer(uint64_t from, uint64_t to) override {
        auto until = Clock::now() + std::chrono::seconds(30);
        for (;;) {
            query("START TRANSACTION");
            try {
                uint64_t low = std::min(from, to), high = std::max(from, to);
                std::array<MYSQL_BIND, 2> keys{number(&low), number(&high)};
                execute(lock_, keys.data());
                uint64_t id = 0, credit = 0;
                std::array<MYSQL_BIND, 2> out{number(&id), number(&credit, true)};
                require(mysql_stmt_bind_result(lock_.get(), out.data()) == 0, "MySQL lock bind");
                unsigned count = 0;
                bool sufficient = false;
                while (fetch(lock_) == 0) {
                    ++count;
                    if (id == from)
                        sufficient = static_cast<int64_t>(credit) > 0;
                }
                mysql_stmt_free_result(lock_.get());
                require(count == 2 && sufficient, "MySQL transfer missing/insufficient");
                std::array<MYSQL_BIND, 3> values{number(&from), number(&low), number(&high)};
                execute(move_, values.data());
                require(mysql_stmt_affected_rows(move_.get()) == 2, "MySQL transfer incomplete");
                query("COMMIT");
                return;
            } catch (const SqlFailure &e) {
                mysql_stmt_free_result(lock_.get());
                query("ROLLBACK");
                if ((e.code == 1213 || e.code == 1205) && Clock::now() < until) {
                    ++metrics.retries;
                    continue;
                }
                throw;
            } catch (...) {
                mysql_stmt_free_result(lock_.get());
                query("ROLLBACK");
                throw;
            }
        }
    }
    void put(const Profile &p) override {
        std::array<uint64_t, 10> values{
            p.id,     p.revision,  static_cast<uint64_t>(p.credit), p.level, p.status, 0, 0, 0,
            p.region, p.last_login};
        std::array<MYSQL_BIND, 10> b{};
        std::array<unsigned long, 3> lengths{};
        for (unsigned i = 0; i < 10; ++i)
            b[i] = number(&values[i], i == 2);
        b[5] = text(p.name, &lengths[0]);
        b[6] = text(p.email, &lengths[1]);
        b[7] = text(p.bio, &lengths[2]);
        execute(put_, b.data());
    }
    void replace(const Profile &p) override {
        std::array<uint64_t, 10> values{p.revision,
                                        static_cast<uint64_t>(p.credit),
                                        p.level,
                                        p.status,
                                        0,
                                        0,
                                        0,
                                        p.region,
                                        p.last_login,
                                        p.id};
        std::array<MYSQL_BIND, 10> b{};
        std::array<unsigned long, 3> lengths{};
        for (unsigned i = 0; i < 10; ++i)
            b[i] = number(&values[i], i == 1);
        b[4] = text(p.name, &lengths[0]);
        b[5] = text(p.email, &lengths[1]);
        b[6] = text(p.bio, &lengths[2]);
        execute(replace_, b.data());
        require(mysql_stmt_affected_rows(replace_.get()) == 1, "MySQL full save missing");
    }
    std::vector<Profile> select(const std::vector<uint64_t> &wanted) override {
        if (wanted.empty())
            return {};
        require(wanted.size() <= batch_size, "MySQL select count");
        std::array<uint64_t, batch_size> ids{};
        std::array<MYSQL_BIND, batch_size> b{};
        for (unsigned i = 0; i < batch_size; ++i) {
            ids[i] = wanted[std::min<size_t>(i, wanted.size() - 1)];
            b[i] = number(&ids[i]);
        }
        execute(many_, b.data());
        bind_full(many_);
        std::vector<Profile> out;
        out.reserve(wanted.size());
        while (fetch(many_) == 0)
            out.push_back(full());
        mysql_stmt_free_result(many_.get());
        require(out.size() == wanted.size(), "MySQL batch incomplete");
        return out;
    }
    std::vector<Profile> many(uint64_t first) override {
        std::vector<uint64_t> ids(batch_size);
        std::iota(ids.begin(), ids.end(), first);
        return select(ids);
    }
    void seed_begin() override {
        query("START TRANSACTION");
    }
    void seed_end() override {
        query("COMMIT");
    }
};
class MySQLEngine final : public Engine {
    using Db = std::unique_ptr<MYSQL, decltype(&mysql_close)>;
    Db control_{mysql_init(nullptr), mysql_close};
    void query(const std::string &q) {
        require(mysql_real_query(control_.get(), q.data(), q.size()) == 0,
                "MySQL control: " + std::string(mysql_error(control_.get())));
    }

  public:
    explicit MySQLEngine(const Config &c) : Engine(c) {
        require(bool(control_), "MySQL control init");
        require(mysql_real_connect(control_.get(), "localhost", "root", nullptr, "bench", 0,
                                   "/data/m.sock", 0) != nullptr,
                "MySQL control connect");
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<MySQLConnection>(config);
    }
    void reset() override {
        query("DROP TABLE IF EXISTS profiles");
        query("CREATE TABLE profiles(id BIGINT UNSIGNED PRIMARY KEY,revision BIGINT UNSIGNED NOT "
              "NULL,credit BIGINT NOT NULL,level INT UNSIGNED NOT NULL,status INT UNSIGNED NOT "
              "NULL,name VARCHAR(256) NOT NULL,email VARCHAR(256) NOT NULL,bio MEDIUMTEXT NOT "
              "NULL,region INT UNSIGNED NOT NULL,last_login BIGINT UNSIGNED NOT NULL) "
              "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }
    void sync() override {
        query("FLUSH ENGINE LOGS");
    }
};
std::unique_ptr<Engine> make_mysql(const Config &c) {
    return std::make_unique<MySQLEngine>(c);
}
} // namespace business
