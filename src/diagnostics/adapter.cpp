#include "fengqudb_diagnostics.h"
#include "api_support.hpp"
#include "ptpma_log.h"
#include <filesystem>
#include <memory>
using LocalLog = fengqudb::diagnostics::ptpma::LocalLog;
using Json = fengqudb::diagnostics::ptpma::Json;
struct fqdb_diagnostics {
    std::unique_ptr<LocalLog> log;
    fqdb_log_handle *registration = nullptr;
    bool reader_only = false;
};
namespace {
bool valid_directory_length(const std::filesystem::path &root) {
#ifdef _WIN32
    constexpr size_t path_limit = 240;
#else
    constexpr size_t path_limit = 960;
#endif
    // Reserve the longest internal name before LocalLog can create parents.
    if (root.native().size() >= path_limit - 56)
        return false;
    for (const auto &component : root.relative_path())
        if (component.native().size() > 255)
            return false;
    return true;
}
template <class F> fqdb_status boundary(F &&function) noexcept {
    return fengqudb::detail::status_boundary([&]() -> fqdb_status {
        try {
            return function();
        } catch (const nlohmann::json::exception &) {
            return FQDB_INVALID_ARGUMENT;
        } catch (const std::runtime_error &) {
            return FQDB_IO_ERROR;
        }
    });
}
fqdb_status output(const Json &json, fqdb_buffer *out) {
    auto text = json.dump(-1, ' ', false, Json::error_handler_t::replace);
    return fqdb_buffer_copy({text.data(), text.size()}, out);
}
Json arguments(fqdb_slice input) {
    if (input.size > 4096 || (input.size && !input.data))
        throw std::invalid_argument("invalid diagnostic arguments");
    if (!input.size)
        return Json::object();
    auto begin = static_cast<const char *>(input.data);
    return Json::parse(begin, begin + input.size, [](int depth, Json::parse_event_t, Json &) {
        if (depth > 8)
            throw std::invalid_argument("diagnostic depth limit");
        return true;
    });
}
const char *stage_name(uint32_t stage) noexcept {
    constexpr const char *names[] = {
        "arguments", "open",  "file_header", "record_header", "payload",  "trailer", "checksum",
        "index",     "write", "flush",       "publish",       "complete", "repair"};
    return stage < sizeof(names) / sizeof(*names) ? names[stage] : "unknown";
}
void FQDB_CALL record(void *user, const fqdb_log_event *event) {
    if (!event || event->abi_version != 1 || event->struct_size < sizeof(*event))
        return;
    auto state = static_cast<fqdb_diagnostics *>(user);
    const char *name = "database.unknown";
    switch (event->operation) {
    case FQDB_LOG_OPEN:
        name = event->repaired_bytes ? "database.recovery.truncated_tail"
               : event->status       ? "database.open.failed"
                                     : "database.open";
        break;
    case FQDB_LOG_COMMIT:
        name = event->status == FQDB_CONFLICT ? "database.transaction.conflict"
               : event->status                ? "database.commit.failed"
                                              : "database.commit.slow";
        break;
    case FQDB_LOG_SYNC:
        name = event->status ? "database.sync.failed" : "database.sync";
        break;
    case FQDB_LOG_CHECKPOINT:
        name = event->status ? "database.checkpoint.failed" : "database.checkpoint";
        break;
    case FQDB_LOG_CHANGES:
        name = "database.changes.failed";
        break;
    case FQDB_LOG_COMPACT:
        name = event->status ? "database.compact.failed" : "database.compact";
        break;
    case FQDB_LOG_BACKUP:
        name = event->status ? "database.backup.failed" : "database.backup";
        break;
    case FQDB_LOG_VERIFY:
        name = event->status ? "database.verify.failed" : "database.verify";
        break;
    case FQDB_LOG_RESTORE:
        name = event->status ? "database.restore.failed" : "database.restore";
        break;
    case FQDB_LOG_PURGE_EXPIRED:
        name = event->status ? "database.purge_expired.failed" : "database.purge_expired";
        break;
    case FQDB_LOG_INDEX_DEFINE:
        name = event->status ? "database.index.define.failed" : "database.index.define";
        break;
    case FQDB_LOG_INDEX_DROP:
        name = event->status ? "database.index.drop.failed" : "database.index.drop";
        break;
    case FQDB_LOG_BACKUP_INCREMENTAL:
        name = event->status ? "database.backup.incremental.failed" : "database.backup.incremental";
        break;
    case FQDB_LOG_RECOVER_CHAIN:
        name = event->status ? "database.recover.chain.failed" : "database.recover.chain";
        break;
    case FQDB_LOG_CONFIGURE:
        name = event->status ? "database.configure.failed" : "database.configure";
        break;
    }
    state->log->write(
        event->level == FQDB_LOG_ERROR     ? "error"
        : event->level == FQDB_LOG_WARNING ? "warning"
                                           : "info",
        name,
        {{"database_id", std::to_string(event->database_id)},
         {"run_id", std::to_string(event->database_id)},
         {"version", std::to_string(event->version)},
         {"base_version", std::to_string(event->base_version)},
         {"file_offset", std::to_string(event->file_offset)},
         {"bytes", std::to_string(event->payload_bytes)},
         {"count", std::to_string(event->operation_count)},
         {"error_code", std::to_string(event->status)},
         {"os_error", std::to_string(event->os_error)},
         {"stage", stage_name(event->stage)},
         {"duration_us", std::to_string(event->duration_us)},
         {"repaired_bytes", std::to_string(event->repaired_bytes)},
         {"durability", event->durability == FQDB_DURABILITY_SYNC ? "sync" : "buffered"}});
}
} // namespace
extern "C" {
fqdb_status FQDB_CALL fqdb_diagnostics_open(const char *directory,
                                            const fqdb_diagnostics_options *options,
                                            fqdb_diagnostics **out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!directory || !*directory ||
        (options &&
         (options->struct_size != sizeof(*options) || options->abi_version != 1 ||
          options->flags > FQDB_DIAGNOSTICS_READER_ONLY || options->min_level > FQDB_LOG_ERROR)))
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        auto state = std::make_unique<fqdb_diagnostics>();
        state->reader_only = options && options->flags == FQDB_DIAGNOSTICS_READER_ONLY;
        std::error_code path_error;
        auto root = std::filesystem::absolute(std::filesystem::u8path(directory)).lexically_normal();
        if (!valid_directory_length(root))
            return FQDB_LIMIT;
        root = std::filesystem::weakly_canonical(root, path_error);
        if (path_error)
            return FQDB_IO_ERROR;
        if (!valid_directory_length(root))
            return FQDB_LIMIT;
        state->log = std::make_unique<LocalLog>(root, "fengqudb", state->reader_only);
        if (!state->reader_only) {
            fqdb_log_options log{sizeof(log),
                                 1,
                                 options ? options->min_level
                                         : static_cast<uint32_t>(FQDB_LOG_WARNING),
                                 0,
                                 options ? options->slow_commit_us : 5000,
                                 record,
                                 state.get()};
            auto status = fqdb_log_open(&log, &state->registration);
            if (status)
                return status;
        }
        *out = state.release();
        return FQDB_OK;
    });
}
fqdb_status FQDB_CALL fqdb_diagnostics_close(fqdb_diagnostics *state) {
    if (!state)
        return FQDB_OK;
    auto result = fqdb_log_close(state->registration);
    if (result)
        return result;
    state->registration = nullptr;
    delete state;
    return FQDB_OK;
}
fqdb_status FQDB_CALL fqdb_diagnostics_status(fqdb_diagnostics *state, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {nullptr, 0};
    if (!state)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        auto json = state->log->status();
        fqdb_log_info info{};
        if (state->registration) {
            auto status = fqdb_log_get_info(state->registration, &info);
            if (status)
                return status;
        }
        json["core_events"] = info.delivered;
        json["callback_errors"] = info.callback_errors;
        return output(json, out);
    });
}
fqdb_status FQDB_CALL fqdb_diagnostics_query(fqdb_diagnostics *state, fqdb_slice args,
                                             fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {nullptr, 0};
    if (!state)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&] { return output(state->log->query(arguments(args)), out); });
}
fqdb_status FQDB_CALL fqdb_diagnostics_configure(fqdb_diagnostics *state, fqdb_slice args,
                                                 fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {nullptr, 0};
    if (!state)
        return FQDB_INVALID_ARGUMENT;
    if (state->reader_only)
        return FQDB_UNSUPPORTED;
    return boundary([&] { return output(state->log->configure(arguments(args)), out); });
}
fqdb_status FQDB_CALL fqdb_diagnostics_clear(fqdb_diagnostics *state, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {nullptr, 0};
    if (!state)
        return FQDB_INVALID_ARGUMENT;
    if (state->reader_only)
        return FQDB_UNSUPPORTED;
    return boundary([&] { return output(state->log->clear(), out); });
}
fqdb_status FQDB_CALL fqdb_diagnostics_flush(fqdb_diagnostics *state, uint32_t timeout_ms) {
    if (!state || timeout_ms > 60000)
        return FQDB_INVALID_ARGUMENT;
    return boundary([&]() -> fqdb_status {
        return state->log->flush(std::chrono::milliseconds(timeout_ms)) ? FQDB_OK : FQDB_TIMEOUT;
    });
}
}
