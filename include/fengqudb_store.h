#ifndef FENGQUDB_STORE_H
#define FENGQUDB_STORE_H
#include "fengqudb.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Store is the process-local paged database. Its path is a directory. */
typedef struct fqdb_store fqdb_store;
typedef struct fqdb_store_txn fqdb_store_txn;
typedef struct fqdb_store_snapshot fqdb_store_snapshot;
typedef struct fqdb_store_value fqdb_store_value;
typedef struct fqdb_store_cursor fqdb_store_cursor;
typedef struct fqdb_store_key fqdb_store_key;
typedef struct fqdb_store_batch fqdb_store_batch;
typedef struct fqdb_store_results fqdb_store_results;
typedef struct fqdb_store_reader fqdb_store_reader;
typedef struct fqdb_store_watch fqdb_store_watch;
typedef struct fqdb_store_change_batch fqdb_store_change_batch;
typedef struct fqdb_store_commit fqdb_store_commit;
/* Optional native-handle bridge, ABI 1; independent of the core ABI.
   Initialize struct_size and abi_version=1 before calling. kind is 1 Store,
   2 active transaction, 3 snapshot. store_token identifies one Core lifetime
   in this loaded Core runtime (never an object address); use the SAME runtime's
   function table for borrowed handles. uuid is the persistent Store generation
   UUID (stable across compaction/reopen, different for a restored copy).
   snapshot_version is the current published version for Store, otherwise the
   captured base version. Identity neither transfers nor extends handle ownership.
   Transactions follow normal ready/cancel/timeout/sealed checks. */
#define FQDB_STORE_BRIDGE_ABI_VERSION 1u
typedef struct fqdb_store_bridge_info {
    uint32_t struct_size, abi_version, kind, reserved;
    uint64_t store_token, snapshot_version;
    unsigned char uuid[16];
} fqdb_store_bridge_info;
FQDB_API uint32_t FQDB_CALL fqdb_store_bridge_abi_version(void);
FQDB_API fqdb_status FQDB_CALL fqdb_store_bridge_identity(
    fqdb_store *store, fqdb_store_bridge_info *info);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_bridge_identity(
    fqdb_store_txn *txn, fqdb_store_bridge_info *info);
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_bridge_identity(
    const fqdb_store_snapshot *snapshot, fqdb_store_bridge_info *info);
/* Persisted per directory. NULL resets AND saves performance/50 ms/1 MiB.
   Performance commits are immediately visible in memory; only their barrier
   is periodic. Reliable mode and force_reliable=1 wait for durable completion.
   Explicit low-level BUFFERED stores remain manual-sync stores. ABI 2. */
enum { FQDB_WRITE_RELIABLE = 0, FQDB_WRITE_PERFORMANCE = 1 };
#ifdef __cplusplus
#define FQDB_WRITE_ARGUMENT uint32_t force_reliable = 0
#else
#define FQDB_WRITE_ARGUMENT uint32_t force_reliable
#endif
typedef struct fqdb_store_write_options {
    uint32_t struct_size, abi_version, mode, period_ms;
    uint64_t threshold_bytes;
} fqdb_store_write_options;
FQDB_API fqdb_status FQDB_CALL fqdb_store_write_configure(fqdb_store *store,
                                                          const fqdb_store_write_options *options);
FQDB_API fqdb_status FQDB_CALL fqdb_store_write_options_get(fqdb_store *store,
                                                            fqdb_store_write_options *out);
/* Owned deferred blind put. Admission is not completion. The receipt completes
   after memory publication in performance mode, after sync in reliable mode
   or with force_reliable=1. All input is copied before admission returns. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_put_submit(fqdb_store *store, fqdb_slice key,
                                                     fqdb_slice value, uint64_t expires_at_ms,
                                                     fqdb_store_commit **out, FQDB_WRITE_ARGUMENT);
/* Consumes the transaction's staged state on successful admission, preserving
   its original snapshot/dependencies. The original handle may then be closed. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_submit(fqdb_store_txn *txn, fqdb_store_commit **out,
                                                     FQDB_WRITE_ARGUMENT);
/* timeout_ms=0 waits indefinitely; poll never waits. completion_status is the
   actual commit outcome, distinct from the wait API's TIMEOUT/NOT_MODIFIED.
   Closing a pending receipt waits for completion to preserve native ownership. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_commit_wait(fqdb_store_commit *commit,
                                                      uint64_t timeout_ms,
                                                      fqdb_status *completion_status,
                                                      uint64_t *version);
FQDB_API fqdb_status FQDB_CALL fqdb_store_commit_poll(fqdb_store_commit *commit,
                                                      fqdb_status *completion_status,
                                                      uint64_t *version);
FQDB_API void FQDB_CALL fqdb_store_commit_cancel(fqdb_store_commit *commit);
FQDB_API void FQDB_CALL fqdb_store_commit_close(fqdb_store_commit *commit);
struct fqdb_store_maintenance_options;
struct fqdb_store_maintenance_info;
typedef struct fqdb_store_runtime_options {
    uint32_t struct_size, abi_version, background, interval_ms;
    uint32_t expiry_keys, group_max, group_delay_us, garbage_percent;
    uint64_t compact_min_bytes, compact_growth_bytes, job_timeout_ms;
    /* ABI 1 uses the original 56-byte prefix; ABI 2 also bounds aggregate
       read/write request bytes of one background worker iteration. */
    uint64_t job_max_read_bytes, job_max_write_bytes;
} fqdb_store_runtime_options;
typedef struct fqdb_store_runtime_info {
    uint64_t running, last_status, expiry_batches, compactions, group_flushes, group_transactions;
} fqdb_store_runtime_info;
/* NULL resets to background disabled, 200 ms checks, 32 MiB minimum/growth,
   opportunistic groups of at most 128, zero intentional group delay.
   Background policy is process-local; explicit settings take precedence. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_configure(fqdb_store *store,
                                                    const fqdb_store_runtime_options *options);
FQDB_API fqdb_status FQDB_CALL fqdb_store_runtime_info_get(fqdb_store *store,
                                                           fqdb_store_runtime_info *out);
enum {
    FQDB_HASH_GET = 1,
    FQDB_HASH_SET = 2,
    FQDB_HASH_DELETE = 3,
    FQDB_HASH_SCAN = 4,
    FQDB_SET_CONTAINS = 10,
    FQDB_SET_ADD = 11,
    FQDB_SET_REMOVE = 12,
    FQDB_SET_SCAN = 13,
    FQDB_LIST_PUSH_LEFT = 20,
    FQDB_LIST_PUSH_RIGHT = 21,
    FQDB_LIST_POP_LEFT = 22,
    FQDB_LIST_POP_RIGHT = 23,
    FQDB_LIST_GET = 24,
    FQDB_LIST_SET = 25,
    FQDB_LIST_RANGE = 26,
    FQDB_SORTED_ADD = 30,
    FQDB_SORTED_REMOVE = 31,
    FQDB_SORTED_SCORE = 32,
    FQDB_SORTED_RANGE = 33,
    FQDB_COLLECTION_SIZE = 40,
    FQDB_COLLECTION_CLEAR = 41,
    FQDB_INDEX_LOOKUP = 50,
    FQDB_INDEX_QUERY = 51,
    FQDB_RECORD_PROJECT = 52,
    FQDB_SELECT = 53,
    FQDB_SELECT_EXPLAIN = 54
};
/* Request protobuf: 1 operation(varint), 2 scope(bytes), 3 key(bytes),
   4 value(bytes), 5 signed argument(fixed64 two's complement), 6 score(fixed64
   IEEE double), 7 limit(fixed64, default 256), 8 flags(varint).
   Response: 1 value(bytes, present even if empty), 2 count(fixed64),
   3 changed(varint), repeated 4 row(message: 1 key,2 value,3 double score),
   5 opaque continuation(bytes). Scan/lookup passes this token in request key;
   an encoded cursor may exceed the 64 KiB business-key limit (max 263168 bytes).
   flags are reserved and must be zero.
   Stages one operation in the existing transaction; caller commits explicitly.
   Allocate/read the result BEFORE committing to avoid post-commit OOM ambiguity. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_execute(fqdb_store_txn *txn, fqdb_slice request,
                                                      fqdb_buffer *out);
/* Read-only FQDB_SELECT / FQDB_SELECT_EXPLAIN on a fresh immutable snapshot.
   Same request/response encoding as fqdb_store_txn_execute; creates no
   committable transaction and records no conflict dependencies. Use
   fqdb_store_txn_execute when the query must see private writes or join a
   transaction's conflict checks. Any other operation is INVALID_ARGUMENT. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_select(fqdb_store *store, fqdb_slice request,
                                                 fqdb_buffer *out);
/* Scalar top-level protobuf fields: use the type numbers documented below.
   flags: 1 unique,2 required. Existing data is built in bounded online batches;
   failure leaves a BUILDING definition, resumable with the same arguments.
   Drop removes the definition before reclaiming its hidden entries. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_index_define(
    fqdb_store *store, fqdb_slice name, fqdb_slice key_prefix, uint32_t field, uint32_t type,
    uint32_t flags, const struct fqdb_store_maintenance_options *options,
    struct fqdb_store_maintenance_info *out);
typedef struct fqdb_store_index_field {
    uint32_t field, type;
} fqdb_store_index_field;
/* Ordered 1..8 distinct top-level fields. Type: 1 uint64,2 int64,3 double,
   4 bytes,5 sint64,6 float,7 fixed64,8 sfixed64,9 fixed32,10 sfixed32,11 bool (0/1).
   Unique/required flags apply to the complete tuple; any missing optional
   component omits that record from the index. Combined encoded tuple <=64KiB. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_index_define_compound(
    fqdb_store *store, fqdb_slice name, fqdb_slice key_prefix, const fqdb_store_index_field *fields,
    uint32_t count, uint32_t flags, const struct fqdb_store_maintenance_options *options,
    struct fqdb_store_maintenance_info *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_index_drop(
    fqdb_store *store, fqdb_slice name, const struct fqdb_store_maintenance_options *options,
    struct fqdb_store_maintenance_info *out);
enum { FQDB_STORE_CREATE = 1u, FQDB_STORE_SEGMENTED = 2u };
typedef struct fqdb_store_options {
    uint32_t struct_size, abi_version, durability, flags;
    uint64_t cache_bytes, delta_bytes, writer_bytes;
    uint64_t transaction_bytes, all_transaction_bytes, pinned_bytes;
    uint64_t max_snapshots;
} fqdb_store_options;
typedef struct fqdb_store_info {
    uint64_t version, durable_version, file_bytes;
    uint64_t indexed_keys, stored_keys, stored_value_bytes;
    uint64_t delta_bytes, cache_bytes, pinned_bytes, transaction_bytes;
    uint64_t recovered_transactions, repaired_bytes;
} fqdb_store_info;
typedef struct fqdb_store_row {
    fqdb_slice key, value;
    uint64_t version, expires_at_ms;
} fqdb_store_row;
typedef struct fqdb_store_maintenance_io {
    uint64_t read_bytes, write_bytes, sync_calls;
} fqdb_store_maintenance_io;
typedef struct fqdb_store_maintenance_options {
    uint32_t struct_size, abi_version, max_catchup_rounds, reserved;
    uint64_t timeout_ms, max_barrier_bytes;
    /* ABI 1 consumes only the original 32-byte prefix. ABI 2 requires the
       complete structure. Zero byte limits mean unlimited; stats is optional
       caller-owned storage, written on completion/error, never polled live.
       Counts are requested file bytes, not physical-device traffic. */
    uint64_t max_read_bytes, max_write_bytes;
    fqdb_store_maintenance_io *io_stats;
} fqdb_store_maintenance_options;
typedef struct fqdb_store_maintenance_info {
    uint64_t snapshot_version, final_version, source_file_bytes, output_file_bytes;
    uint64_t stored_keys, stored_value_bytes, copied_keys, catchup_rounds;
} fqdb_store_maintenance_info;
FQDB_API fqdb_status FQDB_CALL fqdb_store_backup_incremental(
    fqdb_store *store, const char *destination, uint64_t from_version,
    const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_recover_chain(
    const char *base, const char *const *archives, uint32_t archive_count, const char *destination,
    uint64_t target_version, uint64_t target_unix_ms, const fqdb_store_maintenance_options *options,
    fqdb_store_maintenance_info *out);
typedef struct fqdb_store_expiry_options {
    uint32_t struct_size, abi_version, max_keys, reserved;
    uint64_t timeout_ms, max_batch_bytes;
} fqdb_store_expiry_options;
typedef struct fqdb_store_expiry_info {
    uint64_t version, cutoff_ms, removed_keys, removed_value_bytes, visited_pages, more;
} fqdb_store_expiry_info;
/* NULL options: SYNC, create if missing, cache 64 MiB, delta 4 MiB,
   writer 32 MiB, per-txn 64 MiB, all-txn 256 MiB, pins 128 MiB, 1024 snapshots.
   Counts include tombstones/expired stored rows as indicated by field names.
   Budgets bound charged retention, not allocator/OS or entire process RSS. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_open(const char *directory,
                                               const fqdb_store_options *options, fqdb_store **out);
FQDB_API void FQDB_CALL fqdb_store_close(fqdb_store *store);
FQDB_API fqdb_status FQDB_CALL fqdb_store_get_info(fqdb_store *store, fqdb_store_info *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_sync(fqdb_store *store);
FQDB_API fqdb_status FQDB_CALL fqdb_store_checkpoint(fqdb_store *store);
/* NULL options: 8 background catch-up rounds, no deadline, at most 1 MiB of
   estimated changed key/value work under the final writer barrier (also capped
   by delta_bytes). A continuously growing backlog can return LIMIT unchanged.
   Compaction retains old generations while snapshots/transactions pin them.
   One compact/backup/verify operation may run per Store. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_compact(fqdb_store *store,
                                                  const fqdb_store_maintenance_options *options,
                                                  fqdb_store_maintenance_info *out);
/* Consistent full copy into a NEW directory. Never overwrites an existing
   directory. Returned snapshot_version identifies the captured source state.
   The result is independently reopenable and durable, including BUFFERED input.
   A crash can leave an unpublished sibling t<16-hex>.tmp directory. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_backup(fqdb_store *store, const char *destination,
                                                 const fqdb_store_maintenance_options *options,
                                                 fqdb_store_maintenance_info *out);
/* Verify all selected index pages, ordering, counters and referenced values.
   The live Store's files are not modified. Normal open/recovery is separate. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_verify(fqdb_store *store,
                                                 const fqdb_store_maintenance_options *options,
                                                 fqdb_store_maintenance_info *out);
/* Open an existing source, perform normal recovery, then verify/copy its
   captured state into a NEW directory. No overwrite or in-place restore. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_restore(const char *source, const char *destination,
                                                  const fqdb_store_maintenance_options *options,
                                                  fqdb_store_maintenance_info *out);
/* Thread-safe request for the currently active maintenance operation. Once
   manifest/directory publication starts it returns the actual publication result. */
FQDB_API void FQDB_CALL fqdb_store_cancel_maintenance(fqdb_store *store);
/* One atomic batch of expired-key tombstones, without reading value bytes.
   NULL options: at most 1024 keys / 1 MiB estimated metadata, no deadline.
   more requests another batch/pass, possibly an empty wraparound pass. Concurrent updates
   can return CONFLICT; retry from a new call. Uses Store durability and budgets.
   Old snapshots retain values; compact reclaims physical file bytes later.
   cancel_maintenance applies until WAL writing starts, then actual result wins. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_purge_expired(fqdb_store *store,
                                                        const fqdb_store_expiry_options *options,
                                                        fqdb_store_expiry_info *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_get(fqdb_store *store, fqdb_slice key, fqdb_buffer *out);
/* Shared immutable bytes: no result copy on a hot-cache hit. value_data is
   valid until value_close; close/destruction must not race an operation. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_get_view(fqdb_store *store, fqdb_slice key,
                                                   fqdb_store_value **out);
/* Direct shared Hash value path, without a command/result encode or transaction.
   Empty values are valid; lifetime matches fqdb_store_get_view. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_hash_get_view(fqdb_store *store, fqdb_slice scope,
                                                        fqdb_slice key, fqdb_store_value **out);
FQDB_API fqdb_slice FQDB_CALL fqdb_store_value_data(const fqdb_store_value *value);
FQDB_API void FQDB_CALL fqdb_store_value_close(fqdb_store_value *value);
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_create(fqdb_store *store,
                                                          fqdb_store_snapshot **out);
FQDB_API void FQDB_CALL fqdb_store_snapshot_close(fqdb_store_snapshot *snapshot);
/* TTL is evaluated at the snapshot's creation time, including all its cursors. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_get(const fqdb_store_snapshot *snapshot,
                                                       fqdb_slice key, fqdb_buffer *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_get_view(const fqdb_store_snapshot *snapshot,
                                                            fqdb_slice key, fqdb_store_value **out);
/* A transaction handle is single-threaded except for cancel(). A timeout is
   measured from begin using a monotonic clock; zero means no deadline. Cancellation
   and timeout are honored before WAL writing starts. Once writing starts, commit
   reports its actual result. Always close the transaction, even after commit. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_begin(fqdb_store *store, uint64_t timeout_ms,
                                                    fqdb_store_txn **out);
FQDB_API void FQDB_CALL fqdb_store_txn_close(fqdb_store_txn *txn);
FQDB_API void FQDB_CALL fqdb_store_txn_cancel(fqdb_store_txn *txn);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_get(fqdb_store_txn *txn, fqdb_slice key,
                                                  fqdb_buffer *out);
/* expires_at_ms is absolute Unix milliseconds; zero means no expiry. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_put(fqdb_store_txn *txn, fqdb_slice key,
                                                  fqdb_slice value, uint64_t expires_at_ms);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_delete(fqdb_store_txn *txn, fqdb_slice key);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_expire(fqdb_store_txn *txn, fqdb_slice key,
                                                     uint64_t expires_at_ms);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_patch(fqdb_store_txn *txn, fqdb_slice key,
                                                    uint64_t offset, uint64_t remove_bytes,
                                                    fqdb_slice insert);
/* Savepoints share roots in O(1); first edits copy shared paths/value once.
   Rollback keeps the named savepoint, discards younger ones and keeps read/range
   dependencies. release requires the youngest savepoint. Resource/staging errors
   require rollback_to or close before any commit. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_savepoint(fqdb_store_txn *txn, uint64_t *id);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_rollback_to(fqdb_store_txn *txn, uint64_t id);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_release(fqdb_store_txn *txn, uint64_t id);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_commit(fqdb_store_txn *txn, uint64_t *version,
                                                     FQDB_WRITE_ARGUMENT);
/* [begin,end), empty end means unbounded. reverse changes iteration order.
   Tx cursors capture staged data and register the entire range for phantom
   conflict checks. Returned slices are borrowed until next()/close(). */
FQDB_API fqdb_status FQDB_CALL fqdb_store_cursor_open(const fqdb_store_snapshot *snapshot,
                                                      fqdb_slice begin, fqdb_slice end,
                                                      uint32_t reverse, fqdb_store_cursor **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_cursor_open(fqdb_store_txn *txn, fqdb_slice begin,
                                                          fqdb_slice end, uint32_t reverse,
                                                          fqdb_store_cursor **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_cursor_next(fqdb_store_cursor *cursor,
                                                      fqdb_store_row *out);
FQDB_API void FQDB_CALL fqdb_store_cursor_close(fqdb_store_cursor *cursor);

/* Configurable container root + one portable name (1..32 ASCII letters,
   digits, '_' or '-'; Windows device names are rejected on every platform).
   Database files remain flat inside root/name. No global mutable root.
   path_at is read-only and returns an owned UTF-8 absolute directory path.
   open_at may create the container root only when CREATE is enabled.
   The directory is exactly root/name; no implicit db/log/bak subdirectories.
   Names are 1..32 portable ASCII letters/digits/_/-, excluding device names.
   Full internal paths must fit <240 UTF-16 units on Windows or <960 bytes on
   POSIX, including reserved internal-file headroom; each component <=255 native
   units. Path limits are checked before creating any directories. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_path_at(const char *root, const char *name,
                                                  fqdb_buffer *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_open_at(const char *root, const char *name,
                                                  const fqdb_store_options *options,
                                                  fqdb_store **out);

/* Value metadata belongs to the exact immutable bytes returned by get_view. */
FQDB_API uint64_t FQDB_CALL fqdb_store_value_version(const fqdb_store_value *value);
FQDB_API uint64_t FQDB_CALL fqdb_store_value_expires_at_ms(const fqdb_store_value *value);
/* NOT_FOUND can still return the last stored tombstone/expired version. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_get_version(fqdb_store *store, fqdb_slice key,
                                                      uint64_t *out);
typedef struct fqdb_store_atomic_info {
    uint64_t version, observed_version;
    int64_t value;
    uint32_t matched, changed;
} fqdb_store_atomic_info;
typedef struct fqdb_store_i64_field_update {
    fqdb_slice key;
    uint64_t offset;
    int64_t delta, minimum, maximum;
} fqdb_store_i64_field_update;
/* 1..256 ordered updates of existing little-endian signed 64-bit fields.
   Read/check/add/patch execute together against the latest writer view. Each
   resulting field must fit [minimum,maximum]; missing/expired keys, invalid
   offsets, overflow or constraints abort the entire command. Duplicate keys
   and overlapping fields observe prior items in input order. TTL is retained.
   Commands with no changed items do not create a version. info.value is the final item's
   result, observed_version is the writer view before this command, matched=1
   on success, changed=1 if any item changed. Output is zero on failure.
   timeout bounds admission/preparation, not an in-flight durability barrier.
   Inputs remain valid until return; a true force_reliable flushes prior writes
   too, including a no-op. No host callbacks run on the write thread. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_add_i64_fields(fqdb_store *store,
                                                         const fqdb_store_i64_field_update *items,
                                                         uint32_t count, uint64_t timeout_ms,
                                                         fqdb_store_atomic_info *out,
                                                         FQDB_WRITE_ARGUMENT);
/* One-call updates execute against the latest writer view. A true final
   argument forces an immediate reliable barrier through this operation and
   every earlier memory commit. False follows the configured mode. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_put(fqdb_store *store, fqdb_slice key, fqdb_slice value,
                                              uint64_t expires_at_ms, fqdb_store_atomic_info *out,
                                              FQDB_WRITE_ARGUMENT);
FQDB_API fqdb_status FQDB_CALL fqdb_store_delete(fqdb_store *store, fqdb_slice key,
                                                 fqdb_store_atomic_info *out, FQDB_WRITE_ARGUMENT);
FQDB_API fqdb_status FQDB_CALL fqdb_store_patch(fqdb_store *store, fqdb_slice key, uint64_t offset,
                                                uint64_t remove_bytes, fqdb_slice insert,
                                                fqdb_store_atomic_info *out, FQDB_WRITE_ARGUMENT);
typedef struct fqdb_store_write_state {
    uint64_t version, durable_version, pending_bytes, last_status;
} fqdb_store_write_state;
FQDB_API fqdb_status FQDB_CALL fqdb_store_write_state_get(fqdb_store *store,
                                                          fqdb_store_write_state *out);
typedef struct fqdb_store_history_info {
    uint64_t version, floor, oldest_pin, active_pins, total_pins;
    uint64_t retained_generations, retained_file_bytes;
} fqdb_store_history_info;
#define FQDB_HISTORY_CURRENT UINT64_MAX
enum { FQDB_WATCH_METADATA_ONLY = 1u, FQDB_WATCH_INCLUDE_SYSTEM = 2u };
typedef struct fqdb_store_watch_options {
    uint32_t struct_size, abi_version, flags, max_scan_commits;
    uint64_t max_batch_bytes;
} fqdb_store_watch_options;
typedef struct fqdb_store_change_row {
    fqdb_slice key, value;
    uint64_t expires_at_ms;
    uint32_t kind, reserved; /* FQDB_PUT (materialized patch/TTL update) or DELETE. */
} fqdb_store_change_row;
typedef struct fqdb_store_change_batch_info {
    uint64_t version, unix_ms, durable_version;
    uint32_t count, flags;
    const fqdb_store_change_row *rows;
} fqdb_store_change_batch_info;
/* Process-local pull stream, max 64 live watches per Store. from_version is the
   last consumed version; FQDB_HISTORY_CURRENT selects current atomically.
   Default options: values included, scan <=256 commits, output <=64MiB.
   A live watch protects unacknowledged WAL across compaction. Use a separate
   persistent history watermark for restart-safe consumer progress.
   next returns one whole matching commit, or an empty progress batch after
   bounded filtering. Repeated next re-delivers that batch until ack(version).
   No implicit ack: allocation/copy/consumer failure is safely retryable.
   Timeout 0 polls (NOT_MODIFIED); positive waits (TIMEOUT on expiry).
   cancel is terminal, concurrent with next/ack; other calls must be serialized.
   close requires no concurrent calls. Children keep the Store alive. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_watch_open(fqdb_store *store, uint64_t from_version,
                                                     fqdb_slice key_prefix,
                                                     const fqdb_store_watch_options *options,
                                                     fqdb_store_watch **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_watch_next(fqdb_store_watch *watch, uint64_t timeout_ms,
                                                     fqdb_store_change_batch **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_watch_ack(fqdb_store_watch *watch, uint64_t version);
FQDB_API void FQDB_CALL fqdb_store_watch_cancel(fqdb_store_watch *watch);
FQDB_API void FQDB_CALL fqdb_store_watch_close(fqdb_store_watch *watch);
/* Borrowed contiguous rows and bytes remain valid until this batch is closed,
   including after ack, cancel or closing the watch/Store. */
FQDB_API const fqdb_store_change_batch_info *FQDB_CALL
fqdb_store_change_batch_data(const fqdb_store_change_batch *batch);
FQDB_API void FQDB_CALL fqdb_store_change_batch_close(fqdb_store_change_batch *batch);
/* Durable named backup retention, at most 64 names of 1..128 bytes.
   from_version is the last fully archived version, not the next version.
   FQDB_HISTORY_CURRENT atomically reserves the latest published/candidate view.
   The watermark is bound to this database UUID; copied backup controls are
   inactive in an independent database. Release is idempotent. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_history_hold(fqdb_store *store, fqdb_slice name,
                                                       uint64_t from_version, uint64_t timeout_ms,
                                                       fqdb_store_atomic_info *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_history_release(fqdb_store *store, fqdb_slice name,
                                                          uint64_t timeout_ms,
                                                          fqdb_store_atomic_info *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_history_info_get(fqdb_store *store,
                                                           fqdb_store_history_info *out);
/* Linearized in the writer's commit queue, with normal group flush semantics.
   No hidden OCC retry loop. All outputs are zero on error/COMMIT_UNKNOWN.
   timeout_ms=0 means no deadline. Conditions that do not match return OK,
   matched=changed=0 without generating a new commit.
   expected_version=0 means logically absent (including expired/deleted).
   A positive expected version must match a LIVE record. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_compare_exchange(
    fqdb_store *store, fqdb_slice key, uint64_t expected_version, fqdb_slice value,
    uint64_t expires_at_ms, uint64_t timeout_ms, fqdb_store_atomic_info *out, FQDB_WRITE_ARGUMENT);
FQDB_API fqdb_status FQDB_CALL fqdb_store_delete_if_version(fqdb_store *store, fqdb_slice key,
                                                            uint64_t expected_version,
                                                            uint64_t timeout_ms,
                                                            fqdb_store_atomic_info *out,
                                                            FQDB_WRITE_ARGUMENT);
FQDB_API fqdb_status FQDB_CALL fqdb_store_put_if_changed(fqdb_store *store, fqdb_slice key,
                                                         fqdb_slice value, uint64_t expires_at_ms,
                                                         uint64_t timeout_ms,
                                                         fqdb_store_atomic_info *out,
                                                         FQDB_WRITE_ARGUMENT);
/* Exactly 8 little-endian signed bytes, missing starts at zero. Existing TTL
   is preserved; overflow or a result outside [minimum,maximum] is CONSTRAINT.
   A zero delta on an existing counter is a no-op. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_add_i64(fqdb_store *store, fqdb_slice key, int64_t delta,
                                                  int64_t minimum, int64_t maximum,
                                                  uint64_t timeout_ms, fqdb_store_atomic_info *out,
                                                  FQDB_WRITE_ARGUMENT);

typedef struct fqdb_store_put_item {
    fqdb_slice key, value;
    uint64_t expires_at_ms;
} fqdb_store_put_item;
typedef struct fqdb_store_result_row {
    fqdb_slice value;
    uint64_t version, expires_at_ms;
    uint32_t found, reserved;
} fqdb_store_result_row;
/* One-call immutable view. On success close owner with value_close; bytes and
   metadata remain valid across writes, compaction and Store close. */
typedef struct fqdb_store_shared_value {
    fqdb_store_value *owner;
    fqdb_store_result_row row;
} fqdb_store_shared_value;
FQDB_API fqdb_status FQDB_CALL fqdb_store_get_shared(fqdb_store *store, fqdb_slice key,
                                                     fqdb_store_shared_value *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_get_shared(const fqdb_store_snapshot *snapshot,
                                                              fqdb_slice key,
                                                              fqdb_store_shared_value *out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_key_get_shared(fqdb_store_key *key,
                                                         fqdb_store_shared_value *out);
/* Prepared shared reader: keys copied once; every read sees one current view
   and TTL time. Single-threaded: synchronize read/acquire/release/close.
   Borrowed rows AND bytes expire on the next read/acquire/release/close, even
   if that operation fails. Never write through returned pointers.
   Hot unchanged reads allocate/copy nothing. Retention uses pinned_bytes;
   metadata uses the same budget. Does not pin old generations/snapshot slots.
   acquire returns independently owned results, valid until results_close. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_reader_create(fqdb_store *store, const fqdb_slice *keys,
                                                        uint32_t count, fqdb_store_reader **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_reader_read(fqdb_store_reader *reader,
                                                      const fqdb_store_result_row **rows,
                                                      uint32_t *count);
FQDB_API fqdb_status FQDB_CALL fqdb_store_reader_acquire(fqdb_store_reader *reader,
                                                         fqdb_store_results **out);
FQDB_API void FQDB_CALL fqdb_store_reader_release(fqdb_store_reader *reader);
FQDB_API void FQDB_CALL fqdb_store_reader_close(fqdb_store_reader *reader);
/* Copy selected top-level protobuf fields through a read-only path. Fields are
   0..256 valid numbers, duplicates ignored, output preserves original wire order
   and repeated occurrences. Empty selection returns empty protobuf. Missing or
   expired record => NOT_FOUND, malformed protobuf => CONSTRAINT. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_project(fqdb_store *store, fqdb_slice key,
                                                  const uint32_t *fields, uint32_t field_count,
                                                  fqdb_buffer *out);
/* A reader with a precompiled selection; uses the same lifetime, budget, TTL,
   and single-threaded contracts as reader_create/read/acquire. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_project_reader_create(
    fqdb_store *store, const fqdb_slice *keys, uint32_t count, const uint32_t *fields,
    uint32_t field_count, fqdb_store_reader **out);
/* At most 4096 items/call, at most 64 MiB of retained result/value data.
   Results keep input order, duplicates and empty values; found=0 is a miss.
   Borrowed slices last until results_close. Reads see one view/TTL time.
   Tx reads include private writes (version=0) and register OCC dependencies. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_get_many(fqdb_store *store, const fqdb_slice *keys,
                                                   uint32_t count, fqdb_store_results **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_get_many(const fqdb_store_snapshot *snapshot,
                                                            const fqdb_slice *keys, uint32_t count,
                                                            fqdb_store_results **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_get_many(fqdb_store_txn *txn, const fqdb_slice *keys,
                                                       uint32_t count, fqdb_store_results **out);
FQDB_API uint32_t FQDB_CALL fqdb_store_results_count(const fqdb_store_results *results);
FQDB_API const fqdb_store_result_row *FQDB_CALL
fqdb_store_results_data(const fqdb_store_results *results);
FQDB_API fqdb_status FQDB_CALL fqdb_store_results_get(const fqdb_store_results *results,
                                                      uint32_t index, fqdb_store_result_row *out);
FQDB_API void FQDB_CALL fqdb_store_results_close(fqdb_store_results *results);
/* Prevalidate the complete input; resource/staging errors require rollback or
   close before commit. Input order applies; duplicate keys use the last put. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_put_many(fqdb_store_txn *txn,
                                                       const fqdb_store_put_item *items,
                                                       uint32_t count);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_delete_many(fqdb_store_txn *txn,
                                                          const fqdb_slice *keys, uint32_t count);
/* Immutable, Store-bound prepared data/CRC. Applying shares payloads without
   copying them; transaction budgets conservatively charge retained inputs.
   Transactions retain a batch through commit/rollback even after batch_close.
   Patching an adopted value still isolates its complete private buffer once. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_batch_create(fqdb_store *store,
                                                       const fqdb_store_put_item *items,
                                                       uint32_t count, fqdb_store_batch **out);
FQDB_API void FQDB_CALL fqdb_store_batch_close(fqdb_store_batch *batch);
FQDB_API fqdb_status FQDB_CALL fqdb_store_txn_put_batch(fqdb_store_txn *txn,
                                                        const fqdb_store_batch *batch);
/* Prepared keys retain the Store. A synchronized weak cache is valid only for
   the exact immutable view, never just a version number or generation offset.
   Concurrent reads are supported; destruction must not race a call. */
FQDB_API fqdb_status FQDB_CALL fqdb_store_key_prepare(fqdb_store *store, fqdb_slice key,
                                                      fqdb_store_key **out);
FQDB_API void FQDB_CALL fqdb_store_key_close(fqdb_store_key *key);
FQDB_API fqdb_status FQDB_CALL fqdb_store_key_get_view(fqdb_store_key *key, fqdb_store_value **out);
FQDB_API fqdb_status FQDB_CALL fqdb_store_key_get_into(fqdb_store_key *key, void *destination,
                                                       uint64_t capacity, uint64_t *required);
FQDB_API fqdb_status FQDB_CALL fqdb_store_snapshot_get_prepared_view(
    const fqdb_store_snapshot *snapshot, fqdb_store_key *key, fqdb_store_value **out);
#undef FQDB_WRITE_ARGUMENT
#ifdef __cplusplus
}
#endif
#endif
