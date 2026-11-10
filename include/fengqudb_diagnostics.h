#ifndef FENGQUDB_DIAGNOSTICS_H
#define FENGQUDB_DIAGNOSTICS_H
#include "fengqudb_log.h"
#if defined(_WIN32) && !defined(FQDB_DIAGNOSTICS_STATIC)
#ifdef FQDB_DIAGNOSTICS_BUILD
#define FQDB_DIAG_API __declspec(dllexport)
#else
#define FQDB_DIAG_API __declspec(dllimport)
#endif
#elif defined(_WIN32)
#define FQDB_DIAG_API
#else
#define FQDB_DIAG_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { FQDB_DIAGNOSTICS_READER_ONLY = 1 };
typedef struct fqdb_diagnostics fqdb_diagnostics;
typedef struct fqdb_diagnostics_options {
    uint32_t struct_size, abi_version, min_level, flags;
    uint64_t slow_commit_us;
} fqdb_diagnostics_options;
/* Optional PTpma-backed asynchronous JSONL logger. NULL options selects warnings,
   errors and commits >=5ms. Reader-only instances never register a core sink.
   UTF-8 directory is anchored at open. Paths must leave 56 native units for
   internal names below 240 UTF-16 units (Windows) or 960 bytes (POSIX), with
   each component <=255 native units. Excess returns LIMIT before creation. */
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_open(const char *directory,
                                                          const fqdb_diagnostics_options *options,
                                                          fqdb_diagnostics **out);
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_close(fqdb_diagnostics *handle);
/* JSON output uses the CORE allocator: release with fqdb_buffer_free. */
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_status(fqdb_diagnostics *handle,
                                                            fqdb_buffer *out);
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_query(fqdb_diagnostics *handle,
                                                           fqdb_slice arguments_json,
                                                           fqdb_buffer *out);
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_configure(fqdb_diagnostics *handle,
                                                               fqdb_slice settings_json,
                                                               fqdb_buffer *out);
/* Explicitly removes this component's logs only; never called by close(). */
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_clear(fqdb_diagnostics *handle,
                                                           fqdb_buffer *out);
/* Settles the captured queue watermark, not a power-loss durability guarantee.
   Inspect status().dropped/io_errors even when this returns OK. */
FQDB_DIAG_API fqdb_status FQDB_CALL fqdb_diagnostics_flush(fqdb_diagnostics *handle,
                                                           uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
