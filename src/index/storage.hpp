#pragma once
#include "tokenizer.hpp"
#include <chrono>
#include <utility>
namespace fengqudb::index::impl {
inline void check(fqdb_status s) {
    if (s != FQDB_OK)
        throw Error{s};
}
inline std::string component(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(c));
        if (!c)
            out.push_back(static_cast<char>(255));
    }
    out.append(2, '\0');
    return out;
}
inline std::string upper(std::string s) {
    for (size_t i = s.size(); i; --i) {
        auto c = static_cast<unsigned char>(s[i - 1]);
        if (c != 255) {
            s[i - 1] = static_cast<char>(c + 1);
            s.resize(i);
            return s;
        }
    }
    return {};
}
inline std::string root(std::string_view space, std::string_view partition) {
    require(!space.empty() && space.size() <= 128 && !partition.empty() && partition.size() <= 512);
    return std::string("\xff"
                       "FQIX1",
                       6) +
           component(space) + component(partition);
}
struct Blob {
    fqdb_buffer owned{};
    fqdb_store_value *owner = nullptr;
    std::string_view value;
    bool found = false;
    Blob() = default;
    Blob(const Blob &) = delete;
    Blob &operator=(const Blob &) = delete;
    Blob(Blob &&other) noexcept
        : owned(std::exchange(other.owned, {})), owner(std::exchange(other.owner, nullptr)),
          value(other.value), found(other.found) {}
    ~Blob() {
        if (owner)
            fqdb_store_value_close(owner);
        fqdb_buffer_free(&owned);
    }
};
struct Reader {
    fqdb_store_txn *tx = nullptr;
    const fqdb_store_snapshot *snapshot = nullptr;
    fqdb_store_bridge_info identity{};
    Blob get(std::string_view key) const {
        Blob out;
        fqdb_status status;
        if (tx) {
            status = fqdb_store_txn_get(tx, slice(key), &out.owned);
            if (status == FQDB_OK)
                out.value = {static_cast<const char *>(out.owned.data),
                             static_cast<size_t>(out.owned.size)};
        } else {
            fqdb_store_shared_value v{};
            status = fqdb_store_snapshot_get_shared(snapshot, slice(key), &v);
            out.owner = v.owner;
            if (status == FQDB_OK)
                out.value = view(v.row.value);
        }
        if (status == FQDB_NOT_FOUND)
            return out;
        check(status);
        out.found = true;
        return out;
    }
    void put(std::string_view key, std::string_view data) const {
        require(tx != nullptr);
        check(fqdb_store_txn_put(tx, slice(key), slice(data), 0));
    }
    void erase(std::string_view key) const {
        require(tx != nullptr);
        check(fqdb_store_txn_delete(tx, slice(key)));
    }
    template <class F>
    bool scan(const std::string &prefix, F fn, std::string_view after = {}) const {
        auto start = after.empty() ? prefix : prefix + component(after) + std::string(1, '\0');
        auto end = upper(prefix);
        fqdb_store_cursor *raw = nullptr;
        check(tx ? fqdb_store_txn_cursor_open(tx, slice(start), slice(end), 0, &raw)
                 : fqdb_store_cursor_open(snapshot, slice(start), slice(end), 0, &raw));
        struct Close {
            fqdb_store_cursor *p;
            ~Close() {
                fqdb_store_cursor_close(p);
            }
        } close{raw};
        for (;;) {
            fqdb_store_row row{};
            auto status = fqdb_store_cursor_next(raw, &row);
            if (status == FQDB_END)
                return true;
            check(status);
            if (!fn(view(row.key), view(row.value)))
                return false;
        }
    }
};
inline Reader reader(fqdb_store_txn *tx, uint64_t expected) {
    require(tx && expected);
    Reader r;
    r.tx = tx;
    r.identity.struct_size = sizeof(r.identity);
    r.identity.abi_version = 1;
    check(fqdb_store_txn_bridge_identity(tx, &r.identity));
    require(r.identity.store_token == expected, FQDB_CONFLICT);
    return r;
}
inline Reader reader(const fqdb_store_snapshot *s, uint64_t expected) {
    require(s && expected);
    Reader r;
    r.snapshot = s;
    r.identity.struct_size = sizeof(r.identity);
    r.identity.abi_version = 1;
    check(fqdb_store_snapshot_bridge_identity(s, &r.identity));
    require(r.identity.store_token == expected, FQDB_CONFLICT);
    return r;
}
struct Stats {
    uint64_t count = 0, vectors = 0, sequence = 0, layout = 0;
    std::map<std::string, uint64_t> lengths;
};
inline Stats stats(const Reader &r, const std::string &base) {
    auto b = r.get(base + "S");
    Stats s;
    if (!b.found)
        return s;
    try {
        fields(b.value, [&](w::Reader &a, w::Field f) {
            switch (f.number) {
            case 1:
                s.count = number(a, f);
                break;
            case 2:
                s.vectors = number(a, f);
                break;
            case 3:
                s.sequence = number(a, f);
                break;
            case 4: {
                std::string name;
                uint64_t n = 0;
                fields(bytes(a, f), [&](w::Reader &x, w::Field q) {
                    if (q.number == 1)
                        name = bytes(x, q);
                    else if (q.number == 2)
                        n = number(x, q);
                    else
                        return false;
                    return true;
                });
                require(!name.empty() && s.lengths.size() < 128);
                require(s.lengths.emplace(std::move(name), n).second);
                break;
            }
            case 5:
                require(number(a, f) == 1, FQDB_UNSUPPORTED);
                break;
            case 6:
                s.layout = number(a, f);
                break;
            default:
                return false;
            }
            return true;
        });
        require(s.vectors <= s.count);
    } catch (const Error &e) {
        throw Error{e.status == FQDB_UNSUPPORTED ? e.status : FQDB_CORRUPT};
    }
    return s;
}
inline std::string encode(const Stats &s) {
    w::Writer x;
    x.uint_field(1, s.count);
    x.uint_field(2, s.vectors);
    x.uint_field(3, s.sequence);
    x.uint_field(5, 1);
    x.uint_field(6, s.layout);
    for (const auto &[name, n] : s.lengths) {
        w::Writer a;
        a.bytes_field(1, name);
        a.uint_field(2, n);
        x.bytes_field(4, a.data());
    }
    return x.take();
}
inline Document load(const Reader &r, const std::string &base, std::string_view id,
                     bool header = false, bool internal = false) {
    auto b = r.get(base + "R" + component(id));
    require(b.found, FQDB_NOT_FOUND);
    try {
        auto d = document(b.value, header, internal);
        require(d.id == id && !d.id.empty() && d.version, FQDB_CORRUPT);
        return d;
    } catch (const Error &e) {
        throw Error{e.status == FQDB_LIMIT ? FQDB_LIMIT : FQDB_CORRUPT};
    }
}
inline uint64_t df(const Reader &r, const std::string &base, std::string_view term) {
    auto b = r.get(base + "F" + component(term));
    if (!b.found)
        return 0;
    uint64_t n = 0;
    try {
        fields(b.value, [&](w::Reader &a, w::Field f) {
            if (f.number != 1)
                return false;
            n = number(a, f);
            return true;
        });
    } catch (const Error &) {
        throw Error{FQDB_CORRUPT};
    }
    return n;
}
inline void update_df(const Reader &r, const std::string &base, const std::string &term,
                      int delta) {
    auto n = df(r, base, term);
    if (delta < 0) {
        require(n > 0, FQDB_CORRUPT);
        --n;
    } else {
        require(n < UINT64_MAX, FQDB_LIMIT);
        ++n;
    }
    auto key = base + "F" + component(term);
    {
        w::Writer w;
        w.uint_field(1, n);
        r.put(key, w.data());
    }
}
struct Stop {
    uint32_t reason;
};
struct Work {
    uint64_t memory = 16ull << 20, used = 0;
    std::chrono::steady_clock::time_point deadline{};
    bool timed = false;
    const fqidx_query_control *control = nullptr;
    void check() const {
        if (control && control->cancelled && control->cancelled(control->context))
            throw Stop{FQIDX_CANCELLED};
        if (timed && std::chrono::steady_clock::now() >= deadline)
            throw Stop{FQIDX_DEADLINE};
    }
    void take(uint64_t n) {
        if (n > memory || used > memory - n)
            throw Stop{FQIDX_MEMORY_LIMIT};
        used += n;
    }
    void release(uint64_t n) {
        used -= n;
    }
};
struct ScopedCharge {
    Work &w;
    uint64_t n;
    ScopedCharge(Work &x, uint64_t v) : w(x), n(v) {
        w.take(n);
    }
    ~ScopedCharge() {
        w.release(n);
    }
};
} // namespace fengqudb::index::impl
