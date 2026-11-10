#ifndef FENGQUDB_H
#define FENGQUDB_H
#include <stdint.h>
#if defined(_WIN32) && !defined(FQDB_STATIC)
#if defined(FQDB_BUILD)
#define FQDB_API __declspec(dllexport)
#else
#define FQDB_API __declspec(dllimport)
#endif
#elif defined(_WIN32)
#define FQDB_API
#else
#define FQDB_API __attribute__((visibility("default")))
#endif
#if defined(_WIN32)
#define FQDB_CALL __cdecl
#else
#define FQDB_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define FQDB_ABI_VERSION 2u
#define FQDB_MAX_KEY_BYTES 65536u
#define FQDB_MAX_VALUE_BYTES 16777216u
#define FQDB_MAX_TRANSACTION_BYTES 67108864u
typedef int32_t fqdb_status;
enum {
    FQDB_OK = 0,
    FQDB_NOT_FOUND = 1,
    FQDB_END = 2,
    FQDB_INVALID_ARGUMENT = 3,
    FQDB_IO_ERROR = 4,
    FQDB_CORRUPT = 5,
    FQDB_LOCKED = 6,
    FQDB_CONFLICT = 7,
    FQDB_NO_MEMORY = 8,
    FQDB_LIMIT = 9,
    FQDB_COMMIT_UNKNOWN = 10,
    FQDB_RECOVERY_REQUIRED = 11,
    FQDB_UNSUPPORTED = 12,
    FQDB_INTERNAL_ERROR = 13,
    FQDB_HISTORY_LOST = 14,
    FQDB_ALREADY_EXISTS = 15,
    FQDB_BUFFER_TOO_SMALL = 16,
    FQDB_TIMEOUT = 17,
    FQDB_NOT_MODIFIED = 18,
    FQDB_CANCELLED = 19,
    FQDB_CONSTRAINT = 20,
    FQDB_ASYNC_REQUIRED = 21
};
enum { FQDB_DURABILITY_SYNC = 0, FQDB_DURABILITY_BUFFERED = 1 };
enum { FQDB_PUT = 1, FQDB_DELETE = 2 };
typedef struct fqdb_slice {
    const void *data;
    uint64_t size;
} fqdb_slice;
/* Database-allocated memory; release exactly once with fqdb_buffer_free. */
typedef struct fqdb_buffer {
    void *data;
    uint64_t size;
} fqdb_buffer;
FQDB_API uint32_t FQDB_CALL fqdb_abi_version(void);
FQDB_API const char *FQDB_CALL fqdb_status_message(fqdb_status status);
FQDB_API void FQDB_CALL fqdb_buffer_free(fqdb_buffer *buffer);
FQDB_API fqdb_status FQDB_CALL fqdb_buffer_copy(fqdb_slice source, fqdb_buffer *out);

#ifdef __cplusplus
}
#endif
#endif
