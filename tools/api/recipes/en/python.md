<!-- api:read -->
## 1. Read: find a record by key

```python
data = db.get(b"student/7")
student = None if data is None else Codec.decode(data).value
```

A missing record returns None; empty bytes are valid data. Codec is StudentFqCodec generated in the getting-started guide.

<!-- api:save -->
## 2. Insert or save: entities, numbers, and strings

```python
student = Student(7, "Alice", 95)
db.put(Codec.key(student), Codec.encode(student))
db.put(b"name", "Alice".encode("utf-8"))
db.put(b"score", (95).to_bytes(8, "little", signed=True))
score = int.from_bytes(db.get(b"score"), "little", signed=True)
```

put overwrites the previous value at the same key. Single strings and integers need no entity definition; the integer format above also works with add_i64.

<!-- api:modify -->
## 3. Update only what the application needs

```python
data = db.get(b"student/7")
if data is not None:
    row = Codec.decode(data)
    row.value.score = 96
    db.put(row.fq_key(), row.fq_encode())
db.add_i64(b"login-count", 1) # Use atomic increments for concurrent counters
```

patch changes a raw binary range, compare_exchange updates by version, and put_if_absent prevents overwriting an existing record.

<!-- api:delete -->
## 4. Delete

```python
db.delete(b"student/7")
```

<!-- api:batch -->
## 5. Batch: read and write multiple records

```python
values = db.get_many([b"student/7", b"student/8"])
db.put_many([(b"a", b"one"), (b"b", b"two")])
db.delete_many([b"a", b"b"])
```

put_many/delete_many commit their changes as one atomic operation.

<!-- api:transaction -->
## 6. Transactions: changes that succeed together

```python
with db.transaction() as tx:
    tx.put(b"bag/item", b"sword")
    tx.delete(b"reward/pending")
```

A normal exit from with commits automatically; an exceptional exit discards changes. Use savepoint/rollback_to for partial rollback. On CONFLICT, create a new transaction, read again, and recompute.

<!-- api:query -->
## 7. Filtering, sorting, and aggregation

```python
from fengqudb_query import from_, s64, text
score = s64(Codec.Fields.score)
name = text(Codec.Fields.name)
rows = from_(b"student/").where(score >= 90).select(name, score) \
    .order_by(score.desc()).limit(10).run(db)
for row in rows:
    print(row[name], row[score])
```

Combine conditions with `& / | / ~`, rather than Python's and/or/not. group_by/having aggregates groups, join matches equal field values, and explain shows the query plan.

<!-- api:index -->
## 8. Indexes for repeated field queries

```python
db.define_index(b"student-score", b"student/", Codec.Fields.score, 5)
```

5 means a signed ZigZag integer. Use define_compound_index options for compound indexes and unique/required constraints.

<!-- api:durability -->
## 9. Critical writes: wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```python
db.put(b"payment/receipt", b"paid", force_reliable=True)
db.sync()
db.configure_write(mode="reliable") # Require reliable confirmation for every write
```

Asynchronous submit_put returns a receipt; wait for or poll its result. On COMMIT_UNKNOWN, reconcile by business operation ID instead of charging again.

<!-- api:expiry -->
## 10. Expiring data

```python
import time
expires = int(time.time() * 1000) + 5 * 60_000
db.put(b"verification", b"1234", expires_at_ms=expires)
```

Expiry is an absolute Unix timestamp in milliseconds; 0 means no expiry.

<!-- api:fastread -->
## 11. Hot-path and shared reads

```python
with db.prepare_reader([b"student/7", b"student/8"]) as reader:
    rows = reader.read()
```

Ordinary get returns independently owned bytes. Close shared values. A reader's memoryview remains valid only until the next read/release/close. Copy it if it must live longer.

<!-- api:snapshot -->
## 12. Consistent snapshots

```python
with db.snapshot() as snapshot:
    first = snapshot.get(b"a")
    second = snapshot.get(b"b")
```

<!-- api:backup -->
## 13. Backup and restore

```python
db.backup("backups/full-001")
```

Use a new destination directory. Restore with restore/recover_chain after closing the database being replaced. Do not copy a directory while it is being written.

<!-- api:collections -->
## 14. Native collections

execute runs Hash/Set/List/sorted-set commands; hash_get reads a Hash field. Use them for atomic member changes, queues, and rankings.

<!-- api:maintenance -->
## 15. Cleanup, compaction, and verification

```python
db.purge_expired(max_keys=1000)
db.compact()
db.verify()
```

<!-- api:watch -->
## 16. Subscribe to commit changes

watch returns a consumer; ack the processed watermark and close when done. Use it to update in-process caches and trigger background tasks.

<!-- api:serialization -->
## 17. Entity encoding and decoding

Codec.encode accepts Student; decode returns a record containing your Student. Use row.fq_encode after modification to preserve unknown fields. encode_into reuses a bytearray; clear it before reuse. Generation runs at development startup or build time, not on each database operation.
