// Same byte-copy/reused-buffer/batched-copy semantics as the business examples.
#include "fengqudb_store.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
static void ok(fqdb_status status) {
    if (status != FQDB_OK)
        throw std::runtime_error(fqdb_status_message(status));
}
static void check(bool condition) {
    if (!condition)
        throw std::runtime_error("binding benchmark check failed");
}
static fqdb_slice slice(const std::string &value) {
    return {value.data(), value.size()};
}
static uint64_t score(fqdb_slice value) {
    check(value.size != 0 && value.data);
    auto bytes = static_cast<const unsigned char *>(value.data);
    return bytes[0] + bytes[value.size - 1];
}
template <class F> static void measure(const char *name, size_t count, size_t units, F operation) {
    uint64_t checksum = 0;
    for (size_t i = 0; i < std::min<size_t>(count, 10000); ++i)
        checksum ^= operation();
    std::vector<double> samples;
    samples.reserve((count + 63) / 64);
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i) {
        if ((i & 63) == 0) {
            auto before = std::chrono::steady_clock::now();
            checksum += operation();
            samples.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before)
                    .count());
        } else
            checksum += operation();
    }
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"language\":\"native\",\"workload\":\"" << name << "\",\"calls\":" << count
              << ",\"units_per_call\":" << units
              << ",\"ops_per_second\":" << count * units / seconds
              << ",\"p99_us\":" << samples[(samples.size() - 1) * 99 / 100]
              << ",\"checksum\":" << checksum << "}\n";
}
static int run(const std::vector<std::string> &args) {
    try {
        check(args.size() == 4);
        auto count = std::stoull(args[3]);
        check(count >= 10000 && count <= 10000000);
        std::ifstream input(args[2], std::ios::binary);
        check(bool(input));
        const std::string payload{std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()};
        check(!payload.empty() && payload.size() <= FQDB_MAX_VALUE_BYTES);
        fqdb_store *raw = nullptr;
        ok(fqdb_store_open_at((args[1] + "/db").c_str(), "main", nullptr, &raw));
        std::unique_ptr<fqdb_store, decltype(&fqdb_store_close)> db(raw, fqdb_store_close);
        std::vector<std::string> names;
        for (unsigned i = 0; i < 32; ++i)
            names.push_back("bench/" + std::to_string(i));
        std::vector<fqdb_slice> keys;
        for (const auto &name : names)
            keys.push_back(slice(name));
        fqdb_store_txn *pending = nullptr;
        ok(fqdb_store_txn_begin(raw, 0, &pending));
        std::unique_ptr<fqdb_store_txn, decltype(&fqdb_store_txn_close)> tx(pending,
                                                                            fqdb_store_txn_close);
        for (auto key : keys)
            ok(fqdb_store_txn_put(pending, key, slice(payload), 0));
        uint64_t version = 0;
        ok(fqdb_store_txn_commit(pending, &version));
        tx.reset();
        fqdb_store_key *prepared = nullptr;
        ok(fqdb_store_key_prepare(raw, keys[0], &prepared));
        std::unique_ptr<fqdb_store_key, decltype(&fqdb_store_key_close)> key(prepared,
                                                                             fqdb_store_key_close);
        std::vector<unsigned char> reuse(payload.size());
        measure("get_copy", count, 1, [&] {
            fqdb_buffer value{};
            ok(fqdb_store_get(raw, keys[0], &value));
            auto result = score({value.data, value.size});
            fqdb_buffer_free(&value);
            return result;
        });
        measure("prepared_get_into", count, 1, [&] {
            uint64_t required = 0;
            ok(fqdb_store_key_get_into(prepared, reuse.data(), reuse.size(), &required));
            check(required == payload.size());
            return score({reuse.data(), required});
        });
        measure("get_many_32", std::max<uint64_t>(8192, count / 16), 32, [&] {
            fqdb_store_results *raw_rows = nullptr;
            ok(fqdb_store_get_many(raw, keys.data(), static_cast<uint32_t>(keys.size()),
                                   &raw_rows));
            std::unique_ptr<fqdb_store_results, decltype(&fqdb_store_results_close)> rows(
                raw_rows, fqdb_store_results_close);
            const auto values = fqdb_store_results_data(raw_rows);
            struct OwnedRows {
                std::array<fqdb_buffer, 32> values{};
                ~OwnedRows() {
                    for (auto &value : values)
                        fqdb_buffer_free(&value);
                }
            } owned;
            uint64_t result = 0;
            for (size_t i = 0; i < keys.size(); ++i) {
                check(values[i].found != 0);
                ok(fqdb_buffer_copy(values[i].value, &owned.values[i]));
            }
            // Retain all 32 owned copies together, as managed byte[][]/lists do.
            for (const auto &value : owned.values)
                result += score({value.data, value.size});
            return result;
        });
        for (unsigned width : {1u, 32u}) {
            fqdb_store_reader *reader = nullptr;
            ok(fqdb_store_reader_create(raw, keys.data(), width, &reader));
            std::unique_ptr<fqdb_store_reader, decltype(&fqdb_store_reader_close)> owner(reader, fqdb_store_reader_close);
            measure(width == 1 ? "shared_reader" : "shared_reader_32", width == 1 ? count : std::max<uint64_t>(8192, count/16), width, [&] {
                const fqdb_store_result_row *rows = nullptr;
                uint32_t size = 0;
                ok(fqdb_store_reader_read(reader, &rows, &size));
                check(size == width);
                uint64_t result = 0;
                for (unsigned i = 0; i < size; ++i) {
                    check(rows[i].found != 0);
                    result += score(rows[i].value);
                }
                return result;
            });
        }
        for (auto name : keys) {
            fqdb_buffer value{};
            ok(fqdb_store_get(raw, name, &value));
            bool equal =
                std::string_view(static_cast<const char *>(value.data), value.size) == payload;
            fqdb_buffer_free(&value);
            check(equal);
        }
        fqdb_store_maintenance_info info{};
        ok(fqdb_store_verify(raw, nullptr, &info));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int count, wchar_t **values) {
    std::vector<std::string> args;
    for (int i = 0; i < count; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[i], -1, nullptr, 0,
                                       nullptr, nullptr);
        check(size > 0);
        std::string value(size, '\0');
        check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[i], -1, value.data(), size,
                                  nullptr, nullptr) != 0);
        value.pop_back();
        args.push_back(std::move(value));
    }
    return run(args);
}
#else
int main(int count, char **values) {
    return run(std::vector<std::string>(values, values + count));
}
#endif
