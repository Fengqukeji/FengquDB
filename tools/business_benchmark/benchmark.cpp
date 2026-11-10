#include "common.hpp"
#include <mysql.h>
namespace business {
static std::unique_ptr<Engine> make(const Config &c) {
    if (c.engine == "fqdb")
        return make_fq(c);
    if (c.engine.find("sqlite-") == 0)
        return make_sqlite(c);
    if (c.engine == "mysql")
        return make_mysql(c);
    if (c.engine == "redis")
        return make_redis(c);
    if (c.engine == "hybrid")
        return make_hybrid(c);
    throw std::runtime_error("unknown engine");
}
struct Expected {
    uint64_t revision = 0;
    int64_t credit = 0;
};
static void save_expected(const Config &c, const std::vector<Expected> &values) {
    std::ofstream f(c.data + "/expected.bin", std::ios::binary | std::ios::trunc);
    uint64_t n = values.size();
    f.write(reinterpret_cast<const char *>(&n), 8);
    for (const auto &v : values) {
        f.write(reinterpret_cast<const char *>(&v.revision), 8);
        f.write(reinterpret_cast<const char *>(&v.credit), 8);
    }
    require(bool(f), "oracle write");
}
static std::vector<Expected> load_expected(const Config &c) {
    std::ifstream f(c.data + "/expected.bin", std::ios::binary);
    uint64_t n = 0;
    f.read(reinterpret_cast<char *>(&n), 8);
    require(n == c.records, "oracle record count");
    std::vector<Expected> out(n);
    for (auto &v : out) {
        f.read(reinterpret_cast<char *>(&v.revision), 8);
        f.read(reinterpret_cast<char *>(&v.credit), 8);
    }
    require(bool(f) && f.peek() == EOF, "oracle read");
    return out;
}
static void verify(Engine &engine, const std::vector<Expected> &expected) {
    auto conn = engine.connect();
    for (uint64_t first = 0; first < engine.config.records; first += batch_size) {
        auto rows = conn->many(first);
        require(rows.size() == batch_size, "verification row count");
        for (unsigned i = 0; i < batch_size; ++i) {
            auto id = first + i;
            auto want = fixture(id, engine.config.body);
            want.revision = expected[id].revision;
            want.credit = expected[id].credit;
            if (engine.config.workload == "save" && want.revision)
                want.bio.back() = static_cast<char>('0' + want.revision % 10);
            require(rows[i] == want, "complete business profile mismatch id=" + std::to_string(id));
        }
    }
}
static double percentile(std::vector<double> &s, double p) {
    if (s.empty())
        return 0;
    return s[std::min(s.size() - 1, static_cast<size_t>(s.size() * p))];
}
static void codec(const Config &c) {
    std::vector<Profile> data;
    for (unsigned i = 0; i < 1024; ++i)
        data.push_back(fixture(i, c.body));
    uint64_t checksum = 0;
    constexpr unsigned count = 50000;
    std::cout << "{\"ok\":true,\"kind\":\"codec\",\"body\":" << c.body << ",\"samples\":[";
    bool first = true;
    for (bool json : {false, true}) {
        std::vector<std::string> encoded;
        for (const auto &p : data) {
            encoded.push_back(json ? encode_json(p) : encode_pb(p));
            require((json ? decode_json(encoded.back()) : decode_pb(encoded.back())) == p,
                    "codec roundtrip");
        }
        for (bool decode : {false, true}) {
            auto start = Clock::now();
            for (unsigned i = 0; i < count; ++i) {
                auto at = i % data.size();
                if (decode)
                    checksum += score(json ? decode_json(encoded[at]) : decode_pb(encoded[at]));
                else {
                    auto text = json ? encode_json(data[at]) : encode_pb(data[at]);
                    checksum += text.size() + static_cast<unsigned char>(text.back());
                }
            }
            auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
            if (!first)
                std::cout << ",";
            first = false;
            std::cout << "{\"codec\":\"" << (json ? "yyjson" : "protobuf_wire")
                      << "\",\"operation\":\"" << (decode ? "decode" : "encode")
                      << "\",\"calls\":" << count << ",\"ops_per_second\":" << count / seconds
                      << ",\"record_bytes\":" << encoded[7].size() << "}";
        }
    }
    std::cout << "],\"checksum\":" << checksum << "}\n";
}
struct Worker {
    uint64_t calls = 0, reads = 0, writes = 0, checksum = 0;
    std::vector<double> latency;
    std::vector<uint64_t> revisions;
    std::vector<int64_t> delta;
    Metrics metrics;
};
static void run(Engine &engine) {
    auto &c = engine.config;
    auto expected = load_expected(c);
    // Common full warmup includes materialization. Cache hit/miss counts below
    // exclude setup and use only the requested business calls.
    verify(engine, expected);
    std::mutex mutex;
    std::condition_variable changed;
    unsigned ready = 0;
    bool go = false;
    std::atomic<bool> stop{false};
    Clock::time_point start;
    std::string error;
    std::vector<Worker> stats(c.threads);
    std::vector<std::thread> threads;
    for (unsigned tid = 0; tid < c.threads; ++tid) {
        threads.emplace_back([&, tid] {
            bool registered = false;
            mysql_thread_init();
            try {
                auto conn = engine.connect();
                auto &w = stats[tid];
                w.latency.reserve(100000);
                w.delta.resize(c.records);
                w.revisions.resize(c.records);
                const uint64_t low = uint64_t(c.records) * tid / c.threads,
                               high = uint64_t(c.records) * (tid + 1) / c.threads;
                std::vector<Profile> input;
                if (c.workload == "save")
                    for (uint64_t id = low; id < high; ++id)
                        input.push_back(fixture(id, c.body));
                std::mt19937_64 random(0xC0FFEE + tid);
                std::vector<double> weights(c.records);
                for (unsigned i = 0; i < c.records; ++i)
                    weights[i] = 1.0 / std::pow(i + 1.0, 0.99);
                std::discrete_distribution<unsigned> zipf(weights.begin(), weights.end());
                std::array<unsigned, 16384> trace{};
                for (auto &id : trace)
                    id = zipf(random);
                std::array<unsigned char, 16384> choices{};
                for (auto &choice : choices)
                    choice = static_cast<unsigned char>(random() % 100);
                for (unsigned i = 0; i < 128; ++i)
                    require(conn->get(trace[i]).id == trace[i], "worker warmup id");
                conn->metrics = {};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ++ready;
                    registered = true;
                    changed.notify_all();
                    changed.wait(lock, [&] { return go; });
                }
                auto until = start + std::chrono::duration_cast<Clock::duration>(
                                         std::chrono::duration<double>(c.seconds));
                uint64_t at = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    if ((at & 63) == 0 && Clock::now() >= until)
                        break;
                    unsigned id = trace[(at * 2) % trace.size()],
                             next = trace[(at * 2 + 1) % trace.size()];
                    auto chance = choices[at % choices.size()];
                    bool write =
                        (c.workload == "A" && chance < 50) || (c.workload == "B" && chance < 5);
                    if (write || c.workload == "save")
                        id = static_cast<unsigned>(low + (id % (high - low)));
                    if (c.workload == "F" && chance < 50)
                        id %= 128;
                    if (c.workload == "transfer")
                        id %= 128;
                    const bool sample =
                        (at & ((c.mode == "sync" ||
                                (c.mode == "performance" && c.workload != "C" &&
                                 c.workload != "projection" && c.workload != "batch32"))
                                   ? 7
                                   : 63)) == 0;
                    Clock::time_point before;
                    if (sample)
                        before = Clock::now();
                    if (c.workload == "save") {
                        auto &p = input[id - low];
                        p.revision = (uint64_t(tid + 1) << 48) | (at + 1);
                        p.bio.back() = static_cast<char>('0' + p.revision % 10);
                        conn->replace(p);
                        w.revisions[id] = p.revision;
                        ++w.writes;
                    } else if (write) {
                        auto value = (uint64_t(tid + 1) << 48) | (at + 1);
                        conn->revision(id, value);
                        w.revisions[id] = value;
                        ++w.writes;
                    } else if (c.workload == "F" && chance < 50) {
                        conn->increment(id);
                        ++w.delta[id];
                        ++w.writes;
                    } else if (c.workload == "transfer") {
                        auto to = (id + 1 + next % 127) % 128;
                        conn->transfer(id, to);
                        --w.delta[id];
                        ++w.delta[to];
                        ++w.writes;
                    } else if (c.workload == "projection") {
                        auto p = conn->project(id);
                        require(p.credit > 0, "projection credit");
                        w.checksum += p.revision + static_cast<uint64_t>(p.credit);
                        ++w.reads;
                    } else if (c.workload == "batch32") {
                        auto first = uint64_t(id) % (c.records / batch_size) * batch_size;
                        auto rows = conn->many(first);
                        require(rows.size() == batch_size, "batch business count");
                        for (auto &p : rows)
                            w.checksum += score(p);
                        ++w.reads;
                    } else {
                        auto p = conn->get(id);
                        require(p.id == id, "business returned wrong id");
                        w.checksum += score(p);
                        ++w.reads;
                    }
                    if (sample && w.latency.size() < 100000)
                        w.latency.push_back(
                            std::chrono::duration<double, std::micro>(Clock::now() - before)
                                .count());
                    ++w.calls;
                    ++at;
                }
                w.metrics = conn->metrics;
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex);
                if (error.empty())
                    error = e.what();
                stop.store(true);
                if (!registered)
                    ++ready;
                changed.notify_all();
            }
            mysql_thread_end();
        });
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready == c.threads; });
        std::cerr << "BENCH_TIMED_BEGIN\n";
        start = Clock::now();
        go = true;
        changed.notify_all();
    }
    for (auto &t : threads)
        t.join();
    auto visible_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (c.mode == "performance")
        engine.sync();
    auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::cerr << "BENCH_TIMED_END\n";
    require(error.empty(), error);
    std::vector<double> latency;
    uint64_t calls = 0, reads = 0, writes = 0, checksum = 0, hits = 0, misses = 0, retries = 0;
    for (auto &w : stats) {
        calls += w.calls;
        reads += w.reads;
        writes += w.writes;
        checksum += w.checksum;
        hits += w.metrics.hits;
        misses += w.metrics.misses;
        retries += w.metrics.retries;
        latency.insert(latency.end(), w.latency.begin(), w.latency.end());
        for (unsigned i = 0; i < c.records; ++i) {
            if (w.revisions[i])
                expected[i].revision = w.revisions[i];
            expected[i].credit += w.delta[i];
        }
    }
    require(calls > 0, "no calls");
    save_expected(c, expected);
    engine.sync();
    verify(engine, expected);
    std::sort(latency.begin(), latency.end());
    auto pb = encode_pb(fixture(7, c.body)), json = encode_json(fixture(7, c.body));
    std::cout << "{\"ok\":true,\"engine\":\"" << c.engine << "\",\"mode\":\"" << c.mode
              << "\",\"workload\":\"" << c.workload << "\",\"threads\":" << c.threads
              << ",\"records\":" << c.records << ",\"body\":" << c.body
              << ",\"seconds\":" << seconds << ",\"calls\":" << calls
              << ",\"visible_seconds\":" << visible_seconds
              << ",\"visible_ops_per_second\":" << calls / visible_seconds
              << ",\"flush_tail_ms\":" << (seconds - visible_seconds) * 1000
              << ",\"business_ops_per_second\":" << calls / seconds
              << ",\"rows_per_second\":" << calls * (c.workload == "batch32" ? 32.0 : 1.0) / seconds
              << ",\"reads\":" << reads << ",\"writes\":" << writes
              << ",\"sample_count\":" << latency.size() << ",\"p50_us\":" << percentile(latency, .5)
              << ",\"p95_us\":" << percentile(latency, .95)
              << ",\"p99_us\":" << percentile(latency, .99) << ",\"retries\":" << retries
              << ",\"cache_hits\":" << hits << ",\"cache_misses\":" << misses
              << ",\"checksum\":" << checksum << ",\"protobuf_bytes\":" << pb.size()
              << ",\"json_bytes\":" << json.size() << ",\"engine_info\":" << engine.info() << "}\n";
}
static int main_impl(int argc, char **argv) {
    Config c;
    for (int i = 1; i < argc; i += 2) {
        require(i + 1 < argc, "missing argument");
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--engine")
            c.engine = v;
        else if (k == "--mode")
            c.mode = v;
        else if (k == "--workload")
            c.workload = v;
        else if (k == "--action")
            c.action = v;
        else if (k == "--period") {
            const auto period = std::stoull(v);
            require(period >= 1 && period <= 60000, "invalid period");
            c.period_ms = static_cast<uint32_t>(period);
        } else if (k == "--threshold")
            c.threshold_bytes = std::stoull(v);
        else if (k == "--body")
            c.body = std::stoul(v);
        else if (k == "--records")
            c.records = std::stoul(v);
        else if (k == "--threads")
            c.threads = std::stoul(v);
        else if (k == "--seconds")
            c.seconds = std::stod(v);
        else if (k == "--data")
            c.data = v;
        else
            throw std::runtime_error("unknown argument");
    }
    require(c.records >= 256 && c.records % batch_size == 0 && c.threads >= 1 && c.threads <= 64 &&
                c.records / c.threads >= 32 && c.body >= 8 && c.body <= 65536 && c.seconds >= .05 &&
                c.seconds <= 60 && c.threshold_bytes >= 1 && c.threshold_bytes <= (64ull << 20) &&
                c.period_ms >= 1 && c.period_ms <= 60000,
            "invalid configuration");
    require(c.mode == "sync" || c.mode == "buffered" ||
                (c.engine == "fqdb" && c.mode == "performance"),
            "unknown durability");
    require(c.workload == "A" || c.workload == "B" || c.workload == "C" || c.workload == "F" ||
                c.workload == "projection" || c.workload == "batch32" || c.workload == "transfer" ||
                c.workload == "save",
            "unknown workload");
    if (c.action == "codec") {
        codec(c);
        return 0;
    }
    auto engine = make(c);
    if (c.action == "prepare") {
        engine->reset();
        auto conn = engine->connect();
        std::vector<Expected> expected(c.records);
        // Bound fixture transactions independently of row size. A fixed 256
        // rows exceeds the native staging budget for 64 KiB profiles.
        const auto seed_rows = unsigned(std::min<uint64_t>(256, (4ull << 20) / (c.body + 512)));
        for (unsigned first = 0; first < c.records; first += seed_rows) {
            conn->seed_begin();
            for (unsigned i = first; i < std::min(first + seed_rows, c.records); ++i) {
                auto p = fixture(i, c.body);
                require(decode_pb(encode_pb(p)) == p && decode_json(encode_json(p)) == p,
                        "seed codec equality");
                conn->put(p);
                expected[i].credit = p.credit;
            }
            conn->seed_end();
        }
        conn.reset();
        engine->sync();
        save_expected(c, expected);
        verify(*engine, expected);
        std::cout << "{\"ok\":true,\"action\":\"prepare\"}\n";
    } else if (c.action == "verify") {
        verify(*engine, load_expected(c));
        std::cout << "{\"ok\":true,\"action\":\"verify\"}\n";
    } else if (c.action == "run")
        run(*engine);
    else
        throw std::runtime_error("unknown action");
    return 0;
}
} // namespace business
int main(int argc, char **argv) {
    if (mysql_library_init(0, nullptr, nullptr) != 0)
        return 2;
    int status = 0;
    try {
        status = business::main_impl(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::cout << "{\"ok\":false}\n";
        status = 1;
    }
    mysql_library_end();
    return status;
}
