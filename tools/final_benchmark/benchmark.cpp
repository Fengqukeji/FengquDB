#include "common.hpp"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <thread>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace final_bench {
struct Model {
    std::vector<uint64_t> revision;
    std::vector<unsigned char> alive;
};
static void write_oracle(const Config &c, const Model &model) {
    auto path = c.directory + "/expected.bin";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(bool(output), "create expected model");
    output.write("FQBENCH1", 8);
    uint64_t fields[]{c.records, c.payload, model.revision.size()};
    output.write(reinterpret_cast<const char *>(fields), sizeof(fields));
    output.write(reinterpret_cast<const char *>(model.revision.data()),
                 model.revision.size() * sizeof(uint64_t));
    output.write(reinterpret_cast<const char *>(model.alive.data()), model.alive.size());
    output.flush();
    require(bool(output), "write expected model");
    output.close();
    int file = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    require(file >= 0, "open expected model sync");
    auto status = ::fsync(file);
    ::close(file);
    require(status == 0, "sync expected model");
}
static Model read_oracle(const Config &c) {
    std::ifstream input(c.directory + "/expected.bin", std::ios::binary);
    require(bool(input), "missing expected model");
    char header[8];
    uint64_t fields[3]{};
    input.read(header, 8);
    input.read(reinterpret_cast<char *>(fields), sizeof(fields));
    require(input && std::string_view(header, 8) == "FQBENCH1" && fields[0] == c.records &&
                fields[1] == c.payload && fields[2] >= c.records &&
                fields[2] <= c.records + c.fixed_calls,
            "expected model header");
    Model model;
    model.revision.resize(fields[2]);
    model.alive.resize(fields[2]);
    input.read(reinterpret_cast<char *>(model.revision.data()),
               model.revision.size() * sizeof(uint64_t));
    input.read(reinterpret_cast<char *>(model.alive.data()), model.alive.size());
    require(bool(input) && input.peek() == std::char_traits<char>::eof(), "expected model body");
    require(std::all_of(model.alive.begin(), model.alive.end(), [](auto v) { return v <= 1; }),
            "expected visibility flags");
    return model;
}
static uint64_t verify(Engine &engine, const Config &c, const Model &model) {
    std::vector<unsigned char> seen(model.alive.size());
    auto connection = engine.connect();
    uint64_t count = 0;
    connection->scan([&](std::string_view k, std::string_view v) {
        auto id = key_id(k);
        require(id < model.alive.size() && model.alive[id], "unexpected persisted record");
        require(v == payload(id, model.revision[id], c.payload), "full persisted payload mismatch");
        if (seen[id]) {
            require(c.engine == "redis", "duplicate persisted record");
            return;
        }
        seen[id] = 1;
        ++count;
    });
    require(seen == model.alive, "persisted key set differs from model");
    if (c.engine == "hybrid") {
        for (size_t first = 0; first < model.alive.size(); first += batch_size) {
            bool all = true;
            uint64_t expected = 0;
            for (size_t i = first; i < std::min(first + batch_size, model.alive.size()); ++i) {
                all &= model.alive[i] != 0;
                expected += model.revision[i] + i + c.payload + 'a' + i % 26;
            }
            if (all && first + batch_size <= model.alive.size())
                require(connection->read_batch(first) == expected, "stale hybrid batch cache");
            else
                for (size_t i = first; i < std::min(first + batch_size, model.alive.size()); ++i) {
                    auto observed = connection->read(i, true, !model.alive[i]);
                    auto wanted =
                        model.alive[i] ? model.revision[i] + i + c.payload + 'a' + i % 26 : i;
                    require(observed == wanted, "stale hybrid point cache");
                }
        }
    }
    return count;
}
static std::map<std::string, uint64_t> cpu_stats() {
    std::ifstream input("/sys/fs/cgroup/cpu.stat");
    std::string key;
    uint64_t value;
    std::map<std::string, uint64_t> result;
    while (input >> key >> value)
        result[key] = value;
    return result;
}
static uint64_t number_file(const char *path) {
    std::ifstream input(path);
    uint64_t value = 0;
    input >> value;
    return value;
}
struct Random {
    uint64_t value;
    uint64_t next() {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        return value;
    }
};
struct Stats {
    uint64_t calls = 0, reads = 0, writes = 0, checksum = 0, warm_calls = 0, retries = 0;
    std::vector<double> latency;
    std::exception_ptr error;
};
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned ready = 0, warm = 0, phase = 0;
    std::atomic<bool> failed{false};
    Clock::time_point warm_end{}, end{};
    void fail() {
        std::lock_guard<std::mutex> lock(mutex);
        failed.store(true);
        changed.notify_all();
    }
};
static void prepare(Engine &engine, const Config &c) {
    {
        auto sample = payload(42, 7, c.payload);
        std::ofstream out(c.directory + "/payload.pb", std::ios::binary);
        out.write(sample.data(), sample.size());
        require(bool(out), "write Protobuf sample");
    }
    auto connection = engine.connect();
    std::array<Row, batch_size> rows;
    for (size_t first = 0; first < c.records; first += batch_size) {
        for (size_t i = 0; i < batch_size; ++i)
            rows[i] = {key(first + i), payload(first + i, 0, c.payload)};
        connection->put_batch(rows);
    }
    connection.reset();
    engine.sync();
    Model model;
    auto size = c.records + (c.workload == "insert" ? c.fixed_calls : 0);
    model.revision.resize(size);
    model.alive.resize(size);
    std::fill(model.alive.begin(), model.alive.begin() + c.records, 1);
    verify(engine, c, model);
    write_oracle(c, model);
}
static void selftest(Engine &engine, const Config &c) {
    auto model = read_oracle(c);
    require(c.fixed_calls >= batch_size, "selftest model space");
    model.revision.resize(c.records + batch_size);
    model.alive.resize(c.records + batch_size);
    auto connection = engine.connect();
    auto expected = [&](uint64_t id, uint64_t revision) {
        return revision + id + c.payload + 'a' + id % 26;
    };
    require(connection->read(0, true) == expected(0, 0) &&
                connection->read(1, false) == expected(1, 0),
            "point copy/borrow");
    require(connection->read(c.records, true, true) == c.records, "point missing");
    connection->put(0, payload(0, 7, c.payload));
    model.revision[0] = 7;
    require(connection->read(0, true) == expected(0, 7), "point update or stale cache");
    connection->patch(1, revision_bytes(8));
    model.revision[1] = 8;
    require(connection->read(1, true) == expected(1, 8), "patch or stale cache");
    connection->read(2, true);
    connection->erase(2);
    model.alive[2] = 0;
    require(connection->read(2, true, true) == 2, "delete or stale cache");
    connection->put(c.records, payload(c.records, 9, c.payload));
    model.revision[c.records] = 9;
    model.alive[c.records] = 1;
    require(connection->read(c.records, true) == expected(c.records, 9),
            "insert or stale negative cache");
    std::array<Row, batch_size> rows;
    uint64_t score = 0;
    for (size_t i = 0; i < batch_size; ++i) {
        rows[i] = {key(i), payload(i, 10, c.payload)};
        model.revision[i] = 10;
        model.alive[i] = 1;
        score += expected(i, 10);
    }
    connection->put_batch(rows);
    require(connection->read_batch(0) == score, "atomic batch data");
    connection.reset();
    engine.sync();
    verify(engine, c, model);
    write_oracle(c, model);
}
static void run(const Config &c) {
    auto model = read_oracle(c);
    auto engine = make_engine(c);
    const auto version = engine->version();
    const bool fixed = c.workload == "insert" || c.workload == "delete";
    const bool batch = c.workload == "get_many" || c.workload == "put_many";
    const bool read_only = c.workload == "get_copy" || c.workload == "get_borrow" ||
                           c.workload == "get_miss" || c.workload == "get_many";
    require(c.workload != "delete" || c.fixed_calls <= c.records,
            "delete count exceeds seeded rows");
    if (c.workload == "get_miss") {
        auto warmer = engine->connect();
        for (size_t i = 0; i < std::min<size_t>(4096, c.records); ++i)
            warmer->read(c.records + i, true, true);
    } else
        engine->warm();
    const auto sequence_start = *std::max_element(model.revision.begin(), model.revision.end());
    Gate gate;
    std::vector<Stats> stats(c.threads);
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < c.threads; ++thread)
        workers.emplace_back([&, thread] {
            struct Cleanup {
                Engine &engine;
                ~Cleanup() {
                    engine.thread_cleanup();
                }
            } cleanup{*engine};
            auto &s = stats[thread];
            s.latency.reserve(4096);
            try {
                auto connection = engine->connect();
                Random random{0x9e3779b97f4a7c15ull * (thread + 1)};
                uint64_t sequence = sequence_start;
                std::array<Row, batch_size> rows;
                auto operation = [&](uint64_t iteration) {
                    if (read_only) {
                        auto id = random.next() % c.records;
                        ++s.reads;
                        if (c.workload == "get_many")
                            return connection->read_batch((id / batch_size) * batch_size);
                        if (c.workload == "get_miss")
                            return connection->read(
                                c.records + id % std::min<size_t>(4096, c.records), true, true);
                        return connection->read(id, c.copy_reads());
                    }
                    if (c.workload == "mixed95" && random.next() % 20) {
                        ++s.reads;
                        return connection->read(random.next() % c.records, true);
                    }
                    ++s.writes;
                    if (c.workload == "insert") {
                        auto id = c.records + thread + iteration * c.threads;
                        auto value = payload(id, 1, c.payload);
                        connection->put(id, value);
                        model.revision[id] = 1;
                        model.alive[id] = 1;
                        return id;
                    }
                    if (c.workload == "delete") {
                        auto id = thread + iteration * c.threads;
                        connection->erase(id);
                        model.alive[id] = 0;
                        return id;
                    }
                    ++sequence;
                    if (c.workload == "put_many") {
                        auto blocks = c.records / (c.threads * batch_size);
                        auto first = (thread + ((sequence - 1) % blocks) * c.threads) * batch_size;
                        for (size_t i = 0; i < batch_size; ++i)
                            rows[i] = {key(first + i), payload(first + i, sequence, c.payload)};
                        connection->put_batch(rows);
                        for (size_t i = 0; i < batch_size; ++i)
                            model.revision[first + i] = sequence;
                        return first;
                    }
                    auto id = thread + ((sequence - 1) % (c.records / c.threads)) * c.threads;
                    if (c.workload == "patch8")
                        connection->patch(id, revision_bytes(sequence));
                    else
                        connection->put(id, payload(id, sequence, c.payload));
                    model.revision[id] = sequence;
                    return id;
                };
                {
                    std::unique_lock<std::mutex> lock(gate.mutex);
                    ++gate.ready;
                    gate.changed.notify_all();
                    gate.changed.wait(lock, [&] { return gate.phase >= 1 || gate.failed.load(); });
                }
                if (gate.failed.load())
                    return;
                if (!fixed) {
                    while (Clock::now() < gate.warm_end && !gate.failed.load()) {
                        s.checksum ^= operation(s.warm_calls);
                        ++s.warm_calls;
                    }
                }
                s.reads = s.writes = 0;
                s.checksum = 0;
                const auto retry_base = connection->retry_count();
                {
                    std::unique_lock<std::mutex> lock(gate.mutex);
                    ++gate.warm;
                    gate.changed.notify_all();
                    gate.changed.wait(lock, [&] { return gate.phase >= 2 || gate.failed.load(); });
                }
                if (gate.failed.load())
                    return;
                for (;;) {
                    if (gate.failed.load())
                        break;
                    if (fixed) {
                        if (s.calls >= c.fixed_calls / c.threads)
                            break;
                    } else if ((!read_only || (s.calls & 63) == 0) && Clock::now() >= gate.end)
                        break;
                    if ((s.calls & 63) == 0) {
                        auto begin = Clock::now();
                        s.checksum += operation(s.calls);
                        s.latency.push_back(
                            std::chrono::duration<double, std::micro>(Clock::now() - begin)
                                .count());
                    } else
                        s.checksum += operation(s.calls);
                    ++s.calls;
                }
                s.retries = connection->retry_count() - retry_base;
            } catch (...) {
                s.error = std::current_exception();
                gate.fail();
            }
        });
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.changed.wait(lock, [&] { return gate.ready == c.threads || gate.failed.load(); });
        gate.warm_end = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                           std::chrono::duration<double>(c.warmup_seconds));
        gate.phase = 1;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.warm == c.threads || gate.failed.load(); });
    }
    auto metrics_before = engine->metrics();
    std::cerr << "BENCH_TIMED_BEGIN\n" << std::flush;
    auto before = cpu_stats();
    auto start = Clock::now();
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.end = start + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>(c.seconds));
        gate.phase = 2;
        gate.changed.notify_all();
    }
    for (auto &thread : workers)
        thread.join();
    auto end = Clock::now();
    auto after = cpu_stats();
    std::cerr << "BENCH_TIMED_END\n" << std::flush;
    auto metrics_after = engine->metrics();
    for (auto &s : stats)
        if (s.error)
            std::rethrow_exception(s.error);
    uint64_t calls = 0, reads = 0, writes = 0, checksum = 0, warm = 0, retries = 0;
    std::vector<double> latency;
    for (auto &s : stats) {
        calls += s.calls;
        reads += s.reads;
        writes += s.writes;
        checksum += s.checksum;
        warm += s.warm_calls;
        retries += s.retries;
        latency.insert(latency.end(), s.latency.begin(), s.latency.end());
    }
    require(calls && !latency.empty(), "no measured operations");
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](double p) {
        return latency[static_cast<size_t>((latency.size() - 1) * p)];
    };
    double seconds = std::chrono::duration<double>(end - start).count();
    auto memory = number_file("/sys/fs/cgroup/memory.current"),
         peak = number_file("/sys/fs/cgroup/memory.peak");
    engine->sync();
    auto verified = verify(*engine, c, model);
    write_oracle(c, model);
    engine.reset();
    auto reopened = c;
    reopened.action = "verify";
    engine = make_engine(reopened);
    require(verify(*engine, reopened, model) == verified, "reopen verification");
    auto units = batch ? batch_size : 1;
    std::cout << std::fixed << std::setprecision(6) << "{\"ok\":true,\"engine\":\"" << c.engine
              << "\",\"mode\":\"" << c.mode << "\",\"workload\":\"" << c.workload
              << "\",\"version\":\"" << version << "\",\"threads\":" << c.threads
              << ",\"seed_records\":" << c.records << ",\"payload_bytes\":" << c.payload
              << ",\"calls\":" << calls << ",\"units_per_call\":" << units
              << ",\"records\":" << calls * units << ",\"reads\":" << reads
              << ",\"writes\":" << writes << ",\"seconds\":" << seconds
              << ",\"records_per_second\":" << calls * units / seconds
              << ",\"calls_per_second\":" << calls / seconds << ",\"p50_us\":" << percentile(.5)
              << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99)
              << ",\"latency_samples\":" << latency.size() << ",\"warm_calls\":" << warm
              << ",\"transaction_retries\":" << retries << ",\"checksum\":" << checksum
              << ",\"verified_keys\":" << verified
              << ",\"engine_metrics_before\":" << metrics_before
              << ",\"engine_metrics_after\":" << metrics_after
              << ",\"cgroup_cpu_usec\":" << after["usage_usec"] - before["usage_usec"]
              << ",\"cgroup_throttled_usec\":" << after["throttled_usec"] - before["throttled_usec"]
              << ",\"cgroup_memory_end\":" << memory
              << ",\"cgroup_memory_peak_cumulative\":" << peak << "}\n";
}
static Config arguments(int argc, char **argv) {
    Config c;
    for (int i = 1; i < argc; i += 2) {
        require(i + 1 < argc, "missing option value");
        std::string option = argv[i], value = argv[i + 1];
        if (option == "--engine")
            c.engine = value;
        else if (option == "--mode")
            c.mode = value;
        else if (option == "--workload")
            c.workload = value;
        else if (option == "--action")
            c.action = value;
        else if (option == "--data")
            c.directory = value;
        else if (option == "--threads")
            c.threads = std::stoul(value);
        else if (option == "--fq-group-max")
            c.fq_group_max = std::stoul(value);
        else if (option == "--records")
            c.records = std::stoull(value);
        else if (option == "--payload")
            c.payload = std::stoull(value);
        else if (option == "--fixed-calls")
            c.fixed_calls = std::stoull(value);
        else if (option == "--seconds")
            c.seconds = std::stod(value);
        else if (option == "--warmup-seconds")
            c.warmup_seconds = std::stod(value);
        else
            throw std::runtime_error("unknown argument " + option);
    }
    require(c.fq_group_max >= 1 && c.fq_group_max <= 256, "group bounds");
    require(c.threads >= 1 && c.threads <= 128 && (c.threads & (c.threads - 1)) == 0,
            "thread power-of-two bounds");
    require(c.records >= c.threads * batch_size && c.records % (c.threads * batch_size) == 0 &&
                c.records <= 16777216,
            "record partition bounds");
    require(c.payload >= 32 && c.payload <= 1048576 && c.seconds >= .1 && c.seconds <= 120 &&
                c.warmup_seconds >= 0 && c.warmup_seconds <= 10,
            "measurement bounds");
    require(c.fixed_calls >= c.threads && c.fixed_calls % c.threads == 0 &&
                c.fixed_calls <= 1048576,
            "fixed call bounds");
    require(c.mode == "sync" || c.mode == "buffered", "durability mode");
    require(c.action == "prepare" || c.action == "run" || c.action == "verify" ||
                c.action == "selftest",
            "action");
    const std::vector<std::string> workloads{"get_copy", "get_borrow", "get_miss", "get_many",
                                             "insert",   "update",     "delete",   "patch8",
                                             "put_many", "mixed95"};
    require(std::find(workloads.begin(), workloads.end(), c.workload) != workloads.end(),
            "workload");
    require(std::filesystem::path(c.directory).is_absolute(),
            "absolute benchmark data root required");
    return c;
}
} // namespace final_bench
int main(int argc, char **argv) {
    try {
        auto c = final_bench::arguments(argc, argv);
        if (c.action == "run")
            final_bench::run(c);
        else {
            std::filesystem::create_directories(c.directory);
            auto engine = final_bench::make_engine(c);
            if (c.action == "prepare")
                final_bench::prepare(*engine, c);
            else if (c.action == "selftest")
                final_bench::selftest(*engine, c);
            else {
                auto model = final_bench::read_oracle(c);
                final_bench::verify(*engine, c, model);
            }
            std::cout << "{\"ok\":true,\"action\":\"" << c.action << "\",\"engine\":\"" << c.engine
                      << "\"}\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "BENCH_ERROR " << error.what() << '\n';
        return 1;
    }
}
