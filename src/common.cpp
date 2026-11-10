#include "fengqudb.h"
#include <cstdlib>
#include <cstring>

uint32_t FQDB_CALL fqdb_abi_version(void) {
    return FQDB_ABI_VERSION;
}
const char *FQDB_CALL fqdb_status_message(fqdb_status s) {
    switch (s) {
    case FQDB_CANCELLED:
        return "cancelled";
    case FQDB_OK:
        return "ok";
    case FQDB_NOT_FOUND:
        return "not found";
    case FQDB_END:
        return "end";
    case FQDB_INVALID_ARGUMENT:
        return "invalid argument";
    case FQDB_IO_ERROR:
        return "I/O error";
    case FQDB_CORRUPT:
        return "corrupt database";
    case FQDB_LOCKED:
        return "database locked";
    case FQDB_CONFLICT:
        return "transaction conflict";
    case FQDB_NO_MEMORY:
        return "out of memory";
    case FQDB_LIMIT:
        return "size/version limit";
    case FQDB_COMMIT_UNKNOWN:
        return "commit outcome unknown; reopen and reconcile";
    case FQDB_RECOVERY_REQUIRED:
        return "database requires reopen";
    case FQDB_UNSUPPORTED:
        return "unsupported version/options";
    case FQDB_ASYNC_REQUIRED:
        return "periodic writes require submit/poll; disable write scheduling for synchronous commit";
    case FQDB_HISTORY_LOST:
        return "change cursor expired; rebuild from a snapshot";
    case FQDB_ALREADY_EXISTS:
        return "destination already exists";
    case FQDB_BUFFER_TOO_SMALL:
        return "destination buffer too small";
    case FQDB_CONSTRAINT:
        return "constraint violation";
    case FQDB_TIMEOUT:
        return "operation timed out";
    case FQDB_NOT_MODIFIED:
        return "not modified";
    default:
        return "internal/unknown error";
    }
}
void FQDB_CALL fqdb_buffer_free(fqdb_buffer *out) {
    if (out) {
        std::free(out->data);
        *out = {nullptr, 0};
    }
}
fqdb_status FQDB_CALL fqdb_buffer_copy(fqdb_slice source, fqdb_buffer *out) {
    if (!out)
        return FQDB_INVALID_ARGUMENT;
    *out = {nullptr, 0};
    if (source.size > FQDB_MAX_VALUE_BYTES || (source.size && !source.data))
        return FQDB_INVALID_ARGUMENT;
    if (!source.size)
        return FQDB_OK;
    auto data = std::malloc(static_cast<size_t>(source.size));
    if (!data)
        return FQDB_NO_MEMORY;
    std::memcpy(data, source.data, static_cast<size_t>(source.size));
    *out = {data, source.size};
    return FQDB_OK;
}
