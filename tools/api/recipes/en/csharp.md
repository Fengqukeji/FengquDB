<!-- api:read -->
## 1. Read: find a student or a score

The examples use the shared entry point: `var db = AppDb.Main;`.

```csharp
Student? student = db.GetEntity<Student>(7); // Returns null if the record is missing
int score = db.Get<int>("score");             // Throws if the key is missing
bool found = db.TryGet<int>("score", out int value);
```

Read one field without constructing the entire student object:

```csharp
using var reader = db.PrepareReader(Student.MakeKey(7));
int grade = reader.Read(rows => Student.ReadScore(rows[0].Value));
```

<!-- api:save -->
## 2. Insert or save: entities, numbers, and strings

Put overwrites the previous value at the same key. Student is defined in the getting-started guide.

```csharp
db.Put(new Student { Id=7, Name="Alice", Score=95 });
db.Put("score", 95);
db.Put("name", "Alice");
db.Put("enabled", true);
```

<!-- api:modify -->
## 3. Update: scores, conditional writes, and atomic counters

```csharp
var student = db.GetEntity<Student>(7);
if (student != null) { student.Score = 96; db.Put(student); }
```

Use an atomic operation when multiple threads increment a counter, rather than reading and overwriting the same old value:

```csharp
byte[] key = System.Text.Encoding.UTF8.GetBytes("login-count");
var changed = db.AddInt64(key, 1);
```

AddInt64 uses a dedicated eight-byte integer format; do not call it on a key saved with `Put("score", 95)`. `CompareExchange` updates by version, `PutIfAbsent` writes only when missing, and `Patch` changes raw binary ranges managed by your application.

<!-- api:delete -->
## 4. Delete records

```csharp
db.Delete(Student.MakeKey(7));
db.Delete(System.Text.Encoding.UTF8.GetBytes("score"));
```

<!-- api:batch -->
## 5. Batch: read and write multiple records

```csharp
byte[][] keys = { Student.MakeKey(7), Student.MakeKey(8) };
var values = db.GetMany(keys);
```

Save multiple entities in one transaction to avoid committing them individually.

<!-- api:transaction -->
## 6. Transactions: changes that must succeed together

```csharp
using var tx = db.Begin();
tx.Put("score", 100);
tx.Put("level", 2);
tx.Commit();
```

Disposing without Commit discards the changes. On CONFLICT, create a new transaction, read again, and recompute; do not simply replay stale values.

<!-- api:query -->
## 7. Filtering, sorting, rankings, and aggregation

```csharp
var score = Student.Cols.Score;
var name = Student.Cols.Name;
var rows = Q.From("student/").Where(score >= 90L)
    .Select(name, score).OrderBy(score.Desc()).Limit(10).Run(db);
foreach (var row in rows) Console.WriteLine($"{row.Get(name)} {row.Get(score)}");
```

Combine `QCond` with `&`, `|`, and `!`. `GroupBy/Having` aggregates groups, and `Join` matches records by field. `Explain` shows index use; `Memory/TimeoutMs` limits resources for large queries. All query methods and overloads are listed below.

<!-- api:index -->
## 8. Indexes: find records by field

```csharp
byte[] index = System.Text.Encoding.UTF8.GetBytes("student-score");
byte[] prefix = System.Text.Encoding.UTF8.GetBytes("student/");
db.DefineIndex(index, prefix, 3, 5); // Field 3: signed ZigZag integer
```

Indexes narrow the candidates before the remaining filters run. Configure uniqueness, required fields, and compound-field constraints through index options.

<!-- api:durability -->
## 9. Reliable writes: persist payment records before proceeding

The default performance mode uses 50 ms / 1 MiB and returns after applying writes in memory. For critical writes:

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```csharp
db.Put("payment/receipt", "paid", forceReliable: true);
using (var tx = db.Begin()) { tx.Put("paid", true); tx.Commit(forceReliable: true); }
db.Sync(); // Wait for previous writes to reach durable storage
```

To require reliable confirmation for the whole database:

```csharp
db.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
```

To avoid blocking a UI thread, obtain a receipt from SubmitPut, then await WaitAsync. COMMIT_UNKNOWN means the outcome is uncertain: reconcile by order ID instead of charging again.

<!-- api:expiry -->
## 10. Expiring data: verification codes and temporary state

```csharp
ulong expires = (ulong)DateTimeOffset.UtcNow.AddMinutes(5).ToUnixTimeMilliseconds();
db.Put("verification", "1234", expiresAtMs: expires);
```

Time is an absolute Unix timestamp in milliseconds; 0 means no expiry. Expired records are hidden from reads, and cleanup or compaction later reclaims their space.

<!-- api:fastread -->
## 11. Hot-path reads: reuse keys and read buffers

```csharp
using var scoreReader = db.PrepareValue<int>("score");
int score = scoreReader.Read();
using var entityReader = db.PrepareEntity<Student>(7);
Student student = entityReader.Read();
```

Ordinary Get returns an independently owned value. Dispose shared reads and readers promptly. Do not retain a reader's byte view across the next Read.

<!-- api:snapshot -->
## 12. Consistent reads: use the same point in time

```csharp
using var snapshot = db.Snapshot();
var student = snapshot.GetEntity<Student>(7);
int score = snapshot.Get<int>("score");
```

<!-- api:backup -->
## 13. Backup and restore: save state before an update

```csharp
db.Backup("backups/full-001"); // Use a new destination directory
```

Close the database being replaced and call Restore/RecoverChain. Do not copy a database directory while it is being written.

<!-- api:collections -->
## 14. Collections: Hash, Set, List, and sorted sets

Execute accepts collection commands; HashGet reads a Hash field. Use them for inventory members, deduplication, and rankings without repeatedly loading and rewriting an entire collection. See the types and overloads in this section for command parameters.

<!-- api:maintenance -->
## 15. Compaction, cleanup, and verification

```csharp
db.PurgeExpired();
db.Compact();
db.Verify();
```

Run these during maintenance periods. Use options to limit work per call on large databases.

<!-- api:watch -->
## 16. Change notifications: consume in-process commit changes

Watch creates a consumer; Ack acknowledges the processed watermark, and Dispose closes it. Use it to update application caches or trigger background tasks.

<!-- api:serialization -->
## 17. Encoding, decoding, and manual generation

Normal compilation generates the entity's Decode, EncodeTo, MakeKey, field readers, and Cols without runtime reflection. Keep existing field numbers unchanged.

See this language's getting-started guide for tool placement and double-click instructions. The native tool reads `// fqdb:entity prefix=student/ key=Id` and generates a separate codec, whose calls differ from compiler-generated entity methods. Use the manual fallback like this, without runtime reflection:

```csharp
byte[] key = StudentFqCodec.Key(student);
db.Put(key, StudentFqCodec.Encode(student));
var row = StudentFqCodec.Decode(db.Get(key)!);
row.Value.Score++;
db.Put(row.Key(), row.Encode());
```

Use the decoded record's Encode when rewriting it to preserve unknown fields. A reused encoding buffer must not overlap the entity's byte fields.
