// Production-API integration checks: atomicity, snapshots, CAS, bounded retrieval and vectors.
#include "fengqudb_index.h"
#include "fengqudb_wire.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace w = fengqudb::wire;
static unsigned checks = 0;
static void expect(bool value, const char *why) {
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
static void ok(fqdb_status s) {
    if (s != FQDB_OK)
        throw std::runtime_error(fqdb_status_message(s));
}
static fqdb_slice bytes(std::string_view s) {
    return {s.data(), s.size()};
}
struct Buffer {
    fqdb_buffer b{};
    ~Buffer() {
        fqdb_buffer_free(&b);
    }
    std::string text() const {
        return {b.size ? static_cast<char *>(b.data) : "", static_cast<size_t>(b.size)};
    }
};
struct Tx {
    fqdb_store_txn *p = nullptr;
    explicit Tx(fqdb_store *s) {
        ok(fqdb_store_txn_begin(s, 0, &p));
    }
    ~Tx() {
        fqdb_store_txn_close(p);
    }
    void commit() {
        uint64_t v = 0;
        ok(fqdb_store_txn_commit(p, &v, 1));
    }
};
struct Snap {
    fqdb_store_snapshot *p = nullptr;
    explicit Snap(fqdb_store *s) {
        ok(fqdb_store_snapshot_create(s, &p));
    }
    ~Snap() {
        fqdb_store_snapshot_close(p);
    }
};
struct Identity {
    fqdb_store_bridge_info info{};
    explicit Identity(fqdb_store *s) {
        info.struct_size = sizeof(info);
        info.abi_version = 1;
        ok(fqdb_store_bridge_identity(s, &info));
    }
};
static std::string scalar_text(std::string_view s) {
    w::Writer a;
    a.uint_field(1, 1);
    a.bytes_field(2, s);
    return a.take();
}
static std::string scalar_uint(uint64_t n) {
    w::Writer a;
    a.uint_field(1, 3);
    a.uint_field(4, n);
    return a.take();
}
static void meta(w::Writer &d, std::string_view name, std::string_view v) {
    w::Writer a;
    a.bytes_field(1, name);
    a.bytes_field(2, v);
    d.bytes_field(6, a.data());
}
static std::string floats(float a, float b) {
    w::Writer x;
    uint32_t v = 0;
    std::memcpy(&v, &a, 4);
    x.fixed(v, 4);
    std::memcpy(&v, &b, 4);
    x.fixed(v, 4);
    return x.take();
}
static std::string document(std::string_view id, uint64_t version, std::string_view text,
                            std::string_view app = "editor", std::string_view vector = {}) {
    w::Writer d;
    d.bytes_field(1, id);
    d.uint_field(2, version);
    d.uint_field(3, 42);
    d.uint_field(4, 1);
    w::Writer t;
    t.bytes_field(1, "body");
    t.bytes_field(2, text);
    d.bytes_field(5, t.data());
    meta(d, "app", scalar_text(app));
    meta(d, "wide", scalar_uint(UINT64_MAX));
    if (!vector.empty()) {
        d.bytes_field(7, vector);
        d.bytes_field(8, "space-v1");
    }
    d.bytes_field(11, "payload");
    return d.take();
}
static std::string mutation(std::string_view doc, uint64_t expected = 0,
                            std::string_view partition = "one") {
    w::Writer x;
    x.bytes_field(1, "test");
    x.bytes_field(2, partition);
    x.uint_field(3, 1);
    x.uint_field(4, expected);
    x.bytes_field(5, doc);
    return x.take();
}
static std::string predicate(std::string_view field, std::string_view scalar, uint32_t op = 1) {
    w::Writer p;
    p.uint_field(1, op);
    p.bytes_field(2, field);
    p.bytes_field(3, scalar);
    return p.take();
}
struct Request {
    w::Writer x;
    Request(std::string_view text = "", uint32_t op = 1, std::string_view partition = "one") {
        x.bytes_field(1, "test");
        x.bytes_field(2, partition);
        x.uint_field(3, op);
        if (!text.empty())
            x.bytes_field(4, text);
    }
};
struct Hit {
    std::string id, raw;
    double score = 0;
    uint64_t version = 0;
};
struct Result {
    std::vector<Hit> hits;
    uint64_t sequence = 0, records = 0, stop = 0, evidence = 0;
    std::string cursor;
};
static Result decode(std::string_view s) {
    Result o;
    w::Reader r(s);
    w::Field f;
    while (r.next(f)) {
        uint64_t n = 0;
        std::string_view b;
        if (f.number == 1) {
            expect(r.bytes(b), "result hit wire");
            Hit h;
            w::Reader a(b);
            w::Field q;
            while (a.next(q)) {
                if (q.number == 1) {
                    expect(a.bytes(b), "document wire");
                    h.raw = b;
                    w::Reader d(b);
                    w::Field z;
                    while (d.next(z)) {
                        if (z.number == 1) {
                            expect(d.bytes(b), "id wire");
                            h.id = b;
                        } else if (z.number == 2) {
                            expect(d.varint(n), "version wire");
                            h.version = n;
                        } else
                            expect(d.skip(z), "doc skip");
                    }
                    expect(d.done(), "doc done");
                } else if (q.number == 2) {
                    expect(a.fixed(n, 8), "score wire");
                    std::memcpy(&h.score, &n, 8);
                } else
                    expect(a.skip(q), "hit skip");
            }
            o.hits.push_back(std::move(h));
        } else if (f.number == 3) {
            expect(r.varint(o.sequence), "sequence wire");
        } else if (f.number == 5) {
            expect(r.varint(o.stop), "stop wire");
        } else if (f.number == 7) {
            expect(r.varint(o.evidence), "evidence wire");
        } else if (f.number == 11) {
            expect(r.bytes(b), "cursor wire");
            o.cursor = b;
        } else if (f.number == 13) {
            expect(r.varint(o.records), "stats wire");
        } else
            expect(r.skip(f), "result skip");
    }
    expect(r.done(), "result done");
    return o;
}
static Result search(const Snap &s, uint64_t token, const Request &q) {
    Buffer b;
    ok(fqidx_snapshot_query(s.p, token, bytes(q.x.data()), nullptr, &b.b));
    return decode(b.text());
}
static void upsert(fqdb_store *s, uint64_t token, std::string_view d, uint64_t old = 0,
                   std::string_view part = "one") {
    Tx t(s);
    auto m = mutation(d, old, part);
    ok(fqidx_txn_apply(t.p, token, bytes(m)));
    t.commit();
}
static uint32_t FQDB_CALL cancel(void *) {
    return 1;
}
// An exhaustive oracle over independently counted ASCII words. It deliberately
// does not reuse the native analyzer, posting decoder, scoring or filtering code.
static void oracle(fqdb_store *store, uint64_t token) {
    constexpr unsigned count = 96, words = 7;
    struct Record {
        std::array<unsigned, words> body{}, title{};
        unsigned body_length = 0, title_length = 0;
        uint64_t version = 1;
        bool alive = true, editor = false;
        float x = 0, y = 0;
    };
    std::array<Record, count> records{};
    std::mt19937 rng(8137);
    auto word = [](unsigned n) { return "term" + std::to_string(n); };
    auto id = [](unsigned n) { return "record" + std::to_string(n); };
    auto generate = [&](Record &r) {
        r.body.fill(0);
        r.title.fill(0);
        r.body_length = 1 + rng() % 12;
        r.title_length = 1 + rng() % 3;
        for (unsigned j = 0; j < r.body_length; ++j)
            ++r.body[rng() % words];
        for (unsigned j = 0; j < r.title_length; ++j)
            ++r.title[rng() % words];
        r.editor = rng() % 3 != 0;
        r.x = static_cast<float>(int(rng() % 201) - 100);
        r.y = static_cast<float>(int(rng() % 201) - 100);
        if (r.x == 0 && r.y == 0)
            r.x = 1;
    };
    auto text = [&](const auto &frequencies) {
        std::string s;
        for (unsigned j = 0; j < words; ++j)
            for (unsigned k = 0; k < frequencies[j]; ++k)
                s += word(j) + " ";
        return s;
    };
    auto encode_record = [&](unsigned n) {
        const auto &r = records[n];
        auto d = document(id(n), r.version, text(r.body), r.editor ? "editor" : "other",
                          floats(r.x, r.y));
        w::Writer title, field;
        field.bytes_field(1, "title");
        field.bytes_field(2, text(r.title));
        field.double_field(3, 2.5);
        title.bytes_field(5, field.data());
        d += title.data();
        return d;
    };
    {
        Tx tx(store);
        for (unsigned i = 0; i < count; ++i) {
            generate(records[i]);
            auto m = mutation(encode_record(i), 0, "oracle");
            ok(fqidx_txn_apply(tx.p, token, bytes(m)));
        }
        tx.commit();
    }
    auto verify = [&] {
        unsigned live = 0;
        double body_sum = 0, title_sum = 0;
        std::array<unsigned, words> df{};
        for (const auto &r : records) {
            if (!r.alive)
                continue;
            ++live;
            body_sum += r.body_length;
            title_sum += r.title_length;
            for (unsigned j = 0; j < words; ++j)
                if (r.body[j] || r.title[j])
                    ++df[j];
        }
        Snap snap(store);
        for (unsigned sample = 0; sample < 28; ++sample) {
            unsigned a = sample % words, b = (a + 1 + sample / words) % words;
            bool all = sample % 2, filtered = sample % 3 == 0;
            Request q(word(a) + " " + word(b), 1, "oracle");
            q.x.uint_field(5, 11);
            q.x.uint_field(15, all);
            if (filtered) {
                // AND(EQ(app, editor), NOT(EQ(app, other))).
                w::Writer no, both;
                no.uint_field(1, 11);
                no.bytes_field(4, predicate("app", scalar_text("other")));
                both.uint_field(1, 9);
                both.bytes_field(4, predicate("app", scalar_text("editor")));
                both.bytes_field(4, no.data());
                q.x.bytes_field(6, both.data());
            }
            std::vector<Hit> expected;
            for (unsigned n = 0; n < count; ++n) {
                const auto &r = records[n];
                if (!r.alive || (filtered && !r.editor))
                    continue;
                bool has_a = r.body[a] || r.title[a], has_b = r.body[b] || r.title[b];
                if (all ? !(has_a && has_b) : !(has_a || has_b))
                    continue;
                double score = 0;
                auto order = std::array<unsigned, 2>{a, b};
                std::sort(order.begin(), order.end(), [&](unsigned x, unsigned y) {
                    return df[x] != df[y] ? df[x] < df[y] : x < y;
                });
                for (auto j : order) {
                    double idf = std::log(1 + (live - df[j] + .5) / (df[j] + .5));
                    auto field_score = [&](double tf, double length, double sum, double weight) {
                        return tf ? idf * weight * (tf * 2.2) /
                                        (tf + 1.2 * (.25 + .75 * length / (sum / live)))
                                  : 0.;
                    };
                    score += field_score(r.body[j], r.body_length, body_sum, 1);
                    score += field_score(r.title[j], r.title_length, title_sum, 2.5);
                }
                expected.push_back({id(n), {}, score, r.version});
            }
            std::sort(expected.begin(), expected.end(), [](const Hit &x, const Hit &y) {
                return x.score != y.score ? x.score > y.score : x.id < y.id;
            });
            expected.resize(std::min<size_t>(11, expected.size()));
            auto got = search(snap, token, q);
            expect(got.records == live && got.hits.size() == expected.size(), "oracle count");
            for (size_t i = 0; i < expected.size(); ++i) {
                expect(got.hits[i].id == expected[i].id, "exhaustive BM25 ordering");
                expect(std::abs(got.hits[i].score - expected[i].score) < 1e-10,
                       "exhaustive BM25 weighted score");
            }
        }
        Request dense("", 1, "oracle");
        dense.x.uint_field(7, 1);
        dense.x.bytes_field(8, floats(3, 4));
        dense.x.bytes_field(9, "space-v1");
        dense.x.uint_field(5, count);
        auto got = search(snap, token, dense);
        std::vector<Hit> expected;
        for (unsigned n = 0; n < count; ++n) {
            const auto &r = records[n];
            if (!r.alive)
                continue;
            double length = std::hypot(double(r.x), double(r.y));
            double score =
                double(float(r.x / length)) * float(.6) + double(float(r.y / length)) * float(.8);
            expected.push_back({id(n), {}, score, r.version});
        }
        std::sort(expected.begin(), expected.end(), [](const Hit &x, const Hit &y) {
            return x.score != y.score ? x.score > y.score : x.id < y.id;
        });
        expect(got.hits.size() == expected.size(), "exact cosine count");
        for (size_t i = 0; i < expected.size(); ++i) {
            expect(got.hits[i].id == expected[i].id, "exhaustive cosine ordering");
            expect(std::abs(got.hits[i].score - expected[i].score) < 1e-6, "cosine score");
        }
        std::atomic<bool> concurrent_ok{true};
        std::vector<std::thread> readers;
        for (unsigned i = 0; i < 4; ++i)
            readers.emplace_back([&] {
                // Avoid the non-atomic test assertion counter on reader threads.
                for (unsigned j = 0; j < 16; ++j) {
                    Buffer result;
                    if (fqidx_snapshot_query(snap.p, token, bytes(dense.x.data()), nullptr,
                                             &result.b) != FQDB_OK)
                        concurrent_ok = false;
                }
            });
        for (auto &reader : readers)
            reader.join();
        expect(concurrent_ok, "concurrent queries share an immutable snapshot");
    };
    verify();
    {
        Tx tx(store);
        for (unsigned i = 0; i < count; i += 4) {
            auto &r = records[i];
            ++r.version;
            generate(r);
            auto m = mutation(encode_record(i), r.version - 1, "oracle");
            ok(fqidx_txn_apply(tx.p, token, bytes(m)));
        }
        for (unsigned i = 1; i < count; i += 5) {
            auto &r = records[i];
            w::Writer m;
            m.bytes_field(1, "test");
            m.bytes_field(2, "oracle");
            m.uint_field(3, 3);
            m.uint_field(4, r.version);
            m.bytes_field(6, id(i));
            m.uint_field(7, ++r.version);
            m.uint_field(8, 1);
            ok(fqidx_txn_apply(tx.p, token, bytes(m.data())));
            r.alive = false;
        }
        tx.commit();
    }
    verify();
}
int main() {
    auto path = std::filesystem::current_path() /
                ("index-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fqdb_store *store = nullptr, *other = nullptr;
    try {
        expect(!std::filesystem::exists(path), "fixture path must be unused");
        std::filesystem::create_directory(path);
        ok(fqdb_store_open_at(path.string().c_str(), "main", nullptr, &store));
        ok(fqdb_store_open_at(path.string().c_str(), "other", nullptr, &other));
        auto token = Identity(store).info.store_token;
        auto second = Identity(other).info.store_token;
        expect(token && token != second, "distinct Core tokens");
        expect(fqidx_abi_version() == 1, "module ABI");
        upsert(store, token,
               document("a", 1, "数据库 可靠 写入 durable save", "editor", floats(1, 0)));
        upsert(store, token,
               document("b", 1, "数据库 缓存 性能 cache save", "other", floats(0, 1)));
        upsert(store, token, document("c", 1, "unrelated text", "editor", floats(-1, 0)));
        upsert(store, token, document("a", 1, "secret partition"), 0, "two");
        {
            Snap s(store);
            auto r = search(s, token, Request("可靠"));
            expect(r.hits.size() == 1 && r.hits[0].id == "a" && r.records == 3,
                   "CJK posting retrieval");
            auto p = Request("save");
            p.x.bytes_field(6, predicate("app", scalar_text("other")));
            r = search(s, token, p);
            expect(r.hits.size() == 1 && r.hits[0].id == "b", "hard typed filter");
            auto big = Request("save");
            big.x.bytes_field(6, predicate("wide", scalar_uint(UINT64_MAX)));
            expect(search(s, token, big).hits.size() == 2, "uint64 exact predicate");
            auto v = Request();
            v.x.uint_field(7, 1);
            v.x.bytes_field(8, floats(10, 0));
            v.x.bytes_field(9, "space-v1");
            v.x.uint_field(5, 2);
            r = search(s, token, v);
            expect(r.hits.size() == 2 && r.hits[0].id == "a" &&
                       std::abs(r.hits[0].score - 1) < 1e-6,
                   "exact normalized vectors");
            auto wrong = v;
            wrong.x.bytes_field(9, "unknown");
            Buffer b;
            expect(fqidx_snapshot_query(s.p, token, bytes(wrong.x.data()), nullptr, &b.b) ==
                       FQDB_UNSUPPORTED,
                   "unknown vector space");
            Request all("", 4);
            all.x.uint_field(5, 2);
            r = search(s, token, all);
            expect(r.hits.size() == 2 && r.cursor == "b", "keyset first page");
            Request next("", 4);
            next.x.uint_field(5, 2);
            next.x.bytes_field(17, r.cursor);
            next.x.uint_field(22, r.sequence);
            r = search(s, token, next);
            expect(r.hits.size() == 1 && r.hits[0].id == "c", "keyset next page");
        }
        auto old = std::make_unique<Snap>(store);
        {
            Tx t(store);
            ok(fqdb_store_txn_put(t.p, bytes("business"), bytes("new"), 0));
            w::Writer d;
            meta(d, "app", scalar_text("other"));
            w::Writer m;
            m.bytes_field(1, "test");
            m.bytes_field(2, "one");
            m.uint_field(3, 2);
            m.uint_field(4, 1);
            m.bytes_field(5, d.data());
            m.bytes_field(6, "a");
            m.uint_field(7, 2);
            m.uint_field(8, 1);
            ok(fqidx_txn_apply(t.p, token, bytes(m.data())));
            Request q("可靠");
            Buffer b;
            ok(fqidx_txn_query(t.p, token, bytes(q.x.data()), nullptr, &b.b));
            expect(decode(b.text()).hits.front().version == 2, "read own staged projection");
            t.commit();
        }
        expect(search(*old, token, Request("可靠")).hits.front().version == 1,
               "old snapshot remains old");
        old.reset();
        {
            Snap s(store);
            auto q = Request("可靠");
            q.x.bytes_field(6, predicate("app", scalar_text("editor")));
            expect(search(s, token, q).hits.empty(), "metadata patch affects filter");
            q = Request();
            q.x.uint_field(7, 1);
            q.x.bytes_field(8, floats(1, 0));
            q.x.bytes_field(9, "space-v1");
            expect(search(s, token, q).hits.front().id == "a", "patch retains vector");
        }
        {
            Tx t(store);
            ok(fqdb_store_txn_put(t.p, bytes("must_rollback"), bytes("x"), 0));
            auto m = mutation(document("a", 9, "bad"), 1);
            expect(fqidx_txn_apply(t.p, token, bytes(m)) == FQDB_CONFLICT, "stale CAS rejected");
            uint64_t version = 0;
            expect(fqdb_store_txn_commit(t.p, &version, 1) == FQDB_CANCELLED,
                   "failed index stage cancels entire transaction");
        }
        {
            Buffer b;
            expect(fqdb_store_get(store, bytes("must_rollback"), &b.b) == FQDB_NOT_FOUND,
                   "domain write rolled back");
        }
        upsert(store, token, document("a", 3, "replacement fooBar FQDB_COMMIT_UNKNOWN"), 2);
        {
            Snap s(store);
            expect(search(s, token, Request("可靠")).hits.empty(), "old-only postings removed");
            expect(search(s, token, Request("commit")).hits.front().id == "a", "identifier pieces");
            auto q = Request("数据库");
            q.x.uint_field(10, 1);
            Buffer b;
            expect(fqidx_snapshot_query(s.p, token, bytes(q.x.data()), nullptr, &b.b) == FQDB_LIMIT,
                   "budget is not successful empty");
            q.x.uint_field(19, 1);
            auto r = search(s, token, q);
            expect(r.stop == FQIDX_POSTING_LIMIT && r.evidence != FQIDX_PROVEN_NONE,
                   "partial outcome explicit");
            fqidx_query_control control{sizeof(control), 1, nullptr, cancel};
            Request normal("save");
            expect(fqidx_snapshot_query(s.p, token, bytes(normal.x.data()), &control, &b.b) ==
                       FQDB_CANCELLED,
                   "query cancel");
            expect(fqidx_snapshot_query(s.p, second, bytes(normal.x.data()), nullptr, &b.b) ==
                       FQDB_CONFLICT,
                   "cross-store handle rejected");
        }
        {
            Tx t(store);
            w::Writer m;
            m.bytes_field(1, "test");
            m.bytes_field(2, "one");
            m.uint_field(3, 3);
            m.uint_field(4, 3);
            m.bytes_field(6, "a");
            m.uint_field(7, 4);
            m.uint_field(8, 1);
            ok(fqidx_txn_apply(t.p, token, bytes(m.data())));
            t.commit();
        }
        {
            Snap s(store);
            expect(search(s, token, Request("replacement")).hits.empty(),
                   "deleted doc cannot return");
            expect(search(s, token, Request("secret", 1, "two")).hits.size() == 1,
                   "partition isolation");
        }
        {
            Tx t(store);
            auto m = mutation(document("a", 5, "revive"), 4);
            expect(fqidx_txn_apply(t.p, token, bytes(m)) == FQDB_CONFLICT,
                   "same generation resurrection rejected");
        }
        {
            Tx t(store);
            auto m = mutation(document("z", 1, "NaN", "editor",
                                       floats(std::numeric_limits<float>::quiet_NaN(), 0)));
            expect(fqidx_txn_apply(t.p, token, bytes(m)) == FQDB_INVALID_ARGUMENT,
                   "nonfinite vector rejected");
        }
        {
            Tx t(store);
            std::string malformed(11, static_cast<char>(0x80));
            expect(fqidx_txn_apply(t.p, token, bytes(malformed)) == FQDB_INVALID_ARGUMENT,
                   "malformed protobuf rejected");
        }
        fqdb_store_close(store);
        store = nullptr;
        fqdb_store_close(other);
        other = nullptr;
        ok(fqdb_store_open_at(path.string().c_str(), "main", nullptr, &store));
        auto newtoken = Identity(store).info.store_token;
        expect(newtoken != token, "Core reopen gets new token");
        {
            Snap s(store);
            expect(search(s, newtoken, Request("save")).hits.size() == 1,
                   "persistent postings after reopen");
        }
        oracle(store, newtoken);
        fqdb_store_close(store);
        store = nullptr;
        std::cout << "PASS native index " << checks << " assertions\n";
    } catch (const std::exception &e) {
        if (store)
            fqdb_store_close(store);
        if (other)
            fqdb_store_close(other);
        std::cerr << e.what() << "\n";
        return 1;
    }
    std::filesystem::remove_all(path);
    return 0;
}
