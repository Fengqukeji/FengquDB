<!-- api:read -->
## 1. Read

```js
const data = db.get('student/7');
const student = data === null ? null : Codec.decode(data).value;
```

A missing record returns null; an empty Uint8Array is valid data. Codec is StudentFqCodec generated in the getting-started guide.

<!-- api:save -->
## 2. Insert, save, and store strings or integers

```js
db.put(Codec.key(student), Codec.encode(student));
db.put('name', 'Alice');
const number = Buffer.alloc(8); number.writeBigInt64LE(95n);
db.put('score', number);
const score = Buffer.from(db.get('score')).readBigInt64LE();
```

put overwrites the previous value at the same key. Use bigint for 64-bit integers.

<!-- api:modify -->
## 3. Update

```js
const bytes = db.get('student/7');
if (bytes !== null) {
  const row = Codec.decode(bytes);
  row.value.score = 96;
  db.put(row.fqKey(), row.fqEncode());
}
```

Put multi-key updates in a transaction. The Node package currently has no convenience methods for atomic counters or binary patching.

<!-- api:delete -->
## 4. Delete

```js
const removed = db.delete('student/7');
```

<!-- api:batch -->
## 5. Batch updates

```js
const tx = db.transaction();
try { tx.putMany([['a','one'],['b','two']]); tx.commit(); }
finally { tx.close(); }
```

The whole batch is committed as one transaction.

<!-- api:transaction -->
## 6. Transactions and savepoints

```js
const tx = db.transaction();
try {
  tx.put('a','one');
  const point = tx.savepoint();
  tx.put('temporary','discard');
  tx.rollbackTo(point);
  tx.commit();
} finally { tx.close(); }
```

commit closes the transaction; closing without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute.

<!-- api:query -->
## 7. Filtering, sorting, and aggregation

```js
const {from, s64, text} = require('./FengquDB/node');
const score = s64(Codec.fields.score), name = text(Codec.fields.name);
const rows = from('student/').where(score.ge(90n))
  .select(name,score).orderBy(score.desc()).limit(10).run(db);
try { for(const row of rows) console.log(row.get(name),row.get(score)); }
finally { rows.close(); }
```

and/or/not combine conditions, groupBy/having aggregates groups, and join performs equality joins. prepare reuses a query, and explain shows index use. Column types must match the encoding; ordinary number fields use f64 by default.

<!-- api:index -->
## 8. Indexes

```js
db.defineCompoundIndex('student-score', 'student/', [[Codec.fields.score,5]], 0);
```

5 means a signed ZigZag integer.

<!-- api:durability -->
## 9. Critical writes: wait for durable storage

The default performance mode uses 50 ms / 1 MiB. Ordinary success means the change is visible in memory.

A sudden power loss can lose data that has not yet reached durable storage. Wait for reliable confirmation for critical operations, as shown below.

```js
db.put('payment/receipt', 'paid', {forceReliable:true});
db.sync();
```

Use commit(true) for transactions. Switch the whole database with `db.configureWrite({mode:'reliable'})`, or select performance mode with `db.configureWrite({mode:'performance', periodMs:50, thresholdBytes:1024*1024})`. Omitted parameters retain their current values. `db.writeOptions()` reads the settings, and `db.resetWriteOptions()` restores default performance mode. Settings are saved and survive reopening.

The API is synchronous. Run reliable writes and large queries in a worker to avoid blocking the event loop. On COMMIT_UNKNOWN, reconcile by business operation ID instead of charging again.

<!-- api:expiry -->
## 10. Expiring data

```js
db.put('verification', '1234', {expiresAtMs:Date.now()+300_000});
```

Expiry is an absolute Unix timestamp in milliseconds; 0 means no expiry.

<!-- api:fastread -->
## 11. Hot-path reads and shared bytes

```js
db.withShared('student/7', bytes => { if(bytes) consume(bytes); });
const reader = db.prepareReader(['student/7','student/8']);
try { reader.read(); if(reader.found(0)) consume(reader.value(0)); }
finally { reader.close(); }
```

Shared bytes are read-only and must not be used after the callback ends or the lease is released. Reader views remain valid only until the next read/release/close. Copy them if they must live longer.

<!-- api:snapshot -->
## 12. Consistent snapshots

```js
const snapshot = db.snapshot();
try { const value = snapshot.get('student/7'); }
finally { snapshot.close(); }
```

<!-- api:serialization -->
## 13. Entity encoding and decoding

Codec.encode/decode uses your Student. Use the record's fqEncode when rewriting it to preserve unknown fields. encode(value, destination) reuses a Uint8Array and returns the actual encoded range. npm prebuild runs generation; the SDK includes the generator.
