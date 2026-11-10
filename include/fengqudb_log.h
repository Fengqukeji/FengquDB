#ifndef FENGQUDB_LOG_H
#define FENGQUDB_LOG_H
#include "fengqudb.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { FQDB_LOG_INFO = 0, FQDB_LOG_WARNING = 1, FQDB_LOG_ERROR = 2 };
enum {
    FQDB_LOG_OPEN = 1,
    FQDB_LOG_COMMIT = 2,
    FQDB_LOG_SYNC = 3,
    FQDB_LOG_CHECKPOINT = 4,
    FQDB_LOG_CHANGES = 5,
    FQDB_LOG_COMPACT = 6,
    FQDB_LOG_BACKUP = 7,
    FQDB_LOG_VERIFY = 8,
    FQDB_LOG_RESTORE = 9,
    FQDB_LOG_PURGE_EXPIRED = 10,
    FQDB_LOG_INDEX_DEFINE = 11,
    FQDB_LOG_INDEX_DROP = 12,
    FQDB_LOG_BACKUP_INCREMENTAL = 13,
    FQDB_LOG_RECOVER_CHAIN = 14,
    FQDB_LOG_CONFIGURE = 16
};
enum {
    FQDB_STAGE_ARGUMENTS = 0,
    FQDB_STAGE_OPEN = 1,
    FQDB_STAGE_FILE_HEADER = 2,
    FQDB_STAGE_RECORD_HEADER = 3,
    FQDB_STAGE_PAYLOAD = 4,
    FQDB_STAGE_TRAILER = 5,
    FQDB_STAGE_CHECKSUM = 6,
    FQDB_STAGE_INDEX = 7,
    FQDB_STAGE_WRITE = 8,
    FQDB_STAGE_FLUSH = 9,
    FQDB_STAGE_PUBLISH = 10,
    FQDB_STAGE_COMPLETE = 11,
    FQDB_STAGE_REPAIR = 12
};
/* Numeric operation metadata only. Never includes keys, values or paths. */
typedef struct fqdb_log_event {
    uint32_t struct_size, abi_version, operation, level, stage;
    fqdb_status status;
    uint32_t durability, os_error;
    uint64_t database_id, version, base_version, file_offset, payload_bytes, operation_count,
        duration_us, repaired_bytes;
} fqdb_log_event;
typedef void(FQDB_CALL *fqdb_log_callback)(void *user, const fqdb_log_event *event);
typedef struct fqdb_log_options {
    uint32_t struct_size, abi_version, min_level, reserved;
    uint64_t slow_commit_us;
    fqdb_log_callback callback;
    void *user;
} fqdb_log_options;
typedef struct fqdb_log_info {
    uint64_t delivered, callback_errors, active;
} fqdb_log_info;
typedef struct fqdb_log_handle fqdb_log_handle;
/* One process-wide sink; a second registration returns LOCKED. Callback events
   are borrowed for the call. Callbacks run after database locks are released,
   may run concurrently and must be short. Nested logging is suppressed. */
FQDB_API fqdb_status FQDB_CALL fqdb_log_open(const fqdb_log_options *options,
                                             fqdb_log_handle **out);
FQDB_API fqdb_status FQDB_CALL fqdb_log_get_info(fqdb_log_handle *handle, fqdb_log_info *out);
/* Unregister and wait for callbacks to finish. user remains owned by the caller
   and can be released after success. Closing from that sink's callback is rejected.
   Destruction must not race other operations on this handle. */
FQDB_API fqdb_status FQDB_CALL fqdb_log_close(fqdb_log_handle *handle);
#ifdef __cplusplus
}
#endif
#endif
