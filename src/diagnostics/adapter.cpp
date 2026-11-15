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
    bool reader_only = false, full_trace = false;
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
void FQDB_CALL record(void *user, const fqdb_log_event *event) {
    if (!event || event->abi_version != 1 || event->struct_size < sizeof(*event)) return;
    auto state = static_cast<fqdb_diagnostics *>(user);
    state->log->write(*event, !state->full_trace);
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
        state->full_trace = options && options->slow_commit_us == 0;
        state->reader_only = options && options->flags == FQDB_DIAGNOSTICS_READER_ONLY;
        std::u8string utf8(directory, directory + std::char_traits<char>::length(directory));
        std::error_code path_error;
        auto root = std::filesystem::absolute(std::filesystem::path(utf8)).lexically_normal();
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
