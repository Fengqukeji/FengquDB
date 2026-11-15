# Go API and examples

[Getting started](../languages/go.md) · [简体中文](../../api/go.md)

Start with the common operations below. Each section combines purpose, examples, and methods. Expand “Functions and parameters” for all overloads.

## 1. Read

```go
data, err := st.Get([]byte("student/7"))
if errors.Is(err, fengqudb.ErrNotFound) { /* No such record */ }
if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data)
```

Import errors. ErrNotFound distinguishes missing records; an empty slice is a valid value.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `// Get returns an independent copy; a miss returns ErrNotFound. An empty value returns a non-nil empty slice. func (s *Store) Get(key []byte) ([]byte, error)` | Read data or status |
| `fengqudb.Store` | `// GetAppend appends the value to dst and returns it; no allocation if capacity is sufficient, with one cgo round trip. // A miss returns (dst, ErrNotFound). func (s *Store) GetAppend(dst, key []byte) ([]byte, error)` | Read data or status |

</details>

## 2. Insert, save, and store strings or integers

```go
if err := st.Put([]byte("name"), []byte("Alice")); err != nil { return err }
var bytes [8]byte
binary.LittleEndian.PutUint64(bytes[:], uint64(95))
if err := st.Put([]byte("score"), bytes[:]); err != nil { return err }
```

The integer example uses encoding/binary. Put overwrites the previous value at the same key; see the getting-started guide for entity encoding.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `// Put writes a key-value pair without expiry. func (s *Store) Put(key, value []byte) error` | Save or set data |
| `fengqudb.Store` | `// PutExpiring writes a key-value pair; expiresAtMs is absolute Unix milliseconds, and 0 means no expiry. func (s *Store) PutExpiring(key, value []byte, expiresAtMs uint64) error` | Save or set data |

</details>

## 3. Update a score

```go
data, err := st.Get([]byte("student/7")); if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data); if err != nil { return err }
row.Value.Score = 96
data, err = row.FqEncode(); if err != nil { return err }
if err = st.Put([]byte("student/7"), data); err != nil { return err }
```

Use the record's FqEncode when rewriting it to preserve unknown fields.

## 4. Delete

```go
if err := st.Delete([]byte("student/7")); err != nil { return err }
```

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `// Delete deletes a key. func (s *Store) Delete(key []byte) error` | Delete a record or member |

</details>

## 5. Transactions and atomic batch writes

```go
tx, err := st.Begin(0); if err != nil { return err }; defer tx.Close()
if err = tx.Put([]byte("a"), []byte("one")); err != nil { return err }
if err = tx.Put([]byte("b"), []byte("two")); err != nil { return err }
_, err = tx.Commit(); if err != nil { return err }
```

Closing without a commit discards changes. On CONFLICT, create a new transaction, read again, and recompute. PutMany writes multiple key-value pairs in one transaction.

<details>
<summary>Functions and parameters (9)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `// Begin starts a transaction; timeoutMs=0 means no deadline. func (s *Store) Begin(timeoutMs uint64) (*Txn, error)` | Transaction operation |
| `fengqudb.Txn` | `// Close closes the transaction, discarding uncommitted changes; repeated calls have no effect. func (t *Txn) Close()` | Release an object or borrowed resource |
| `fengqudb.Txn` | `// Commit commits the transaction and returns its version; Close is still required afterward. func (t *Txn) Commit() (uint64, error)` | Commit a transaction or inspect its result |
| `fengqudb.Txn` | `// Delete deletes a key within the transaction. func (t *Txn) Delete(key []byte) error` | Delete a record or member |
| `fengqudb.Txn` | `// Get copies a value visible to the transaction, including its private writes; a miss returns ErrNotFound. func (t *Txn) Get(key []byte) ([]byte, error)` | Read data or status |
| `fengqudb.Txn` | `// Put writes a key-value pair within the transaction. func (t *Txn) Put(key, value []byte) error` | Save or set data |
| `fengqudb.Txn` | `// PutExpiring writes within the transaction with absolute Unix expiry in milliseconds. func (t *Txn) PutExpiring(key, value []byte, expiresAtMs uint64) error` | Save or set data |
| `fengqudb.Txn` | `// PutMany commits multiple writes together; keys and values are copied to C memory before the core validates the whole batch. func (t *Txn) PutMany(items []PutItem) error` | Save or set data |
| `fengqudb.Txn` | `// SelectRequest runs SELECT/EXPLAIN inside the transaction, seeing private writes and registering conflict dependencies. func (t *Txn) SelectRequest(request []byte, out *Result) error` | Transaction operation |

</details>

## 6. Conditional queries and sorting

```go
score := fengqudb.S64(fqmodels.StudentFieldScore)
name := fengqudb.Text(fqmodels.StudentFieldName)
rows, err := fengqudb.From("student/").Where(score.Ge(90)).
    Select(name,score).OrderBy(score.Desc()).Limit(10).Run(st)
if err != nil { return err }; defer rows.Close()
```

And/Or/Not combine conditions, GroupBy/Having aggregates groups, Join matches records, and Explain shows the query plan.

<details>
<summary>Functions and parameters (84)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `func Avg(c Col) Agg` | Build a query or read its results |
| `fengqudb` | `func Count() Agg` | Build a query or read its results |
| `fengqudb` | `func CountDistinct(c Col) Agg` | Build a query or read its results |
| `fengqudb` | `func CountOf(c Col) Agg` | Build a query or read its results |
| `fengqudb` | `func F32(n uint64) Col` | Build a query or read its results |
| `fengqudb` | `func F64(n uint64) Col` | Build a query or read its results |
| `fengqudb` | `// From starts a builder with a string prefix. func From(prefix string) *Builder` | Build a query or read its results |
| `fengqudb` | `func I64(n uint64) Col` | Build a query or read its results |
| `fengqudb` | `func Max(c Col) Agg` | Build a query or read its results |
| `fengqudb` | `func Min(c Col) Agg` | Build a query or read its results |
| `fengqudb` | `// NewQuery returns a query with default limits, matching the Python Query defaults. func NewQuery(prefix []byte) *Query` | Build a query or read its results |
| `fengqudb` | `func S64(n uint64) Col` | Build a query or read its results |
| `fengqudb` | `func Sum(c Col) Agg` | Build a query or read its results |
| `fengqudb` | `func Text(n uint64) Col` | Build a query or read its results |
| `fengqudb` | `// Field factories: names correspond to protobuf wire types. func U64(n uint64) Col` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Asc(nulls ...bool) Sort` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Desc(nulls ...bool) Sort` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Eq(v any) Cond` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Ge(v any) Cond` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Gt(v any) Cond` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Le(v any) Cond` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Lt(v any) Cond` | Build a query or read its results |
| `fengqudb.Agg` | `func (a Agg) Ne(v any) Cond` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) After(cursor []byte) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) AllowScan(on bool) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `// Build returns the equivalent low-level Query. func (b *Builder) Build() (*Query, error)` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) CountTotal(on bool) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Distinct(on bool) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `// Explain returns the access plan. func (b *Builder) Explain(db Database) (*Result, error)` | Inspect the query plan |
| `fengqudb.Builder` | `func (b *Builder) GroupBy(cols ...Col) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Having(c Cond) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Index(name string) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `// Join adds a join: left is a field from the primary or an earlier source, right is a field from the joined prefix. func (b *Builder) Join(prefix string, left, right Col, kind JoinKind) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Limit(n uint64) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Memory(n uint64) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Offset(n uint64) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) OrderBy(items ...Orderable) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `// Pages executes page by page and calls fn for each page; see Query.Pages. func (b *Builder) Pages(db Database, fn func(*Rows) error) error` | Build a query or read its results |
| `fengqudb.Builder` | `// Prepare encodes once for repeated execution. func (b *Builder) Prepare() (*PreparedQuery, error)` | Build a query or read its results |
| `fengqudb.Builder` | `// Run executes the query; Close the returned Rows when done. func (b *Builder) Run(db Database) (*Rows, error)` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Select(items ...Selectable) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) TimeoutMs(n uint64) *Builder` | Build a query or read its results |
| `fengqudb.Builder` | `func (b *Builder) Where(c Cond) *Builder` | Build a query or read its results |
| `fengqudb.Col` | `// Any marks a protobuf repeated field: any matching element makes the condition true. func (c Col) Any() Col` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Asc(nulls ...bool) Sort` | Build a query or read its results |
| `fengqudb.Col` | `// Between is equivalent to Ge(low) AND Le(high). func (c Col) Between(low, high any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Contains(s string) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Desc(nulls ...bool) Sort` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) EndsWith(s string) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Eq(v any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Exists() Cond` | Build a query or read its results |
| `fengqudb.Col` | `// Fuzzy tests whether the entire UTF-8 field has edit distance at most distance (0..8) from s. func (c Col) Fuzzy(s string, distance int) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Ge(v any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Gt(v any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) IContains(s string) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) ILike(s string) Cond` | Build a query or read its results |
| `fengqudb.Col` | `// In tests whether the field value belongs to the given set. func (c Col) In(values ...any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `// InSource returns the same field referencing source (0 for the primary prefix, 1..7 for joins). func (c Col) InSource(source uint64) Col` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) IsNull() Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Le(v any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Like(s string) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Lt(v any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Ne(v any) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) Regex(s string) Cond` | Build a query or read its results |
| `fengqudb.Col` | `func (c Col) StartsWith(s string) Cond` | Build a query or read its results |
| `fengqudb.Cond` | `// And returns c AND o. func (c Cond) And(o Cond) Cond` | Build a query or read its results |
| `fengqudb.Cond` | `// Not returns NOT c. func (c Cond) Not() Cond` | Build a query or read its results |
| `fengqudb.Cond` | `// Or returns c OR o. func (c Cond) Or(o Cond) Cond` | Build a query or read its results |
| `fengqudb.PreparedQuery` | `// Execute runs the query and returns a new Result; Close it when done. func (p *PreparedQuery) Execute(db Database) (*Result, error)` | Build a query or read its results |
| `fengqudb.PreparedQuery` | `// ExecuteInto runs the query into reusable out, with no hot-path allocation. func (p *PreparedQuery) ExecuteInto(db Database, out *Result) error` | Build a query or read its results |
| `fengqudb.PreparedQuery` | `// Explain returns the access plan (EXPLAIN). func (p *PreparedQuery) Explain(db Database) (*Result, error)` | Inspect the query plan |
| `fengqudb.PreparedQuery` | `// Request returns complete SELECT (explain=false) or EXPLAIN request bytes. func (p *PreparedQuery) Request(explain bool) []byte` | Build a query or read its results |
| `fengqudb.Query` | `// AppendEncode appends the encoded Query to b. func (q *Query) AppendEncode(b []byte) ([]byte, error)` | Build a query or read its results |
| `fengqudb.Query` | `// Encode returns the encoded Query bytes. func (q *Query) Encode() ([]byte, error)` | Build a query or read its results |
| `fengqudb.Query` | `// Execute encodes and runs the query. func (q *Query) Execute(db Database) (*Result, error)` | Build a query or read its results |
| `fengqudb.Query` | `// Explain encodes and executes EXPLAIN. func (q *Query) Explain(db Database) (*Result, error)` | Inspect the query plan |
| `fengqudb.Query` | `// Pages starts at q.After, calls fn for each page, and uses the returned cursor as the next After until empty. // An error from fn stops iteration. q is unchanged; each Result is closed after fn returns. func (q *Query) Pages(db Database, fn func(*Result) error) error` | Build a query or read its results |
| `fengqudb.Query` | `// Prepare encodes the query and builds SELECT / EXPLAIN requests. func (q *Query) Prepare() (*PreparedQuery, error)` | Build a query or read its results |
| `fengqudb.Row` | `// ExpiresAtMs returns absolute Unix expiry in milliseconds; 0 means no expiry. func (r *Row) ExpiresAtMs() uint64` | Build a query or read its results |
| `fengqudb.Row` | `// Found distinguishes a hit from a miss; an empty value is a hit. func (r *Row) Found() bool` | Build a query or read its results |
| `fengqudb.Row` | `// Value returns a read-only view; a miss returns nil, and an empty value returns a non-nil empty slice. func (r *Row) Value() []byte` | Build a query or read its results |
| `fengqudb.Row` | `// Version returns the value's version. func (r *Row) Version() uint64` | Build a query or read its results |
| `fengqudb.Rows` | `// Value returns the selected column's value in the current row. func (r *Rows) Value(item Selectable) (Value, error)` | Build a query or read its results |
| `fengqudb.Store` | `// SelectRequest executes a read-only SELECT/EXPLAIN using complete outer request bytes (see EncodeRequest). // The result is stored in native buffer out after releasing its previous contents. func (s *Store) SelectRequest(request []byte, out *Result) error` | Build a query or read its results |

</details>

## 7. Indexes

DefineCompoundIndex indexes frequently filtered fields; DropIndex removes an index. Conditional queries use available indexes.

<details>
<summary>Functions and parameters (2)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `// DefineCompoundIndex defines a compound index for keys starting with prefix. func (s *Store) DefineCompoundIndex(name, prefix []byte, fields []IndexField, flags uint32) error` | Define or use an index |
| `fengqudb.Store` | `// DropIndex removes an index. func (s *Store) DropIndex(name []byte) error` | Define or use an index |

</details>

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

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `// DefaultWriteOptions returns new-database defaults: performance mode, 50 ms, 1 MiB. func DefaultWriteOptions() WriteOptions` | Control durability or wait for a commit |
| `fengqudb.Store` | `// ConfigureWrite changes and saves write settings; it may wait for pending writes to become durable. // To retain other settings, read WriteOptions first and change only the desired fields. func (s *Store) ConfigureWrite(options WriteOptions) error` | Control durability or wait for a commit |
| `fengqudb.Store` | `// ResetWriteOptions restores and saves default performance mode, 50 ms, 1 MiB. func (s *Store) ResetWriteOptions() error` | Control durability or wait for a commit |
| `fengqudb.Store` | `// Sync flushes committed data to durable storage. func (s *Store) Sync() error` | Control durability or wait for a commit |
| `fengqudb.Store` | `// WriteOptions reads the current database write settings. func (s *Store) WriteOptions() (WriteOptions, error)` | Control durability or wait for a commit |

</details>

## 9. Temporary data

PutExpiring takes an absolute Unix timestamp in milliseconds; 0 means no expiry. Use it for verification codes, temporary state, and short-lived caches.

## 10. Reduce hot-path read allocations

```go
shared, err := st.GetShared([]byte("student/7"))
if err != nil { return err }; defer shared.Close()
```

Close shared views. Prepared keys and readers reuse keys and batch rows. Reader rows remain valid only until the next Read/Release/Close.

<details>
<summary>Functions and parameters (17)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.PreparedKey` | `// Close releases the prepared key; repeated calls have no effect. Previously acquired Shared values remain valid. func (p *PreparedKey) Close()` | Release an object or borrowed resource |
| `fengqudb.PreparedKey` | `// GetShared returns a shared view of the current value; a miss returns ErrNotFound. Close it when done. func (p *PreparedKey) GetShared() (Shared, error)` | Read data or status |
| `fengqudb.Reader` | `// Acquire reads the whole batch into an independently owned result set. func (rd *Reader) Acquire() (*Results, error)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Reader` | `// Close closes the reader; repeated calls have no effect. func (rd *Reader) Close()` | Release an object or borrowed resource |
| `fengqudb.Reader` | `// Read reads the whole key batch and returns rows in key order. func (rd *Reader) Read() ([]Row, error)` | Read data or status |
| `fengqudb.Reader` | `// Release releases the retained result while keeping prepared keys. func (rd *Reader) Release()` | Release an object or borrowed resource |
| `fengqudb.Shared` | `// Bytes returns a read-only slice over native memory; an empty value returns a non-nil empty slice. func (s *Shared) Bytes() []byte` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Shared` | `// Close returns the lease; repeated calls on the same Shared have no effect. func (s *Shared) Close()` | Release an object or borrowed resource |
| `fengqudb.Shared` | `// Escape transfers lease ownership to a heap object with a finalizer, allocating once. // The original Shared becomes invalid; do not call Close on it afterward. func (s *Shared) Escape() *SharedValue` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Shared` | `// ExpiresAtMs returns absolute Unix expiry in milliseconds; 0 means no expiry. func (s *Shared) ExpiresAtMs() uint64` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Shared` | `// Len returns the value's byte length. func (s *Shared) Len() int` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Shared` | `// Text returns a zero-copy string view; like Bytes, it is valid only until Close. func (s *Shared) Text() string` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Shared` | `// Version returns this value's version. func (s *Shared) Version() uint64` | Reuse keys, buffers, or shared bytes |
| `fengqudb.SharedValue` | `// Close returns the lease and removes its finalizer. func (v *SharedValue) Close()` | Release an object or borrowed resource |
| `fengqudb.Store` | `// GetShared returns a read-only native-memory view; a miss returns ErrNotFound. Shared.Close is required when done. func (s *Store) GetShared(key []byte) (Shared, error)` | Read data or status |
| `fengqudb.Store` | `// PrepareKey creates a prepared key. func (s *Store) PrepareKey(key []byte) (*PreparedKey, error)` | Reuse keys, buffers, or shared bytes |
| `fengqudb.Store` | `// PrepareReader creates a batch reader for 1..4096 keys, preserving order, duplicate keys, and empty values. func (s *Store) PrepareReader(keys [][]byte) (*Reader, error)` | Reuse keys, buffers, or shared bytes |

</details>

## 11. Consistent snapshots

Snapshot keeps reads on one version; Close it when done. Do not retain snapshots you no longer need.

<details>
<summary>Functions and parameters (5)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Result` | `// Cursor returns the pagination cursor as a native view; empty means no more pages. Copy it into Query.After to continue. func (r *Result) Cursor() ([]byte, error)` | Consistent reads or range iteration |
| `fengqudb.Snapshot` | `// Close closes the snapshot; previously acquired shared views remain valid. func (sn *Snapshot) Close()` | Release an object or borrowed resource |
| `fengqudb.Snapshot` | `// Get copies a value from the snapshot. func (sn *Snapshot) Get(key []byte) ([]byte, error)` | Read data or status |
| `fengqudb.Snapshot` | `// GetShared returns a shared view of a snapshot value. func (sn *Snapshot) GetShared(key []byte) (Shared, error)` | Read data or status |
| `fengqudb.Store` | `// Snapshot creates a snapshot; expiry is evaluated at snapshot creation time. func (s *Store) Snapshot() (*Snapshot, error)` | Consistent reads or range iteration |

</details>

## maintenance

<details>
<summary>Functions and parameters (1)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb.Store` | `// Info returns Store statistics. func (s *Store) Info() (Info, error)` | Inspect status or maintain the database |

</details>

## 12. Entity encoding and decoding

StudentFqEncode/StudentFqDecode use your models.Student. StudentFqEncodeInto accepts a reusable slice; pass dst[:0] to reuse its capacity. Commit the field-number lock with your application sources and do not reassign existing field numbers.

The Go client currently has no high-level backup, maintenance, collection, or change-subscription methods. This page lists the Go APIs you can call directly.

## Other utilities

<details>
<summary>Functions and parameters (44)</summary>

| Owner | Function and parameters | Purpose |
| --- | --- | --- |
| `fengqudb` | `// ABIVersion returns the loaded core ABI version (this binding targets ABI 2). func ABIVersion() uint32` | Utility operation |
| `fengqudb` | `func BoolValue(v bool) Value` | Utility operation |
| `fengqudb` | `func Boolean(n uint64) Col` | Utility operation |
| `fengqudb` | `func BytesValue(v []byte) Value` | Utility operation |
| `fengqudb` | `// ColumnField references output column index. func ColumnField(index int) Field` | Utility operation |
| `fengqudb` | `// EncodeRequest builds the outer fqdb_store_select / txn_execute request: // 0x08 varint(op), bytes fields 2/3/4 (scope/key/value), argument=0, score=0, limit=256, flags=0. func EncodeRequest(operation uint64, scope, key, value []byte) []byte` | Utility operation |
| `fengqudb` | `func Fixed32(n uint64) Col` | Utility operation |
| `fengqudb` | `func Fixed64(n uint64) Col` | Utility operation |
| `fengqudb` | `func FloatValue(v float64) Value` | Utility operation |
| `fengqudb` | `// FromBytes starts a builder with a byte prefix. func FromBytes(prefix []byte) *Builder` | Utility operation |
| `fengqudb` | `func IntValue(v int64) Value` | Utility operation |
| `fengqudb` | `// Key references the record key. func Key() Col` | Utility operation |
| `fengqudb` | `// KeyField references the record key itself. func KeyField(source uint64) Field` | Utility operation |
| `fengqudb` | `// Nested references a field at a nested path; typ is the leaf's wire type. func Nested(path []uint64, typ uint64) Col` | Utility operation |
| `fengqudb` | `// Literal constructors. Like Python's binding, IntValue encodes nonnegative integers as uint and negative integers as ZigZag sint. func NullValue() Value` | Utility operation |
| `fengqudb` | `// Open opens the database in dir, or creates it according to Flags. func Open(dir string, opts *Options) (*Store, error)` | Utility operation |
| `fengqudb` | `// PrepareEncoded builds a PreparedQuery from encoded Query bytes. func PrepareEncoded(encoded []byte) *PreparedQuery` | Utility operation |
| `fengqudb` | `// RecordField references the entire record. func RecordField(source uint64) Field` | Utility operation |
| `fengqudb` | `func Sfixed32(n uint64) Col` | Utility operation |
| `fengqudb` | `func Sfixed64(n uint64) Col` | Utility operation |
| `fengqudb` | `func StringValue(v string) Value` | Utility operation |
| `fengqudb` | `func UintValue(v uint64) Value` | Utility operation |
| `fengqudb.Error` | `func (e *Error) Error() string` | Utility operation |
| `fengqudb.Error` | `// Is compares status codes for errors.Is(err, ErrNotFound) and similar calls. func (e *Error) Is(target error) bool` | Utility operation |
| `fengqudb.Field` | `// Any returns a copy marked as repeated. func (f Field) Any() Field` | Utility operation |
| `fengqudb.Result` | `// Access decodes the access plan; strings are copied. func (r *Result) Access() ([]Access, error)` | Utility operation |
| `fengqudb.Result` | `// AppendValues decodes all values in the current row and appends to dst; no allocation if capacity is sufficient. func (r *Result) AppendValues(dst []Value) ([]Value, error)` | Utility operation |
| `fengqudb.Result` | `// Close releases the native buffer; repeated calls are allowed, and the Result can be reused for execution. func (r *Result) Close()` | Release an object or borrowed resource |
| `fengqudb.Result` | `// Err returns a parsing error; check it after Next returns false. func (r *Result) Err() error` | Utility operation |
| `fengqudb.Result` | `// Key returns the current row key as a native view; aggregate/group rows use the encoded group key. func (r *Result) Key() []byte` | Utility operation |
| `fengqudb.Result` | `// Next advances to the next row; false means end of results or a malformed response (check Err). func (r *Result) Next() bool` | Utility operation |
| `fengqudb.Result` | `// NumValues returns the number of values in the current row. func (r *Result) NumValues() int` | Utility operation |
| `fengqudb.Result` | `// Raw returns native response bytes as a read-only view. func (r *Result) Raw() []byte` | Utility operation |
| `fengqudb.Result` | `// Stats decodes statistics. func (r *Result) Stats() (Stats, error)` | Utility operation |
| `fengqudb.Result` | `// Value decodes value i in the current row; an invalid index or malformed input returns ErrMalformed. func (r *Result) Value(i int) (Value, error)` | Utility operation |
| `fengqudb.Results` | `// Close releases the result set. func (r *Results) Close()` | Release an object or borrowed resource |
| `fengqudb.Results` | `// Rows returns result rows, valid until Close. func (r *Results) Rows() []Row` | Utility operation |
| `fengqudb.Store` | `// Close closes the Store; repeated calls have no effect. func (s *Store) Close() error` | Release an object or borrowed resource |
| `fengqudb.Value` | `// Bool returns a boolean. func (v Value) Bool() bool` | Utility operation |
| `fengqudb.Value` | `// Bytes returns a byte value as a native view; see Value's lifetime rules. func (v Value) Bytes() []byte` | Utility operation |
| `fengqudb.Value` | `// Float returns a double value. func (v Value) Float() float64` | Utility operation |
| `fengqudb.Value` | `// Int returns an integer; uint values beyond int64 wrap using two's-complement representation. func (v Value) Int() int64` | Utility operation |
| `fengqudb.Value` | `// IsNull reports whether the value is null. func (v Value) IsNull() bool` | Utility operation |
| `fengqudb.Value` | `// Uint returns an unsigned integer. func (v Value) Uint() uint64` | Utility operation |

</details>

## Parameter types, return values, and constants

Look up fields here when setting options or checking returned status. You can start with the CRUD examples above.

<details>
<summary>fengqudb</summary>

| Declaration |
| --- |
| `Access struct` |
| `Agg struct` |
| `AggAvg` |
| `AggCount` |
| `AggCountAll` |
| `AggMax` |
| `AggMin` |
| `AggNone Aggregate = iota` |
| `AggSum` |
| `Aggregate uint32` |
| `Builder struct` |
| `Col struct` |
| `Column struct` |
| `ColumnSource = 255` |
| `Cond struct` |
| `Database interface` |
| `DurabilityBuffered = 1` |
| `DurabilitySync = 0` |
| `// ErrClosed indicates use of a closed Store, transaction, reader, or other object. ErrClosed = errors.New("fengqudb: use of closed object")` |
| `ErrConflict = statusTable[StatusConflict]` |
| `ErrLimit = statusTable[StatusLimit]` |
| `// ErrMalformed indicates truncated or invalid protobuf in a query response. ErrMalformed = errors.New("fengqudb: malformed query response")` |
| `ErrNotFound = statusTable[StatusNotFound]` |
| `Error struct` |
| `Expr struct` |
| `Field struct` |
| `FlagCreate = 1` |
| `FlagSegmented = 2` |
| `IndexBool = 11` |
| `IndexBytes = 4` |
| `IndexDouble = 3` |
| `IndexField struct` |
| `IndexFixed32 = 9` |
| `IndexFixed64 = 7` |
| `IndexFloat = 6` |
| `IndexInt64 = 2` |
| `IndexRequired = 2` |
| `IndexSfixed32 = 10` |
| `IndexSfixed64 = 8` |
| `IndexSint64 = 5` |
| `IndexUint64 = 1` |
| `IndexUnique = 1` |
| `Info struct` |
| `Join struct` |
| `JoinAnti` |
| `JoinInner JoinKind = iota` |
| `JoinKind uint32` |
| `JoinLeft` |
| `JoinSemi` |
| `KindBool ValueKind = 5` |
| `KindBytes ValueKind = 4` |
| `KindDouble ValueKind = 3` |
| `KindNull ValueKind = 0` |
| `KindSint ValueKind = 2` |
| `KindUint ValueKind = 1` |
| `Op uint32` |
| `OpAnd` |
| `OpContains` |
| `OpEndsWith` |
| `OpEq` |
| `OpExists` |
| `OpFalse` |
| `OpFuzzy` |
| `OpGe` |
| `OpGt` |
| `OpIContains` |
| `OpILike` |
| `OpIRegex` |
| `OpIn` |
| `OpIsNull` |
| `OpLe` |
| `OpLike` |
| `OpLt` |
| `OpNe` |
| `OpNot` |
| `OpOr` |
| `OpRegex` |
| `OpSelect = 53` |
| `OpSelectExplain = 54` |
| `OpStartsWith` |
| `OpTrue Op = iota` |
| `Options struct` |
| `Order struct` |
| `Orderable interface` |
| `PreparedKey struct` |
| `PreparedQuery struct` |
| `PutItem struct` |
| `Query struct` |
| `Reader struct` |
| `Result struct` |
| `Results struct` |
| `Row struct` |
| `Rows struct` |
| `Selectable interface` |
| `Shared struct` |
| `SharedValue struct` |
| `Snapshot struct` |
| `Sort struct` |
| `Stats struct` |
| `Status int32` |
| `StatusAlreadyExists Status = 15` |
| `StatusAsyncRequired Status = 21` |
| `StatusBufferTooSmall Status = 16` |
| `StatusCancelled Status = 19` |
| `StatusCommitUnknown Status = 10` |
| `StatusConflict Status = 7` |
| `StatusConstraint Status = 20` |
| `StatusCorrupt Status = 5` |
| `StatusEnd Status = 2` |
| `StatusHistoryLost Status = 14` |
| `StatusIOError Status = 4` |
| `StatusInternalError Status = 13` |
| `StatusInvalidArgument Status = 3` |
| `StatusLimit Status = 9` |
| `StatusLocked Status = 6` |
| `StatusNoMemory Status = 8` |
| `StatusNotFound Status = 1` |
| `StatusNotModified Status = 18` |
| `StatusOK Status = 0` |
| `StatusRecoveryRequired Status = 11` |
| `StatusTimeout Status = 17` |
| `StatusUnsupported Status = 12` |
| `Store struct` |
| `Txn struct` |
| `TypeBool = 11` |
| `TypeBytes = 4` |
| `TypeDouble = 3` |
| `TypeFixed32 = 9` |
| `TypeFixed64 = 7` |
| `TypeFloat = 6` |
| `TypeInt64 = 2` |
| `TypeRecord = 0` |
| `TypeSfixed32 = 10` |
| `TypeSfixed64 = 8` |
| `TypeSint64 = 5` |
| `TypeUint64 = 1` |
| `Value struct` |
| `ValueKind uint8` |
| `WriteMode uint32` |
| `WriteOptions struct` |
| `WritePerformance WriteMode = 1 // Return after applying in memory; flush at the period or byte threshold.` |
| `WriteReliable WriteMode = 0 // Return after reaching durable storage.` |

</details>

<details>
<summary>fengqudb.Access</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Col</summary>

| Declaration |
| --- |
| `` |

</details>

<details>
<summary>fengqudb.Column</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Error</summary>

| Declaration |
| --- |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Expr</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Field</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.IndexField</summary>

| Declaration |
| --- |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Info</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Join</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Options</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Order</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.PreparedQuery</summary>

| Declaration |
| --- |
| `` |

</details>

<details>
<summary>fengqudb.PutItem</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Query</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Stats</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Value</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.WriteOptions</summary>

| Declaration |
| --- |
| `` |
| `` |
| `` |

</details>
