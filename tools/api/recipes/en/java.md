<!-- api:read -->
## 1. Read a student

```java
byte[] bytes = db.get("student/7".getBytes(StandardCharsets.UTF_8));
Student student = bytes == null ? null : StudentFqCodec.decode(bytes).value;
```

A missing record returns null; an empty array is valid data. This page uses `java.nio.charset.StandardCharsets`.

<!-- api:save -->
## 2. Insert, save, and store a string or integer

```java
Student student = new Student();
student.id=7; student.name="Alice"; student.score=95;
db.put(StudentFqCodec.key(student), StudentFqCodec.encode(student));
db.put("name".getBytes(StandardCharsets.UTF_8), "Alice".getBytes(StandardCharsets.UTF_8));
```

A single integer needs no entity; use eight-byte little-endian encoding:

```java
byte[] key = "score".getBytes(StandardCharsets.UTF_8);
byte[] value = java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(95).array();
db.put(key, value);
long score = java.nio.ByteBuffer.wrap(db.get(key)).order(java.nio.ByteOrder.LITTLE_ENDIAN).getLong();
```

<!-- api:modify -->
## 3. Update scores and use atomic counters

```java
byte[] key = "student/7".getBytes(StandardCharsets.UTF_8);
byte[] bytes = db.get(key);
if (bytes != null) {
    StudentFqCodec.Record row = StudentFqCodec.decode(bytes);
    row.value.score = 96;
    db.put(row.fqKey(), row.fqEncode());
}
db.addInt64("login-count".getBytes(StandardCharsets.UTF_8), 1);
```

addInt64 is only for eight-byte integers. compareExchange updates by version; patch changes raw byte ranges managed by your application.

<!-- api:delete -->
## 4. Delete

```java
db.delete("student/7".getBytes(StandardCharsets.UTF_8));
```

<!-- api:batch -->
## 5. Batch reads and writes

```java
byte[][] keys = { "student/7".getBytes(StandardCharsets.UTF_8), "student/8".getBytes(StandardCharsets.UTF_8) };
byte[][] values = db.getMany(keys);
```

putMany/deleteMany apply multiple changes in one atomic commit.

<!-- api:transaction -->
## 6. Transactions: changes that succeed together

```java
try (Store.Transaction tx = db.begin()) {
    tx.put("a".getBytes(StandardCharsets.UTF_8), new byte[]{1});
    tx.put("b".getBytes(StandardCharsets.UTF_8), new byte[]{2});
    tx.commit();
}
```

Closing without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute.

<!-- api:query -->
## 7. Conditional queries and sorting

```java
var score = io.fengqudb.Q.s64(StudentFqCodec.Field_score);
var name = io.fengqudb.Q.text(StudentFqCodec.Field_name);
var rows = io.fengqudb.Q.from("student/").where(score.ge(90L))
    .select(name, score).orderBy(score.desc()).limit(10).run(db);
```

Combine conditions with and/or/not; groupBy/having aggregates groups, join performs equality joins, and explain shows index selection.

<!-- api:index -->
## 8. Indexes

```java
db.defineIndex("student-score".getBytes(StandardCharsets.UTF_8),
    "student/".getBytes(StandardCharsets.UTF_8), StudentFqCodec.Field_score, 5, 0);
```

5 means a signed ZigZag integer.

<!-- api:durability -->
## 9. Critical writes such as payments: reliable confirmation

The default performance mode uses 50 ms / 1 MiB. Ordinary writes acknowledge in-memory visibility. Pass true for critical writes:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```java
db.put("payment/receipt".getBytes(StandardCharsets.UTF_8), "paid".getBytes(StandardCharsets.UTF_8), true);
db.sync();
```

Use commit(true) for transactions. submitPut returns a receipt that you must wait for or poll. On COMMIT_UNKNOWN, reconcile by business operation ID instead of charging again.

<!-- api:expiry -->
## 10. Temporary data and expiry

Use the overload with expiresAtMs. `System.currentTimeMillis() + 300_000` expires after five minutes; 0 means no expiry.

<!-- api:fastread -->
## 11. Frequent reads

```java
try (var key = db.prepareKey("student/7".getBytes(StandardCharsets.UTF_8))) {
    byte[] bytes = key.get();
}
```

Ordinary get returns an independent array. Close shared ByteBuffer leases and readers promptly; do not use derived views after closing them.

<!-- api:snapshot -->
## 12. Consistent snapshots

```java
try (var snapshot = db.snapshot()) {
    byte[] value = snapshot.get("student/7".getBytes(StandardCharsets.UTF_8));
}
```

<!-- api:backup -->
## 13. Backup and restore

```java
db.backup("backups/full-001");
```

Use a new backup directory. Restore with restore/recoverChain after closing the database being replaced.

<!-- api:collections -->
## 14. Hash, Set, List, and sorted sets

execute runs collection commands; hashGet reads a Hash field. Use them for atomic member operations, queues, and rankings.

<!-- api:maintenance -->
## 15. Cleanup and compaction

```java
db.purgeExpired();
db.compact();
db.verify();
```

<!-- api:watch -->
## 16. Commit changes

watch creates a consumer; ack after processing and close when done. Use it to update in-process application caches.

<!-- api:serialization -->
## 17. Entity encoding and decoding

StudentFqCodec.encode/decode accesses Student directly. The record's fqEncode preserves unknown fields; encodeInto(value, buffer, offset) reuses an array. Generation runs before Gradle compilation, using the native tool included in the SDK.
