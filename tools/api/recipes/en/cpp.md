<!-- api:read -->
## 1. Read: find data by key

```cpp
if (auto value = db.get("student/7")) {
    auto row = Codec::decode(value->view());
    auto grade = row.value.score;
}
```

A missing record returns an empty optional. Codec in this page refers to StudentFqCodec generated in the getting-started guide.

<!-- api:save -->
## 2. Insert or save

```cpp
Student student{7, "Alice", 95};
db.put(Codec::key(student), Codec::encode(student));
db.put("name", "Alice"); // Store a single string directly as UTF-8
```

put overwrites the previous value at the same key.

<!-- api:modify -->
## 3. Updates and atomic operations

```cpp
if (auto bytes = db.get("student/7")) {
    auto row = Codec::decode(bytes->view());
    row.value.score = 96;
    db.put(Codec::key(row.value), Codec::encode(row));
}
db.add_i64("login-count", 1);
```

add_i64 uses eight-byte little-endian integers; do not apply it to encoded entities. compare_exchange updates by version, put_if_absent inserts only when missing, and patch changes raw byte ranges.

<!-- api:delete -->
## 4. Delete

```cpp
db.erase("student/7");
```

<!-- api:batch -->
## 5. Batch operations

```cpp
db.put_many({{"a", "one"}, {"b", "two"}});
auto values = db.get_many({"a", "b"});
```

put_many is one atomic batch write.

<!-- api:transaction -->
## 6. Transactions: commit changes together

```cpp
auto tx = db.transaction();
tx.put("bag/item", "sword");
tx.erase("reward/pending");
auto result = tx.commit();
if (!result) throw fengqudb::Error(static_cast<fqdb_status>(result.status));
```

Destroying an uncommitted transaction discards its changes. On CONFLICT, create a new transaction, read again, and recompute.

<!-- api:query -->
## 7. Filtering, sorting, and aggregation

```cpp
#include <fengqudb_query.hpp>
namespace q = fengqudb::q;
auto score = q::s64(Codec::Field_score);
auto name = q::text(Codec::Field_name);
auto query = q::from("student/").where(score >= 90)
    .select(name, score).order_by(score.desc()).limit(10);
auto rows = q::run(db, query);
for (auto row : rows) { auto grade = row[score]; }
```

Combine conditions, aggregates, groups, and equality joins. prepare reuses a query; explain shows its execution plan.

<!-- api:index -->
## 8. Indexes for frequent field queries

```cpp
db.define_index("student-score", "student/", Codec::Field_score, 5);
```

Field type 5 means a signed ZigZag integer. See the parameters in this section for other types and index options.

<!-- api:durability -->
## 9. Reliable writes

The default performance mode uses 50 ms / 1 MiB and returns after applying the write in memory. Pass the final reliability flag for critical writes:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```cpp
db.put("payment/receipt", "paid", 0, true);
db.sync();
auto pending = db.submit_put("payment/next", "paid", 0, true);
auto result = pending.wait();
```

A UI thread can retain the receipt and poll later. On COMMIT_UNKNOWN, reconcile by business operation ID instead of blindly replaying a payment.

<!-- api:expiry -->
## 10. Automatic expiry

```cpp
db.put("verification", "1234", expires_at_unix_ms);
```

Time is an absolute Unix timestamp in milliseconds; 0 means no expiry.

<!-- api:fastread -->
## 11. Hot-path reads

```cpp
auto key = db.prepare_key("student/7");
auto value = key.get();
auto reader = db.prepare_reader({"student/7", "student/8"});
auto rows = reader.read();
```

Shared bytes are borrowed and read-only; keep their owner alive. Reader rows remain valid only until the next read/release. Copy them if they must live longer.

<!-- api:snapshot -->
## 12. Consistent snapshots

```cpp
auto snapshot = db.snapshot();
auto a = snapshot.get("a");
auto b = snapshot.get("b");
```

<!-- api:backup -->
## 13. Backup and restore

```cpp
db.backup("backups/full-001");
```

Use a new destination directory. Restore with restore/recover_chain after closing the database being replaced. Do not copy files while they are being written.

<!-- api:collections -->
## 14. Collection operations

execute accepts Hash/Set/List/sorted-set commands; hash_get reads a Hash field. Use them for atomic member changes, queues, and rankings.

<!-- api:maintenance -->
## 15. Cleanup and compaction

```cpp
db.purge_expired();
db.compact();
db.verify();
```

<!-- api:watch -->
## 16. Consume changes

watch creates a change consumer; acknowledge its watermark after processing. Use it to update in-process caches and trigger background tasks. Handles use RAII.

<!-- api:serialization -->
## 17. Entity encoding and decoding

Codec::encode/encode_into accesses your struct directly; decode returns the original struct and unknown fields. Use encode(row) when rewriting a record to preserve fields added by newer versions. Reuse a string or caller-owned buffer; output must not overlap input fields.
