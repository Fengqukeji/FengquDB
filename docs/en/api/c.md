# C API and examples

[Getting started](../languages/c.md) · [简体中文](../../api/c.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read

The bytes helper and db handle below are defined in the getting-started guide.

```c
fqdb_buffer value = {0};
fqdb_status status = fqdb_store_get(db, bytes("name"), &value);
if (status == FQDB_OK) {
    /* value.data/value.size is independently owned data; process it here */
    fqdb_buffer_free(&value);
} else if (status == FQDB_NOT_FOUND) {
    /* Not found */
} else {
    /* Handle the error */
}
```

An empty value is valid data. A length of zero does not mean the key is missing.

<details>
<summary>Functions and parameters (8)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_get(fqdb_store *store, fqdb_slice key, fqdb_buffer *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_get_version(fqdb_store *store, fqdb_slice key, uint64_t *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_get_view(fqdb_store *store, fqdb_slice key, fqdb_store_value **out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_reader_acquire(fqdb_store_reader *reader, fqdb_store_results **out)` | Read data or status |
| `Global` | `void fqdb_store_reader_close(fqdb_store_reader *reader)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_reader_create(fqdb_store *store, const fqdb_slice *keys, uint32_t count, fqdb_store_reader **out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_reader_read(fqdb_store_reader *reader, const fqdb_store_result_row **rows, uint32_t *count)` | Read data or status |
| `Global` | `void fqdb_store_reader_release(fqdb_store_reader *reader)` | Release an object or borrowed resource |

</details>

## 2. Insert, save, and store a string

```c
fqdb_store_atomic_info info = {0};
fqdb_status status = fqdb_store_put(db, bytes("name"), bytes("Alice"), 0, &info, 0);
if (status != FQDB_OK) { /* Handle the error */ }
```

put overwrites the previous value at the same key. Integers can use an explicit eight-byte little-endian encoding; keys used with add_i64 must use that format.

Use the generated functions to write an entity:

```c
struct Student student = {7, fqdb_text_from_cstr("Alice"), 95};
unsigned char key[64], encoded[256]; size_t key_size, value_size;
fqg_slice unknown = {NULL, 0};
if (!models_StudentFqCodec_key(&student,key,sizeof(key),&key_size)) return 1;
if (!models_StudentFqCodec_encode(&student,unknown,encoded,sizeof(encoded),&value_size)) return 1;
fqdb_slice k = {key,key_size}, v = {encoded,value_size};
status = fqdb_store_put(db,k,v,0,&info,0);
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_put(fqdb_store *store, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Save or set data |

</details>

## 3. Updates and atomic counters

Call put again to replace a whole value. patch changes a raw byte range, compare_exchange updates by version, and add_i64 atomically adds to a dedicated eight-byte integer. If encoding fails or the buffer is too small, do not write partial output to the database.

<details>
<summary>Functions and parameters (6)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_add_i64(fqdb_store *store, fqdb_slice key, int64_t delta, int64_t minimum, int64_t maximum, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Conditional update or partial modification |
| `Global` | `fqdb_status fqdb_store_add_i64_fields(fqdb_store *store, const fqdb_store_i64_field_update *items, uint32_t count, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Conditional update or partial modification |
| `Global` | `fqdb_status fqdb_store_compare_exchange(fqdb_store *store, fqdb_slice key, uint64_t expected_version, fqdb_slice value, uint64_t expires_at_ms, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Conditional update or partial modification |
| `Global` | `fqdb_status fqdb_store_delete_if_version(fqdb_store *store, fqdb_slice key, uint64_t expected_version, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Delete a record or member |
| `Global` | `fqdb_status fqdb_store_patch(fqdb_store *store, fqdb_slice key, uint64_t offset, uint64_t remove_bytes, fqdb_slice insert, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Conditional update or partial modification |
| `Global` | `fqdb_status fqdb_store_put_if_changed(fqdb_store *store, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Save or set data |

</details>

## 4. Delete

```c
fqdb_store_atomic_info info = {0};
fqdb_status status = fqdb_store_delete(db, bytes("student/7"), &info, 0);
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_delete(fqdb_store *store, fqdb_slice key, fqdb_store_atomic_info *out, uint32_t force_reliable)` | Delete a record or member |

</details>

## batch

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `void fqdb_store_batch_close(fqdb_store_batch *batch)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_batch_create(fqdb_store *store, const fqdb_store_put_item *items, uint32_t count, fqdb_store_batch **out)` | Process a batch |
| `Global` | `void fqdb_store_change_batch_close(fqdb_store_change_batch *batch)` | Release an object or borrowed resource |
| `Global` | `const fqdb_store_change_batch_info * fqdb_store_change_batch_data(const fqdb_store_change_batch *batch)` | Process a batch |
| `Global` | `fqdb_status fqdb_store_get_many(fqdb_store *store, const fqdb_slice *keys, uint32_t count, fqdb_store_results **out)` | Read data or status |

</details>

## 5. Transactions and atomic batch writes

```c
fqdb_store_txn *tx = NULL;
fqdb_status status = fqdb_store_txn_begin(db, 0, &tx);
if (status == FQDB_OK) {
    status = fqdb_store_txn_put(tx, bytes("a"), bytes("one"), 0);
    if (status == FQDB_OK) {
        uint64_t version = 0;
        status = fqdb_store_txn_commit(tx, &version, 0);
    }
    fqdb_store_txn_close(tx);
}
```

Closing an uncommitted transaction discards its changes. On CONFLICT, create a new transaction, read again, and recompute. put_many/delete_many apply a batch in one atomic write.

<details>
<summary>Functions and parameters (19)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_txn_begin(fqdb_store *store, uint64_t timeout_ms, fqdb_store_txn **out)` | Transaction operation |
| `Global` | `void fqdb_store_txn_cancel(fqdb_store_txn *txn)` | Transaction operation |
| `Global` | `void fqdb_store_txn_close(fqdb_store_txn *txn)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_txn_commit(fqdb_store_txn *txn, uint64_t *version, uint32_t force_reliable)` | Commit a transaction or inspect its result |
| `Global` | `fqdb_status fqdb_store_txn_cursor_open(fqdb_store_txn *txn, fqdb_slice begin, fqdb_slice end, uint32_t reverse, fqdb_store_cursor **out)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_delete(fqdb_store_txn *txn, fqdb_slice key)` | Delete a record or member |
| `Global` | `fqdb_status fqdb_store_txn_delete_many(fqdb_store_txn *txn, const fqdb_slice *keys, uint32_t count)` | Delete a record or member |
| `Global` | `fqdb_status fqdb_store_txn_execute(fqdb_store_txn *txn, fqdb_slice request, fqdb_buffer *out)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_expire(fqdb_store_txn *txn, fqdb_slice key, uint64_t expires_at_ms)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_get(fqdb_store_txn *txn, fqdb_slice key, fqdb_buffer *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_txn_get_many(fqdb_store_txn *txn, const fqdb_slice *keys, uint32_t count, fqdb_store_results **out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_txn_patch(fqdb_store_txn *txn, fqdb_slice key, uint64_t offset, uint64_t remove_bytes, fqdb_slice insert)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_put(fqdb_store_txn *txn, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_put_batch(fqdb_store_txn *txn, const fqdb_store_batch *batch)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_put_many(fqdb_store_txn *txn, const fqdb_store_put_item *items, uint32_t count)` | Transaction operation |
| `Global` | `fqdb_status fqdb_store_txn_release(fqdb_store_txn *txn, uint64_t id)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_txn_rollback_to(fqdb_store_txn *txn, uint64_t id)` | Roll back to a savepoint |
| `Global` | `fqdb_status fqdb_store_txn_savepoint(fqdb_store_txn *txn, uint64_t *id)` | Create or manage a transaction savepoint |
| `Global` | `fqdb_status fqdb_store_txn_submit(fqdb_store_txn *txn, fqdb_store_commit **out, uint32_t force_reliable)` | Transaction operation |

</details>

## 6. Conditional queries

fqdb_store_select accepts a binary query request and returns binary results for combined filters, sorting, grouping, aggregation, and equality joins. The C API has no typed query builder. Request and result fields are defined in the query formats at the end of this page.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_select(fqdb_store *store, fqdb_slice request, fqdb_buffer *out)` | Build a query or read its results |

</details>

## 7. Field indexes

index_define/index_define_compound create indexes for frequent filters and support constraints such as unique and required fields; index_drop removes an index.

<details>
<summary>Functions and parameters (3)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_index_define(fqdb_store *store, fqdb_slice name, fqdb_slice key_prefix, uint32_t field, uint32_t type, uint32_t flags, const struct fqdb_store_maintenance_options *options, struct fqdb_store_maintenance_info *out)` | Define or use an index |
| `Global` | `fqdb_status fqdb_store_index_define_compound(fqdb_store *store, fqdb_slice name, fqdb_slice key_prefix, const fqdb_store_index_field *fields, uint32_t count, uint32_t flags, const struct fqdb_store_maintenance_options *options, struct fqdb_store_maintenance_info *out)` | Define or use an index |
| `Global` | `fqdb_status fqdb_store_index_drop(fqdb_store *store, fqdb_slice name, const struct fqdb_store_maintenance_options *options, struct fqdb_store_maintenance_info *out)` | Define or use an index |

</details>

## 8. Critical writes: reliable confirmation

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory. Pass 1 as the final reliability flag:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```c
status = fqdb_store_put(db, bytes("payment/receipt"), bytes("paid"), 0, &info, 1);
status = fqdb_store_sync(db);
```

Check every status. On COMMIT_UNKNOWN, reconcile by business operation ID instead of blindly replaying a payment.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_commit_poll(fqdb_store_commit *commit, fqdb_status *completion_status, uint64_t *version)` | Commit a transaction or inspect its result |
| `Global` | `fqdb_status fqdb_store_commit_wait(fqdb_store_commit *commit, uint64_t timeout_ms, fqdb_status *completion_status, uint64_t *version)` | Commit a transaction or inspect its result |
| `Global` | `fqdb_status fqdb_store_put_submit(fqdb_store *store, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms, fqdb_store_commit **out, uint32_t force_reliable)` | Save or set data |
| `Global` | `fqdb_status fqdb_store_sync(fqdb_store *store)` | Control durability or wait for a commit |
| `Global` | `fqdb_status fqdb_store_write_configure(fqdb_store *store, const fqdb_store_write_options *options)` | Control durability or wait for a commit |
| `Global` | `fqdb_status fqdb_store_write_options_get(fqdb_store *store, fqdb_store_write_options *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_write_state_get(fqdb_store *store, fqdb_store_write_state *out)` | Read data or status |

</details>

## 9. Expiring data

Pass an absolute Unix timestamp in milliseconds to put's expires_at_ms parameter; 0 means no expiry. Expired records are no longer visible to reads.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_purge_expired(fqdb_store *store, const fqdb_store_expiry_options *options, fqdb_store_expiry_info *out)` | Manage expiring data |
| `Global` | `uint64_t fqdb_store_value_expires_at_ms(const fqdb_store_value *value)` | Manage expiring data |

</details>

## 10. Shared reads and reusable buffers

get_shared returns a read-only lease; release it with value_close. key_prepare reuses a key, and reader_create reuses a batch reader. Reader rows become invalid on the next acquire/release/close.

<details>
<summary>Functions and parameters (3)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_get_shared(fqdb_store *store, fqdb_slice key, fqdb_store_shared_value *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_key_get_shared(fqdb_store_key *key, fqdb_store_shared_value *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_snapshot_get_shared(const fqdb_store_snapshot *snapshot, fqdb_slice key, fqdb_store_shared_value *out)` | Read data or status |

</details>

## 11. Consistent snapshots and iteration

snapshot creates a consistent view; cursor iterates by prefix or range. Release them with snapshot_close and cursor_close, respectively.

<details>
<summary>Functions and parameters (9)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `void fqdb_store_cursor_close(fqdb_store_cursor *cursor)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_cursor_next(fqdb_store_cursor *cursor, fqdb_store_row *out)` | Consistent reads or range iteration |
| `Global` | `fqdb_status fqdb_store_cursor_open(const fqdb_store_snapshot *snapshot, fqdb_slice begin, fqdb_slice end, uint32_t reverse, fqdb_store_cursor **out)` | Consistent reads or range iteration |
| `Global` | `void fqdb_store_snapshot_close(fqdb_store_snapshot *snapshot)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_snapshot_create(fqdb_store *store, fqdb_store_snapshot **out)` | Consistent reads or range iteration |
| `Global` | `fqdb_status fqdb_store_snapshot_get(const fqdb_store_snapshot *snapshot, fqdb_slice key, fqdb_buffer *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_snapshot_get_many(const fqdb_store_snapshot *snapshot, const fqdb_slice *keys, uint32_t count, fqdb_store_results **out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_snapshot_get_prepared_view(const fqdb_store_snapshot *snapshot, fqdb_store_key *key, fqdb_store_value **out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_snapshot_get_view(const fqdb_store_snapshot *snapshot, fqdb_slice key, fqdb_store_value **out)` | Read data or status |

</details>

## 12. Backup and restore

backup writes to a new backup directory; restore/recover_chain restore full or incremental backups. Close the database being replaced first. Do not copy files while they are being written.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_backup(fqdb_store *store, const char *destination, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | Backup, restore, or history retention |
| `Global` | `fqdb_status fqdb_store_backup_incremental(fqdb_store *store, const char *destination, uint64_t from_version, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | Backup, restore, or history retention |
| `Global` | `fqdb_status fqdb_store_history_hold(fqdb_store *store, fqdb_slice name, uint64_t from_version, uint64_t timeout_ms, fqdb_store_atomic_info *out)` | Backup, restore, or history retention |
| `Global` | `fqdb_status fqdb_store_history_info_get(fqdb_store *store, fqdb_store_history_info *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_history_release(fqdb_store *store, fqdb_slice name, uint64_t timeout_ms, fqdb_store_atomic_info *out)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_recover_chain(const char *base, const char *const *archives, uint32_t archive_count, const char *destination, uint64_t target_version, uint64_t target_unix_ms, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | Backup, restore, or history retention |
| `Global` | `fqdb_status fqdb_store_restore(const char *source, const char *destination, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | Backup, restore, or history retention |

</details>

## 13. Native collections

execute handles Hash/Set/List/sorted-set commands, and hash_get reads a Hash field. Use them for atomic member updates, queues, and rankings.

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_hash_get_view(fqdb_store *store, fqdb_slice scope, fqdb_slice key, fqdb_store_value **out)` | Read data or status |

</details>

## 14. Compaction, cleanup, and verification

checkpoint creates a checkpoint, compact reclaims space, purge_expired cleans up expired records in batches, and verify checks files. Use options to bound the work per maintenance call.

<details>
<summary>Functions and parameters (7)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_log_get_info(fqdb_log_handle *handle, fqdb_log_info *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_checkpoint(fqdb_store *store)` | Inspect status or maintain the database |
| `Global` | `fqdb_status fqdb_store_compact(fqdb_store *store, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | Inspect status or maintain the database |
| `Global` | `fqdb_status fqdb_store_configure(fqdb_store *store, const fqdb_store_runtime_options *options)` | Inspect status or maintain the database |
| `Global` | `fqdb_status fqdb_store_get_info(fqdb_store *store, fqdb_store_info *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_runtime_info_get(fqdb_store *store, fqdb_store_runtime_info *out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_verify(fqdb_store *store, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | Inspect status or maintain the database |

</details>

## 15. Commit changes and logging

watch consumes commit changes, ack acknowledges the processed watermark, and watch_close releases the consumer. fqdb_log_* configures diagnostic log callbacks.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `fqdb_status fqdb_store_watch_ack(fqdb_store_watch *watch, uint64_t version)` | Consume changes or diagnose issues |
| `Global` | `void fqdb_store_watch_cancel(fqdb_store_watch *watch)` | Consume changes or diagnose issues |
| `Global` | `void fqdb_store_watch_close(fqdb_store_watch *watch)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_watch_next(fqdb_store_watch *watch, uint64_t timeout_ms, fqdb_store_change_batch **out)` | Consume changes or diagnose issues |
| `Global` | `fqdb_status fqdb_store_watch_open(fqdb_store *store, uint64_t from_version, fqdb_slice key_prefix, const fqdb_store_watch_options *options, fqdb_store_watch **out)` | Consume changes or diagnose issues |

</details>

## 16. Encoding, decoding, and resource release

Generated C decoded records borrow the input, which must remain alive and unchanged. Pass row.unknown_source when encoding again to preserve unknown fields. Output must not overlap the input.

Release ordinary read buffers with fqdb_buffer_free and shared values with value_close; do not interchange release functions. Keys are limited to 64 KiB and values to 16 MiB.

## Other utilities

<details>
<summary>Functions and parameters (27)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `Global` | `uint32_t fqdb_abi_version()` | Utility operation |
| `Global` | `fqdb_status fqdb_buffer_copy(fqdb_slice source, fqdb_buffer *out)` | Utility operation |
| `Global` | `void fqdb_buffer_free(fqdb_buffer *buffer)` | Utility operation |
| `Global` | `fqdb_status fqdb_log_close(fqdb_log_handle *handle)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_log_open(const fqdb_log_options *options, fqdb_log_handle **out)` | Utility operation |
| `Global` | `const char * fqdb_status_message(fqdb_status status)` | Utility operation |
| `Global` | `void fqdb_store_cancel_maintenance(fqdb_store *store)` | Utility operation |
| `Global` | `void fqdb_store_close(fqdb_store *store)` | Release an object or borrowed resource |
| `Global` | `void fqdb_store_commit_cancel(fqdb_store_commit *commit)` | Commit a transaction or inspect its result |
| `Global` | `void fqdb_store_commit_close(fqdb_store_commit *commit)` | Release an object or borrowed resource |
| `Global` | `void fqdb_store_key_close(fqdb_store_key *key)` | Release an object or borrowed resource |
| `Global` | `fqdb_status fqdb_store_key_get_into(fqdb_store_key *key, void *destination, uint64_t capacity, uint64_t *required)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_key_get_view(fqdb_store_key *key, fqdb_store_value **out)` | Read data or status |
| `Global` | `fqdb_status fqdb_store_key_prepare(fqdb_store *store, fqdb_slice key, fqdb_store_key **out)` | Utility operation |
| `Global` | `fqdb_status fqdb_store_open(const char *directory, const fqdb_store_options *options, fqdb_store **out)` | Utility operation |
| `Global` | `fqdb_status fqdb_store_open_at(const char *root, const char *name, const fqdb_store_options *options, fqdb_store **out)` | Utility operation |
| `Global` | `fqdb_status fqdb_store_path_at(const char *root, const char *name, fqdb_buffer *out)` | Utility operation |
| `Global` | `fqdb_status fqdb_store_project(fqdb_store *store, fqdb_slice key, const uint32_t *fields, uint32_t field_count, fqdb_buffer *out)` | Utility operation |
| `Global` | `fqdb_status fqdb_store_project_reader_create(fqdb_store *store, const fqdb_slice *keys, uint32_t count, const uint32_t *fields, uint32_t field_count, fqdb_store_reader **out)` | Utility operation |
| `Global` | `void fqdb_store_results_close(fqdb_store_results *results)` | Release an object or borrowed resource |
| `Global` | `uint32_t fqdb_store_results_count(const fqdb_store_results *results)` | Utility operation |
| `Global` | `const fqdb_store_result_row * fqdb_store_results_data(const fqdb_store_results *results)` | Utility operation |
| `Global` | `fqdb_status fqdb_store_results_get(const fqdb_store_results *results, uint32_t index, fqdb_store_result_row *out)` | Read data or status |
| `Global` | `void fqdb_store_value_close(fqdb_store_value *value)` | Release an object or borrowed resource |
| `Global` | `fqdb_slice fqdb_store_value_data(const fqdb_store_value *value)` | Utility operation |
| `Global` | `uint64_t fqdb_store_value_version(const fqdb_store_value *value)` | Utility operation |
| `Global` | `fqdb_text fqdb_text_from_cstr(const char *value)` | Utility operation |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>Global</summary>

| Declaration |
| --- |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_ALREADY_EXISTS` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_ASYNC_REQUIRED` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_BUFFER_TOO_SMALL` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CANCELLED` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_COLLECTION_CLEAR` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_COLLECTION_SIZE` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_COMMIT_UNKNOWN` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CONFLICT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CONSTRAINT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CORRUPT` |
| `(anonymous enum at include/fengqudb.h:54:1) FQDB_DELETE` |
| `(anonymous enum at include/fengqudb.h:53:1) FQDB_DURABILITY_BUFFERED` |
| `(anonymous enum at include/fengqudb.h:53:1) FQDB_DURABILITY_SYNC` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_END` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_DELETE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_GET` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_SCAN` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_SET` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_HISTORY_LOST` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_INDEX_LOOKUP` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_INDEX_QUERY` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_INTERNAL_ERROR` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_INVALID_ARGUMENT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_IO_ERROR` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_LIMIT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_GET` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_POP_LEFT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_POP_RIGHT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_PUSH_LEFT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_PUSH_RIGHT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_RANGE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_SET` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_LOCKED` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_BACKUP` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_BACKUP_INCREMENTAL` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_CHANGES` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_CHECKPOINT` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_COMMIT` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_COMPACT` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_CONFIGURE` |
| `(anonymous enum at include/fengqudb_log.h:7:1) FQDB_LOG_ERROR` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_INDEX_DEFINE` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_INDEX_DROP` |
| `(anonymous enum at include/fengqudb_log.h:7:1) FQDB_LOG_INFO` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_OPEN` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_PURGE_EXPIRED` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_RECOVER_CHAIN` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_RESTORE` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_SYNC` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_VERIFY` |
| `(anonymous enum at include/fengqudb_log.h:7:1) FQDB_LOG_WARNING` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_NOT_FOUND` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_NOT_MODIFIED` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_NO_MEMORY` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_OK` |
| `(anonymous enum at include/fengqudb.h:54:1) FQDB_PUT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_RECORD_PROJECT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_RECOVERY_REQUIRED` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SELECT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SELECT_EXPLAIN` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_ADD` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_CONTAINS` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_REMOVE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_SCAN` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_ADD` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_RANGE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_REMOVE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_SCORE` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_ARGUMENTS` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_CHECKSUM` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_COMPLETE` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_FILE_HEADER` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_FLUSH` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_INDEX` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_OPEN` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_PAYLOAD` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_PUBLISH` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_RECORD_HEADER` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_REPAIR` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_TRAILER` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_WRITE` |
| `(anonymous enum at include\fengqudb_store.h:149:1) FQDB_STORE_CREATE = 1` |
| `(anonymous enum at include\fengqudb_store.h:149:1) FQDB_STORE_SEGMENTED = 2` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_TIMEOUT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_UNSUPPORTED` |
| `(anonymous enum at include\fengqudb_store.h:373:1) FQDB_WATCH_INCLUDE_SYSTEM = 2` |
| `(anonymous enum at include\fengqudb_store.h:373:1) FQDB_WATCH_METADATA_ONLY = 1` |
| `(anonymous enum at include\fengqudb_store.h:24:1) FQDB_WRITE_PERFORMANCE` |
| `(anonymous enum at include\fengqudb_store.h:24:1) FQDB_WRITE_RELIABLE` |
| `struct fqdb_buffer` |
| `struct fqdb_buffer fqdb_buffer` |
| `struct fqdb_bytes fqdb_bytes` |
| `void (*)(void *, const fqdb_log_event *) fqdb_log_callback` |
| `struct fqdb_log_event` |
| `struct fqdb_log_event fqdb_log_event` |
| `struct fqdb_log_handle` |
| `struct fqdb_log_handle fqdb_log_handle` |
| `struct fqdb_log_info` |
| `struct fqdb_log_info fqdb_log_info` |
| `struct fqdb_log_options` |
| `struct fqdb_log_options fqdb_log_options` |
| `struct fqdb_slice` |
| `struct fqdb_slice fqdb_slice` |
| `int32_t fqdb_status` |
| `struct fqdb_store` |
| `struct fqdb_store fqdb_store` |
| `struct fqdb_store_atomic_info` |
| `struct fqdb_store_atomic_info fqdb_store_atomic_info` |
| `struct fqdb_store_batch` |
| `struct fqdb_store_batch fqdb_store_batch` |
| `struct fqdb_store_change_batch` |
| `struct fqdb_store_change_batch fqdb_store_change_batch` |
| `struct fqdb_store_change_batch_info` |
| `struct fqdb_store_change_batch_info fqdb_store_change_batch_info` |
| `struct fqdb_store_change_row` |
| `struct fqdb_store_change_row fqdb_store_change_row` |
| `struct fqdb_store_commit` |
| `struct fqdb_store_commit fqdb_store_commit` |
| `struct fqdb_store_cursor` |
| `struct fqdb_store_cursor fqdb_store_cursor` |
| `struct fqdb_store_expiry_info` |
| `struct fqdb_store_expiry_info fqdb_store_expiry_info` |
| `struct fqdb_store_expiry_options` |
| `struct fqdb_store_expiry_options fqdb_store_expiry_options` |
| `struct fqdb_store_history_info` |
| `struct fqdb_store_history_info fqdb_store_history_info` |
| `struct fqdb_store_i64_field_update` |
| `struct fqdb_store_i64_field_update fqdb_store_i64_field_update` |
| `struct fqdb_store_index_field` |
| `struct fqdb_store_index_field fqdb_store_index_field` |
| `struct fqdb_store_info` |
| `struct fqdb_store_info fqdb_store_info` |
| `struct fqdb_store_key` |
| `struct fqdb_store_key fqdb_store_key` |
| `struct fqdb_store_maintenance_info` |
| `struct fqdb_store_maintenance_info fqdb_store_maintenance_info` |
| `struct fqdb_store_maintenance_io` |
| `struct fqdb_store_maintenance_io fqdb_store_maintenance_io` |
| `struct fqdb_store_maintenance_options` |
| `struct fqdb_store_maintenance_options fqdb_store_maintenance_options` |
| `struct fqdb_store_options` |
| `struct fqdb_store_options fqdb_store_options` |
| `struct fqdb_store_put_item` |
| `struct fqdb_store_put_item fqdb_store_put_item` |
| `struct fqdb_store_reader` |
| `struct fqdb_store_reader fqdb_store_reader` |
| `struct fqdb_store_result_row` |
| `struct fqdb_store_result_row fqdb_store_result_row` |
| `struct fqdb_store_results` |
| `struct fqdb_store_results fqdb_store_results` |
| `struct fqdb_store_row` |
| `struct fqdb_store_row fqdb_store_row` |
| `struct fqdb_store_runtime_info` |
| `struct fqdb_store_runtime_info fqdb_store_runtime_info` |
| `struct fqdb_store_runtime_options` |
| `struct fqdb_store_runtime_options fqdb_store_runtime_options` |
| `struct fqdb_store_shared_value` |
| `struct fqdb_store_shared_value fqdb_store_shared_value` |
| `struct fqdb_store_snapshot` |
| `struct fqdb_store_snapshot fqdb_store_snapshot` |
| `struct fqdb_store_txn` |
| `struct fqdb_store_txn fqdb_store_txn` |
| `struct fqdb_store_value` |
| `struct fqdb_store_value fqdb_store_value` |
| `struct fqdb_store_watch` |
| `struct fqdb_store_watch fqdb_store_watch` |
| `struct fqdb_store_watch_options` |
| `struct fqdb_store_watch_options fqdb_store_watch_options` |
| `struct fqdb_store_write_options` |
| `struct fqdb_store_write_options fqdb_store_write_options` |
| `struct fqdb_store_write_state` |
| `struct fqdb_store_write_state fqdb_store_write_state` |
| `struct fqdb_text fqdb_text` |

</details>

<details>
<summary>fqdb_buffer</summary>

| Declaration |
| --- |
| `void * data` |
| `uint64_t size` |

</details>

<details>
<summary>fqdb_log_event</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `uint64_t base_version` |
| `uint64_t database_id` |
| `uint32_t durability` |
| `uint64_t duration_us` |
| `uint64_t file_offset` |
| `uint32_t level` |
| `uint32_t operation` |
| `uint64_t operation_count` |
| `uint32_t os_error` |
| `uint64_t payload_bytes` |
| `uint64_t repaired_bytes` |
| `uint32_t stage` |
| `fqdb_status status` |
| `uint32_t struct_size` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_log_info</summary>

| Declaration |
| --- |
| `uint64_t active` |
| `uint64_t callback_errors` |
| `uint64_t delivered` |

</details>

<details>
<summary>fqdb_log_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `fqdb_log_callback callback` |
| `uint32_t min_level` |
| `uint32_t reserved` |
| `uint64_t slow_commit_us` |
| `uint32_t struct_size` |
| `void * user` |

</details>

<details>
<summary>fqdb_slice</summary>

| Declaration |
| --- |
| `const void * data` |
| `uint64_t size` |

</details>

<details>
<summary>fqdb_store_atomic_info</summary>

| Declaration |
| --- |
| `uint32_t changed` |
| `uint32_t matched` |
| `uint64_t observed_version` |
| `int64_t value` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_change_batch_info</summary>

| Declaration |
| --- |
| `uint32_t count` |
| `uint64_t durable_version` |
| `uint32_t flags` |
| `const fqdb_store_change_row * rows` |
| `uint64_t unix_ms` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_change_row</summary>

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `fqdb_slice key` |
| `uint32_t kind` |
| `uint32_t reserved` |
| `fqdb_slice value` |

</details>

<details>
<summary>fqdb_store_expiry_info</summary>

| Declaration |
| --- |
| `uint64_t cutoff_ms` |
| `uint64_t more` |
| `uint64_t removed_keys` |
| `uint64_t removed_value_bytes` |
| `uint64_t version` |
| `uint64_t visited_pages` |

</details>

<details>
<summary>fqdb_store_expiry_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `uint64_t max_batch_bytes` |
| `uint32_t max_keys` |
| `uint32_t reserved` |
| `uint32_t struct_size` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fqdb_store_history_info</summary>

| Declaration |
| --- |
| `uint64_t active_pins` |
| `uint64_t floor` |
| `uint64_t oldest_pin` |
| `uint64_t retained_file_bytes` |
| `uint64_t retained_generations` |
| `uint64_t total_pins` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_i64_field_update</summary>

| Declaration |
| --- |
| `int64_t delta` |
| `fqdb_slice key` |
| `int64_t maximum` |
| `int64_t minimum` |
| `uint64_t offset` |

</details>

<details>
<summary>fqdb_store_index_field</summary>

| Declaration |
| --- |
| `uint32_t field` |
| `uint32_t type` |

</details>

<details>
<summary>fqdb_store_info</summary>

| Declaration |
| --- |
| `uint64_t cache_bytes` |
| `uint64_t delta_bytes` |
| `uint64_t durable_version` |
| `uint64_t file_bytes` |
| `uint64_t indexed_keys` |
| `uint64_t pinned_bytes` |
| `uint64_t recovered_transactions` |
| `uint64_t repaired_bytes` |
| `uint64_t stored_keys` |
| `uint64_t stored_value_bytes` |
| `uint64_t transaction_bytes` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_maintenance_info</summary>

| Declaration |
| --- |
| `uint64_t catchup_rounds` |
| `uint64_t copied_keys` |
| `uint64_t final_version` |
| `uint64_t output_file_bytes` |
| `uint64_t snapshot_version` |
| `uint64_t source_file_bytes` |
| `uint64_t stored_keys` |
| `uint64_t stored_value_bytes` |

</details>

<details>
<summary>fqdb_store_maintenance_io</summary>

| Declaration |
| --- |
| `uint64_t read_bytes` |
| `uint64_t sync_calls` |
| `uint64_t write_bytes` |

</details>

<details>
<summary>fqdb_store_maintenance_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `fqdb_store_maintenance_io * io_stats` |
| `uint64_t max_barrier_bytes` |
| `uint32_t max_catchup_rounds` |
| `uint64_t max_read_bytes` |
| `uint64_t max_write_bytes` |
| `uint32_t reserved` |
| `uint32_t struct_size` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fqdb_store_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `uint64_t all_transaction_bytes` |
| `uint64_t cache_bytes` |
| `uint64_t delta_bytes` |
| `uint32_t durability` |
| `uint32_t flags` |
| `uint64_t max_snapshots` |
| `uint64_t pinned_bytes` |
| `uint32_t struct_size` |
| `uint64_t transaction_bytes` |
| `uint64_t writer_bytes` |

</details>

<details>
<summary>fqdb_store_put_item</summary>

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `fqdb_slice key` |
| `fqdb_slice value` |

</details>

<details>
<summary>fqdb_store_result_row</summary>

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `uint32_t found` |
| `uint32_t reserved` |
| `fqdb_slice value` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_row</summary>

| Declaration |
| --- |
| `uint64_t expires_at_ms` |
| `fqdb_slice key` |
| `fqdb_slice value` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_runtime_info</summary>

| Declaration |
| --- |
| `uint64_t compactions` |
| `uint64_t expiry_batches` |
| `uint64_t group_flushes` |
| `uint64_t group_transactions` |
| `uint64_t last_status` |
| `uint64_t running` |

</details>

<details>
<summary>fqdb_store_runtime_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `uint32_t background` |
| `uint64_t compact_growth_bytes` |
| `uint64_t compact_min_bytes` |
| `uint32_t expiry_keys` |
| `uint32_t garbage_percent` |
| `uint32_t group_delay_us` |
| `uint32_t group_max` |
| `uint32_t interval_ms` |
| `uint64_t job_max_read_bytes` |
| `uint64_t job_max_write_bytes` |
| `uint64_t job_timeout_ms` |
| `uint32_t struct_size` |

</details>

<details>
<summary>fqdb_store_shared_value</summary>

| Declaration |
| --- |
| `fqdb_store_value * owner` |
| `fqdb_store_result_row row` |

</details>

<details>
<summary>fqdb_store_watch_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `uint32_t flags` |
| `uint64_t max_batch_bytes` |
| `uint32_t max_scan_commits` |
| `uint32_t struct_size` |

</details>

<details>
<summary>fqdb_store_write_options</summary>

| Declaration |
| --- |
| `uint32_t abi_version` |
| `uint32_t mode` |
| `uint32_t period_ms` |
| `uint32_t struct_size` |
| `uint64_t threshold_bytes` |

</details>

<details>
<summary>fqdb_store_write_state</summary>

| Declaration |
| --- |
| `uint64_t durable_version` |
| `uint64_t last_status` |
| `uint64_t pending_bytes` |
| `uint64_t version` |

</details>

<details>
<summary>Query format: store_select</summary>

```protobuf
syntax = "proto3";
package fengqudb.api;
// C ABI: txn_execute operation 53 executes, 54 explains. The request is in
// FeatureRequest.value; FeatureResult.value contains SelectResult.
message QueryValue { uint32 kind=1; uint64 uint_value=2; sint64 int_value=3; double real_value=4; bytes bytes_value=5; }
message QueryField { uint32 source=1; uint32 type=2; repeated uint32 path=3; }
message QueryExpr { uint32 op=1; QueryField field=2; repeated QueryValue values=3; repeated QueryExpr children=4; optional QueryField right=5; }
message QueryColumn { QueryField field=1; uint32 aggregate=2; string name=3; bool distinct=4; }
message QueryOrder { QueryField field=1; bool descending=2; bool nulls_first=3; }
message QueryJoin { bytes prefix=1; repeated QueryField left=2; repeated QueryField right=3; uint32 kind=4; QueryExpr on=5; }
// SDK encoders explicitly emit defaults; limits cannot be zero.
message Select {
  bytes prefix=1; QueryExpr where=2; repeated QueryColumn columns=3;
  repeated QueryOrder order=4; repeated QueryField group=5; QueryExpr having=6;
  repeated QueryJoin joins=7; uint64 limit=8; uint64 offset=9;
  uint64 memory_bytes=10; uint64 max_candidates=11; uint64 max_work=12;
  uint64 timeout_ms=13; bool allow_scan=14; bool count_total=15;
  bool distinct=16; bytes index_hint=17; bytes after=18;
}
message QueryRow { bytes key=1; repeated QueryValue values=2; }
message QueryAccess { uint32 source=1; string strategy=2; string index=3; string reason=4; bool covering=5; bool ordered=6; bool needs_scan=7; uint64 sampled_candidates=8; }
message QueryStats { uint64 candidates=1; uint64 matched=2; uint64 result_total=3; uint64 work=4; uint64 peak_memory=5; uint64 payload_reads=6; uint64 join_probes=7; bool complete=8; }
message SelectResult { repeated QueryRow rows=1; repeated QueryAccess access=2; QueryStats stats=3; bytes cursor=4; }
```

</details>

<details>
<summary>Query format: store_query</summary>

```protobuf
syntax = "proto3";
package fengqudb.api;

// The inner messages are encoded using the application's own protobuf schema.
// Only a contiguous prefix of index fields belongs in equal. Lower/upper each
// contain exactly the next indexed field. Presence distinguishes no bound.
message IndexQuery {
  bytes equal = 1;
  optional bytes lower = 2;
  optional bytes upper = 3;
  uint32 flags = 4; // 1 lower exclusive, 2 upper inclusive, 4 reverse, 8 values
  repeated uint32 fields = 5; // top-level projection; packed/unpacked accepted
}
```

</details>
