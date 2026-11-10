#include "fengqudb_store.hpp"
#include <charconv>
#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
static void maintenance_json(const fengqudb::StoreMaintenanceInfo &info) {
    std::cout << "{\"status\":0,\"snapshot_version\":" << info.snapshot_version
              << ",\"final_version\":" << info.final_version
              << ",\"copied_keys\":" << info.copied_keys << ",\"stored_keys\":" << info.stored_keys
              << ",\"stored_value_bytes\":" << info.stored_value_bytes << "}\n";
}
static uint64_t number(const std::string &text) {
    uint64_t value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid number");
    return value;
}
static int extended(const std::vector<std::string> &args) {
    const auto &op = args[1];
    try {
        fengqudb::StoreMaintenanceInfo info{};
        if (op == "recover-chain") {
            if (args.size() < 4)
                return 2;
            uint64_t version = 0, time = 0;
            std::vector<std::string> archives;
            for (size_t i = 4; i < args.size(); ++i) {
                if (args[i] == "--target-version" || args[i] == "--target-time-ms") {
                    auto option = args[i];
                    if (++i == args.size())
                        return 2;
                    (option == "--target-version" ? version : time) = number(args[i]);
                } else
                    archives.push_back(args[i]);
            }
            info = fengqudb::Store::recover_chain(args[2], archives, args[3], version, time);
        } else {
            if (args.size() < 3)
                return 2;
            fengqudb::StoreOptions options;
            options.create = false;
            fengqudb::Store store(args[2], options);
            if (op == "backup-incremental") {
                if (args.size() != 5)
                    return 2;
                info = store.backup_incremental(args[3], number(args[4]));
            } else if (op == "index-drop") {
                if (args.size() != 4)
                    return 2;
                info = store.drop_index(args[3]);
            } else if (op == "index-define") {
                if (args.size() != 8)
                    return 2;
                auto field = number(args[5]), type = number(args[6]), flags = number(args[7]);
                if (field > UINT32_MAX || type > UINT32_MAX || flags > UINT32_MAX)
                    return 2;
                info =
                    store.define_index(args[3], args[4], static_cast<uint32_t>(field),
                                       static_cast<uint32_t>(type), static_cast<uint32_t>(flags));
            } else
                return 2;
        }
        maintenance_json(info);
        return 0;
    } catch (const fengqudb::Error &error) {
        std::cerr << "{\"status\":" << error.code() << ",\"message\":\""
                  << fqdb_status_message(error.code()) << "\"}\n";
        return 1;
    } catch (const std::invalid_argument &) {
        return 2;
    } catch (const std::exception &) {
        return 1;
    }
}
static int run(const std::vector<std::string> &args) {
    if (args.size() < 3) {
        std::cerr << "usage: fqdb_store_cli verify|compact|backup|restore|purge-expired SOURCE "
                     "[DESTINATION] "
                     "[--timeout-ms N] [--barrier-bytes N] [--catchup-rounds N] [--max-keys N] "
                     "[--batch-bytes N]\n";
        return 2;
    }
    const auto &operation = args[1];
    if (operation == "recover-chain" ||
        operation == "backup-incremental" || operation == "index-define" ||
        operation == "index-drop")
        return extended(args);
    bool destination = operation == "backup" || operation == "restore";
    if (!(destination || operation == "verify" || operation == "compact" ||
          operation == "purge-expired") ||
        (destination && args.size() < 4))
        return 2;
    fengqudb::StoreMaintenanceOptions options;
    fengqudb::StoreExpiryOptions expiry;
    for (size_t i = destination ? 4 : 3; i < args.size(); i += 2) {
        if (i + 1 == args.size())
            return 2;
        uint64_t value = 0;
        const auto &text = args[i + 1];
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return 2;
        if (args[i] == "--timeout-ms")
            options.timeout_ms = expiry.timeout_ms = value;
        else if (operation != "purge-expired" && args[i] == "--barrier-bytes")
            options.max_barrier_bytes = value;
        else if (operation != "purge-expired" && args[i] == "--catchup-rounds" && value <= 64)
            options.max_catchup_rounds = static_cast<uint32_t>(value);
        else if (operation == "purge-expired" && args[i] == "--max-keys" && value && value <= 65536)
            expiry.max_keys = static_cast<uint32_t>(value);
        else if (operation == "purge-expired" && args[i] == "--batch-bytes")
            expiry.max_batch_bytes = value;
        else
            return 2;
    }
    try {
        fengqudb::StoreMaintenanceInfo info{};
        if (operation == "restore")
            info = fengqudb::Store::restore(args[2], args[3], options);
        else {
            fengqudb::StoreOptions open;
            open.create = false;
            fengqudb::Store store(args[2], open);
            if (operation == "purge-expired") {
                auto result = store.purge_expired(expiry);
                std::cout << "{\"status\":0,\"version\":" << result.version
                          << ",\"cutoff_ms\":" << result.cutoff_ms
                          << ",\"removed_keys\":" << result.removed_keys
                          << ",\"removed_value_bytes\":" << result.removed_value_bytes
                          << ",\"visited_pages\":" << result.visited_pages
                          << ",\"more\":" << result.more << "}\n";
                return 0;
            }
            if (operation == "verify")
                info = store.verify(options);
            else if (operation == "compact")
                info = store.compact(options);
            else
                info = store.backup(args[3], options);
        }
        std::cout << "{\"status\":0,\"snapshot_version\":" << info.snapshot_version
                  << ",\"final_version\":" << info.final_version
                  << ",\"source_file_bytes\":" << info.source_file_bytes
                  << ",\"output_file_bytes\":" << info.output_file_bytes
                  << ",\"stored_keys\":" << info.stored_keys
                  << ",\"stored_value_bytes\":" << info.stored_value_bytes
                  << ",\"copied_keys\":" << info.copied_keys
                  << ",\"catchup_rounds\":" << info.catchup_rounds << "}\n";
        return 0;
    } catch (const fengqudb::Error &error) {
        std::cerr << "{\"status\":" << error.code() << ",\"message\":\""
                  << fqdb_status_message(error.code()) << "\"}\n";
        return 1;
    } catch (const std::exception &) {
        std::cerr << "{\"status\":13,\"message\":\"tool failure\"}\n";
        return 1;
    }
}
#ifdef _WIN32
int wmain(int count, wchar_t **values) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[i], -1, nullptr, 0,
                                           nullptr, nullptr);
            if (!size)
                return 2;
            std::string value(static_cast<size_t>(size), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[i], -1, value.data(),
                                     size, nullptr, nullptr))
                return 2;
            value.pop_back();
            args.push_back(std::move(value));
        }
        return run(args);
    } catch (...) {
        return 1;
    }
}
#else
int main(int count, char **values) {
    return run(std::vector<std::string>(values, values + count));
}
#endif
