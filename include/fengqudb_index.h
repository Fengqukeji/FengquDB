#ifndef FENGQUDB_INDEX_H
#define FENGQUDB_INDEX_H
/* Optional native retrieval module. Request/result wire format: proto/index.proto.
   Callers own the Store and serialize transaction operations. Snapshot reads may
   run concurrently. All returned buffers are freed with fqdb_buffer_free(). */
#include "fengqudb_store.h"
#if defined(_WIN32) && !defined(FQIDX_STATIC)
#if defined(FQIDX_BUILD)
#define FQIDX_API __declspec(dllexport)
#else
#define FQIDX_API __declspec(dllimport)
#endif
#elif defined(_WIN32)
#define FQIDX_API
#else
#define FQIDX_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define FQIDX_ABI_VERSION 1u
enum { FQIDX_APPLY_UPSERT = 1, FQIDX_APPLY_PATCH = 2, FQIDX_APPLY_ERASE = 3 };
enum { FQIDX_QUERY_SEARCH = 1, FQIDX_QUERY_GET = 2, FQIDX_QUERY_STATS = 3, FQIDX_QUERY_LIST = 4 };
enum { FQIDX_LEXICAL = 0, FQIDX_DENSE_EXACT = 1, FQIDX_DENSE_ANN = 2 };

/* HEADER keeps identity, revisions, metadata, source locator and payload. */
enum { FQIDX_PROJECT_FULL = 0, FQIDX_PROJECT_HEADER = 1 };
#define FQIDX_DEFAULT_OUTPUT_BYTES 67108864ull
enum {
    FQIDX_FINISHED = 0,
    FQIDX_POSTING_LIMIT = 1,
    FQIDX_CANDIDATE_LIMIT = 2,
    FQIDX_SCAN_LIMIT = 3,
    FQIDX_MEMORY_LIMIT = 4,
    FQIDX_DEADLINE = 5,
    FQIDX_CANCELLED = 6,
    FQIDX_TERM_LIMIT = 7
};
enum { FQIDX_MATCH_UNKNOWN = 0, FQIDX_HAS_HITS = 1, FQIDX_PROVEN_NONE = 2 };
typedef uint32_t(FQDB_CALL *fqidx_cancel_fn)(void *context);
typedef struct fqidx_query_control {
    uint32_t struct_size, abi_version;
    void *context;
    fqidx_cancel_fn cancelled;
} fqidx_query_control;
FQIDX_API uint32_t FQDB_CALL fqidx_abi_version(void);
/* Compare to the address of fqdb_store_bridge_abi_version in the caller's Core.
   Modules linked to different Core images must not exchange opaque handles. */
FQIDX_API uintptr_t FQDB_CALL fqidx_core_bridge_address(void);
/* Nonzero expected_store_token must match the native lease identity. Any apply
   failure cancels the WHOLE supplied transaction, including earlier domain writes.
   Index never commits, opens another Store, or alters durability configuration. */
FQIDX_API fqdb_status FQDB_CALL fqidx_txn_apply(fqdb_store_txn *txn, uint64_t expected_store_token,
                                                fqdb_slice request);
FQIDX_API fqdb_status FQDB_CALL fqidx_snapshot_query(const fqdb_store_snapshot *snapshot,
                                                     uint64_t expected_store_token,
                                                     fqdb_slice request,
                                                     const fqidx_query_control *control,
                                                     fqdb_buffer *out);
FQIDX_API fqdb_status FQDB_CALL fqidx_txn_query(fqdb_store_txn *txn, uint64_t expected_store_token,
                                                fqdb_slice request,
                                                const fqidx_query_control *control,
                                                fqdb_buffer *out);
/* Bounded, caller-committed maintenance. Any failure cancels the entire txn. */
FQIDX_API fqdb_status FQDB_CALL fqidx_txn_maintain(fqdb_store_txn *txn,
                                                   uint64_t expected_store_token,
                                                   fqdb_slice request,
                                                   const fqidx_query_control *control,
                                                   fqdb_buffer *out);
#ifdef __cplusplus
}
#endif
#endif
