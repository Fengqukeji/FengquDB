<!-- api:read -->
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

<!-- api:save -->
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

<!-- api:modify -->
## 3. Updates and atomic counters

Call put again to replace a whole value. patch changes a raw byte range, compare_exchange updates by version, and add_i64 atomically adds to a dedicated eight-byte integer. If encoding fails or the buffer is too small, do not write partial output to the database.

<!-- api:delete -->
## 4. Delete

```c
fqdb_store_atomic_info info = {0};
fqdb_status status = fqdb_store_delete(db, bytes("student/7"), &info, 0);
```

<!-- api:transaction -->
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

<!-- api:query -->
## 6. Conditional queries

fqdb_store_select accepts a binary query request and returns binary results for combined filters, sorting, grouping, aggregation, and equality joins. The C API has no typed query builder. Request and result fields are defined in the query formats at the end of this page.

<!-- api:index -->
## 7. Field indexes

index_define/index_define_compound create indexes for frequent filters and support constraints such as unique and required fields; index_drop removes an index.

<!-- api:durability -->
## 8. Critical writes: reliable confirmation

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory. Pass 1 as the final reliability flag:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```c
status = fqdb_store_put(db, bytes("payment/receipt"), bytes("paid"), 0, &info, 1);
status = fqdb_store_sync(db);
```

Check every status. On COMMIT_UNKNOWN, reconcile by business operation ID instead of blindly replaying a payment.

<!-- api:expiry -->
## 9. Expiring data

Pass an absolute Unix timestamp in milliseconds to put's expires_at_ms parameter; 0 means no expiry. Expired records are no longer visible to reads.

<!-- api:fastread -->
## 10. Shared reads and reusable buffers

get_shared returns a read-only lease; release it with value_close. key_prepare reuses a key, and reader_create reuses a batch reader. Reader rows become invalid on the next acquire/release/close.

<!-- api:snapshot -->
## 11. Consistent snapshots and iteration

snapshot creates a consistent view; cursor iterates by prefix or range. Release them with snapshot_close and cursor_close, respectively.

<!-- api:backup -->
## 12. Backup and restore

backup writes to a new backup directory; restore/recover_chain restore full or incremental backups. Close the database being replaced first. Do not copy files while they are being written.

<!-- api:collections -->
## 13. Native collections

execute handles Hash/Set/List/sorted-set commands, and hash_get reads a Hash field. Use them for atomic member updates, queues, and rankings.

<!-- api:maintenance -->
## 14. Compaction, cleanup, and verification

checkpoint creates a checkpoint, compact reclaims space, purge_expired cleans up expired records in batches, and verify checks files. Use options to bound the work per maintenance call.

<!-- api:watch -->
## 15. Commit changes and logging

watch consumes commit changes, ack acknowledges the processed watermark, and watch_close releases the consumer. fqdb_log_* configures diagnostic log callbacks.

<!-- api:serialization -->
## 16. Encoding, decoding, and resource release

Generated C decoded records borrow the input, which must remain alive and unchanged. Pass row.unknown_source when encoding again to preserve unknown fields. Output must not overlap the input.

Release ordinary read buffers with fqdb_buffer_free and shared values with value_close; do not interchange release functions. Keys are limited to 64 KiB and values to 16 MiB.
