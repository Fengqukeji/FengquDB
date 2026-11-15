<!-- api:read -->
## 1. Read

```go
data, err := st.Get([]byte("student/7"))
if errors.Is(err, fengqudb.ErrNotFound) { /* No such record */ }
if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data)
```

Import errors. ErrNotFound distinguishes missing records; an empty slice is a valid value.

<!-- api:save -->
## 2. Insert, save, and store strings or integers

```go
if err := st.Put([]byte("name"), []byte("Alice")); err != nil { return err }
var bytes [8]byte
binary.LittleEndian.PutUint64(bytes[:], uint64(95))
if err := st.Put([]byte("score"), bytes[:]); err != nil { return err }
```

The integer example uses encoding/binary. Put overwrites the previous value at the same key; see the getting-started guide for entity encoding.

<!-- api:modify -->
## 3. Update a score

```go
data, err := st.Get([]byte("student/7")); if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data); if err != nil { return err }
row.Value.Score = 96
data, err = row.FqEncode(); if err != nil { return err }
if err = st.Put([]byte("student/7"), data); err != nil { return err }
```

Use the record's FqEncode when rewriting it to preserve unknown fields.

<!-- api:delete -->
## 4. Delete

```go
if err := st.Delete([]byte("student/7")); err != nil { return err }
```

<!-- api:transaction -->
## 5. Transactions and atomic batch writes

```go
tx, err := st.Begin(0); if err != nil { return err }; defer tx.Close()
if err = tx.Put([]byte("a"), []byte("one")); err != nil { return err }
if err = tx.Put([]byte("b"), []byte("two")); err != nil { return err }
_, err = tx.Commit(); if err != nil { return err }
```

Closing without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute. PutMany writes multiple key-value pairs in one transaction.

<!-- api:query -->
## 6. Conditional queries and sorting

```go
score := fengqudb.S64(fqmodels.StudentFieldScore)
name := fengqudb.Text(fqmodels.StudentFieldName)
rows, err := fengqudb.From("student/").Where(score.Ge(90)).
    Select(name,score).OrderBy(score.Desc()).Limit(10).Run(st)
if err != nil { return err }; defer rows.Close()
```

And/Or/Not combine conditions, GroupBy/Having aggregates groups, Join matches records, and Explain shows the query plan.

<!-- api:index -->
## 7. Indexes

DefineCompoundIndex indexes frequently filtered fields; DropIndex removes an index. Conditional queries use available indexes.

<!-- api:durability -->
## 8. Wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary write success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```go
if err := st.Put([]byte("saved"), []byte("yes")); err != nil { return err }
if err := st.Sync(); err != nil { return err }
```

The Go package has no per-write forceReliable parameter. Sync is an explicit durability barrier; Put alone is not reliable confirmation. Set database-wide reliable mode before writing:

```go
options, err := st.WriteOptions(); if err != nil { return err }
options.Mode = fengqudb.WriteReliable
if err := st.ConfigureWrite(options); err != nil { return err }
```

WriteOptions reads the current settings; ResetWriteOptions restores default performance mode. Settings are saved and survive reopening.

<!-- api:expiry -->
## 9. Temporary data

PutExpiring takes an absolute Unix timestamp in milliseconds; 0 means no expiry. Use it for verification codes, temporary state, and short-lived caches.

<!-- api:fastread -->
## 10. Reduce hot-path read allocations

```go
shared, err := st.GetShared([]byte("student/7"))
if err != nil { return err }; defer shared.Close()
```

Close shared views. Prepared keys and readers reuse keys and batch rows. Reader rows remain valid only until the next Read/Release/Close.

<!-- api:snapshot -->
## 11. Consistent snapshots

Snapshot keeps reads on one version; Close it when done. Do not retain snapshots you no longer need.

<!-- api:serialization -->
## 12. Entity encoding and decoding

StudentFqEncode/StudentFqDecode use your models.Student. StudentFqEncodeInto accepts a reusable slice; pass dst[:0] to reuse its capacity. Commit the field-number lock with your application sources and do not reassign existing field numbers.

The Go client currently has no high-level backup, maintenance, collection, or change-subscription methods. This page lists the Go APIs you can call directly.
