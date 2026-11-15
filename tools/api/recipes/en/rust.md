<!-- api:read -->
## 1. Read

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let student = Codec::decode(&bytes)?.value;
}
```

A missing record is None; an empty Vec is valid data. Codec is StudentFqCodec from the getting-started guide.

<!-- api:save -->
## 2. Insert, save, and store strings or integers

```rust
db.put(b"name", "Alice".as_bytes())?;
db.put(b"score", &95_i64.to_le_bytes())?;
let score = db.get(b"score")?
    .map(|b| b.as_slice().try_into().map(i64::from_le_bytes)).transpose()?;
db.put(&Codec::key(&student)?, &Codec::encode(&student)?)?;
```

put overwrites the previous value at the same key. The integer example needs a Result that can represent length-conversion errors.

<!-- api:modify -->
## 3. Update a score

```rust
if let Some(bytes) = db.get(b"student/7")? {
    let mut row = Codec::decode(&bytes)?;
    row.value.score = 96;
    db.put(&row.fq_key()?, &row.fq_encode()?)?;
}
```

The record's fq_encode preserves unknown fields.

<!-- api:delete -->
## 4. Delete

```rust
db.delete(b"student/7")?;
```

<!-- api:transaction -->
## 5. Transactions and atomic batch writes

```rust
let mut tx = db.begin(0)?;
tx.put(b"a", b"one")?;
tx.put(b"b", b"two")?;
tx.commit()?;
```

commit consumes the transaction; Drop without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute. put_many stages multiple changes in a batch.

<!-- api:query -->
## 6. Filtering, sorting, and aggregation

```rust
use fengqudb::builder;
let score = builder::sint64(Codec::FIELD_SCORE, 0);
let name = builder::text(Codec::FIELD_NAME, 0);
let rows = builder::from_(b"student/".to_vec()).where_(score.ge(90_i64))
    .select(&[&name, &score]).order_by([score.desc()]).limit(10).run(&db)?;
```

Combine conditions, group_by/having, and join for more complex queries. prepare reuses a query, and explain shows the execution plan.

<!-- api:index -->
## 7. Indexes

define_compound_index creates field or compound indexes; drop_index removes them. Index field types must match the entity encoding.

<!-- api:durability -->
## 8. Wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```rust
db.put(b"saved", b"yes")?;
db.sync()?;
```

The safe client has no per-write forceReliable parameter. sync is an explicit durability barrier. Set database-wide reliable mode before writing:

```rust
let mut options = db.write_options()?;
options.mode = fengqudb::WriteMode::Reliable;
db.configure_write(&options)?;
```

write_options reads the settings; reset_write_options restores default performance mode. Settings are saved and survive reopening.

<!-- api:expiry -->
## 9. Expiring data

put_with_expiry takes an absolute Unix timestamp in milliseconds; 0 means no expiry. Use it for temporary state and short-lived caches.

<!-- api:fastread -->
## 10. Hot-path shared reads

```rust
if let Some(view) = db.get_shared(b"student/7")? {
    consume(view.as_bytes());
}
```

Borrowed bytes become invalid when the lease is dropped. Reader's Rows borrow prevents using old rows across the next read. Copy data if it must live longer.

<!-- api:snapshot -->
## 11. Consistent snapshots

snapshot creates a consistent view and Drop releases it. Do not retain snapshots you no longer use.

<!-- api:serialization -->
## 12. Entity encoding and decoding

Codec::encode_into accepts a reusable Vec; clear it before encoding to retain capacity. Struct fields must be pub and the struct must implement Default. Generation runs in cargo build's build script, not during database operations.

The safe client currently has no high-level backup, collection, maintenance, or subscription methods. fengqudb_sys at the end is the low-level unsafe API; application code should prefer the safe methods above.
