#include "common.hpp"
#include <sqlite3.h>
namespace business {
class SQLiteConnection final : public Connection {
    sqlite3 *db_ = nullptr;
    bool rows_, pb_, jsonb_;
    std::vector<sqlite3_stmt *> statements_;
    sqlite3_stmt *get_ = nullptr, *project_ = nullptr, *put_ = nullptr, *rev_ = nullptr,
                 *shift_ = nullptr, *many_ = nullptr, *replace_ = nullptr;
    void exec(const std::string &sql) {
        char *e = nullptr;
        auto rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &e);
        std::string error = e ? e : "";
        sqlite3_free(e);
        require(rc == SQLITE_OK, "SQLite exec: " + error);
    }
    sqlite3_stmt *prepare(const std::string &sql) {
        sqlite3_stmt *s = nullptr;
        require(sqlite3_prepare_v2(db_, sql.c_str(), -1, &s, nullptr) == SQLITE_OK,
                sqlite3_errmsg(db_));
        statements_.push_back(s);
        return s;
    }
    void reset(sqlite3_stmt *s) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    void integer(sqlite3_stmt *s, int i, int64_t v) {
        require(sqlite3_bind_int64(s, i, v) == SQLITE_OK, "SQLite integer bind");
    }
    void text(sqlite3_stmt *s, int i, const std::string &v, bool blob = false) {
        auto rc = blob ? sqlite3_bind_blob64(s, i, v.data(), v.size(), SQLITE_STATIC)
                       : sqlite3_bind_text64(s, i, v.data(), v.size(), SQLITE_STATIC, SQLITE_UTF8);
        require(rc == SQLITE_OK, "SQLite string bind");
    }
    void done(sqlite3_stmt *s) {
        auto rc = sqlite3_step(s);
        if (rc != SQLITE_DONE) {
            reset(s);
            throw std::runtime_error("SQLite step: " + std::string(sqlite3_errmsg(db_)));
        }
        reset(s);
    }
    static std::string column(sqlite3_stmt *s, int at) {
        auto p = static_cast<const char *>(sqlite3_column_blob(s, at));
        auto n = sqlite3_column_bytes(s, at);
        return n ? std::string(p, n) : std::string{};
    }
    static std::string_view column_view(sqlite3_stmt *s, int at) {
        auto p = static_cast<const char *>(sqlite3_column_blob(s, at));
        auto n = sqlite3_column_bytes(s, at);
        return n ? std::string_view(p, static_cast<size_t>(n)) : std::string_view{};
    }
    Profile row(sqlite3_stmt *s) {
        if (!rows_) {
            auto value = column_view(s, 0);
            return pb_ ? decode_pb(value) : decode_json(value);
        }
        Profile p;
        p.id = sqlite3_column_int64(s, 0);
        p.revision = sqlite3_column_int64(s, 1);
        p.credit = sqlite3_column_int64(s, 2);
        p.level = sqlite3_column_int64(s, 3);
        p.status = sqlite3_column_int64(s, 4);
        p.name = column(s, 5);
        p.email = column(s, 6);
        p.bio = column(s, 7);
        p.region = sqlite3_column_int64(s, 8);
        p.last_login = sqlite3_column_int64(s, 9);
        return p;
    }
    void patch_blob(uint64_t id, int offset, uint64_t value, bool add = false) {
        sqlite3_blob *raw = nullptr;
        require(sqlite3_blob_open(db_, "main", "profiles", "doc", id, 1, &raw) == SQLITE_OK,
                "SQLite blob open: " + std::string(sqlite3_errmsg(db_)));
        std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> blob(raw, sqlite3_blob_close);
        if (add) {
            std::array<unsigned char, 8> bytes{};
            require(sqlite3_blob_read(raw, bytes.data(), 8, offset) == SQLITE_OK,
                    "SQLite blob read");
            uint64_t old = 0;
            for (unsigned i = 0; i < 8; ++i)
                old |= uint64_t(bytes[i]) << (8 * i);
            value += old;
        }
        auto bytes = fixed64(value);
        require(sqlite3_blob_write(raw, bytes.data(), 8, offset) == SQLITE_OK, "SQLite blob write");
        require(sqlite3_blob_close(blob.release()) == SQLITE_OK, "SQLite blob commit");
    }
    void shift(uint64_t id, int64_t delta) {
        if (pb_) {
            patch_blob(id, 19, static_cast<uint64_t>(delta), true);
            return;
        }
        integer(shift_, 1, delta);
        integer(shift_, 2, id);
        done(shift_);
    }

  public:
    explicit SQLiteConnection(const Config &c)
        : rows_(c.engine == "sqlite-rows"), pb_(c.engine == "sqlite-pb"),
          jsonb_(c.engine == "sqlite-jsonb") {
        require(sqlite3_open_v2((c.data + "/main.db").c_str(), &db_,
                                SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                                nullptr) == SQLITE_OK,
                "SQLite open");
        sqlite3_busy_timeout(db_, 30000);
        try {
            exec(std::string("PRAGMA synchronous=") +
                 (c.mode == "sync" && c.action != "prepare" ? "FULL" : "NORMAL"));
            exec("PRAGMA cache_size=-262144");
            exec("PRAGMA mmap_size=268435456");
            std::string select =
                rows_    ? "id,revision,credit,level,status,name,email,bio,region,last_login"
                : jsonb_ ? "json(doc)"
                         : "doc";
            get_ = prepare("SELECT " + select + " FROM profiles WHERE id=?");
            project_ = prepare(
                rows_ ? "SELECT revision,credit FROM profiles WHERE id=?"
                : pb_ ? "SELECT doc FROM profiles WHERE id=?"
                      : "SELECT json_extract(doc,'$.revision'),json_extract(doc,'$.credit') FROM "
                        "profiles WHERE id=?");
            if (rows_) {
                put_ = prepare("INSERT INTO profiles VALUES(?,?,?,?,?,?,?,?,?,?)");
                rev_ = prepare("UPDATE profiles SET revision=? WHERE id=?");
                shift_ = prepare("UPDATE profiles SET credit=credit+? WHERE id=?");
            } else {
                put_ = prepare("INSERT INTO profiles(id,doc) VALUES(?," +
                               std::string(jsonb_ ? "jsonb(?)" : "?") + ")");
                if (!pb_) {
                    auto fn = std::string(jsonb_ ? "jsonb_set" : "json_set");
                    rev_ = prepare("UPDATE profiles SET doc=" + fn +
                                   "(doc,'$.revision',?) WHERE id=?");
                    shift_ = prepare("UPDATE profiles SET doc=" + fn +
                                     "(doc,'$.credit',json_extract(doc,'$.credit')+?) WHERE id=?");
                }
            }
            replace_ = prepare(rows_ ? "UPDATE profiles SET "
                                       "revision=?,credit=?,level=?,status=?,name=?,email=?,bio=?,"
                                       "region=?,last_login=? WHERE id=?"
                                     : "UPDATE profiles SET doc=" +
                                           std::string(jsonb_ ? "jsonb(?)" : "?") + " WHERE id=?");
            std::string marks;
            for (unsigned i = 0; i < batch_size; ++i)
                marks += (i ? ",?" : "?");
            many_ = prepare("SELECT " + select + " FROM profiles WHERE id IN (" + marks +
                            ") ORDER BY id");
        } catch (...) {
            for (auto s : statements_)
                sqlite3_finalize(s);
            sqlite3_close(db_);
            throw;
        }
    }
    ~SQLiteConnection() {
        for (auto s : statements_)
            sqlite3_finalize(s);
        sqlite3_close(db_);
    }
    Profile get(uint64_t id) override {
        integer(get_, 1, id);
        if (sqlite3_step(get_) != SQLITE_ROW)
            throw std::runtime_error("SQLite get: " + std::string(sqlite3_errmsg(db_)));
        auto p = row(get_);
        reset(get_);
        return p;
    }
    Projection project(uint64_t id) override {
        integer(project_, 1, id);
        require(sqlite3_step(project_) == SQLITE_ROW, "SQLite projection missing");
        Projection p;
        if (pb_) {
            auto bytes = column_view(project_, 0);
            p = project_pb(bytes);
        } else
            p = {static_cast<uint64_t>(sqlite3_column_int64(project_, 0)),
                 sqlite3_column_int64(project_, 1)};
        reset(project_);
        return p;
    }
    void revision(uint64_t id, uint64_t value) override {
        if (pb_)
            patch_blob(id, 10, value);
        else {
            integer(rev_, 1, value);
            integer(rev_, 2, id);
            done(rev_);
        }
    }
    void increment(uint64_t id) override {
        shift(id, 1);
    }
    void transfer(uint64_t from, uint64_t to) override {
        exec("BEGIN IMMEDIATE");
        try {
            require(project(from).credit > 0, "insufficient credit");
            shift(from, -1);
            shift(to, 1);
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
    }
    void put(const Profile &p) override {
        integer(put_, 1, p.id);
        if (rows_) {
            integer(put_, 2, p.revision);
            integer(put_, 3, p.credit);
            integer(put_, 4, p.level);
            integer(put_, 5, p.status);
            text(put_, 6, p.name);
            text(put_, 7, p.email);
            text(put_, 8, p.bio);
            integer(put_, 9, p.region);
            integer(put_, 10, p.last_login);
            done(put_);
        } else {
            auto value = pb_ ? encode_pb(p) : encode_json(p);
            text(put_, 2, value, pb_);
            done(put_);
        }
    }
    void replace(const Profile &p) override {
        if (rows_) {
            integer(replace_, 1, p.revision);
            integer(replace_, 2, p.credit);
            integer(replace_, 3, p.level);
            integer(replace_, 4, p.status);
            text(replace_, 5, p.name);
            text(replace_, 6, p.email);
            text(replace_, 7, p.bio);
            integer(replace_, 8, p.region);
            integer(replace_, 9, p.last_login);
            integer(replace_, 10, p.id);
            done(replace_);
        } else {
            auto bytes = pb_ ? encode_pb(p) : encode_json(p);
            text(replace_, 1, bytes, pb_);
            integer(replace_, 2, p.id);
            done(replace_);
        }
    }
    std::vector<Profile> many(uint64_t first) override {
        for (unsigned i = 0; i < batch_size; ++i)
            integer(many_, i + 1, first + i);
        std::vector<Profile> out;
        out.reserve(batch_size);
        int rc;
        while ((rc = sqlite3_step(many_)) == SQLITE_ROW)
            out.push_back(row(many_));
        require(rc == SQLITE_DONE && out.size() == batch_size, "SQLite batch missing");
        reset(many_);
        return out;
    }
    void seed_begin() override {
        exec("BEGIN IMMEDIATE");
    }
    void seed_end() override {
        exec("COMMIT");
    }
};
class SQLiteEngine final : public Engine {
    void sql(const std::string &sql) {
        sqlite3 *raw = nullptr;
        require(sqlite3_open((config.data + "/main.db").c_str(), &raw) == SQLITE_OK,
                "SQLite engine open");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        char *e = nullptr;
        auto rc = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &e);
        std::string msg = e ? e : "";
        sqlite3_free(e);
        require(rc == SQLITE_OK, "SQLite schema: " + msg);
    }

  public:
    explicit SQLiteEngine(const Config &c) : Engine(c) {
        std::filesystem::create_directories(c.data);
    }
    std::unique_ptr<Connection> connect() override {
        return std::make_unique<SQLiteConnection>(config);
    }
    void reset() override {
        sql("PRAGMA journal_mode=WAL; DROP TABLE IF EXISTS profiles; " +
            std::string(config.engine == "sqlite-rows"
                            ? "CREATE TABLE profiles(id INTEGER PRIMARY KEY,revision INTEGER NOT "
                              "NULL,credit INTEGER NOT NULL,level INTEGER NOT NULL,status INTEGER "
                              "NOT NULL,name TEXT NOT NULL,email TEXT NOT NULL,bio TEXT NOT "
                              "NULL,region INTEGER NOT NULL,last_login INTEGER NOT NULL)"
                        : config.engine == "sqlite-pb"
                            ? "CREATE TABLE profiles(id INTEGER PRIMARY KEY,doc BLOB NOT NULL)"
                            : "CREATE TABLE profiles(id INTEGER PRIMARY KEY,doc " +
                                  std::string(config.engine == "sqlite-jsonb" ? "BLOB" : "TEXT") +
                                  " NOT NULL)"));
    }
    void sync() override {
        sql("PRAGMA synchronous=FULL; PRAGMA wal_checkpoint(TRUNCATE)");
    }
    std::string info() override {
        return "{\"sqlite_version\":\"" + std::string(sqlite3_libversion()) + "\"}";
    }
};
std::unique_ptr<Engine> make_sqlite(const Config &c) {
    return std::make_unique<SQLiteEngine>(c);
}
} // namespace business
