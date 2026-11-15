# Go API 与调用例子

[接入文档](../languages/go.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取

```go
data, err := st.Get([]byte("student/7"))
if errors.Is(err, fengqudb.ErrNotFound) { /* 没有这条记录 */ }
if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data)
```

引入 errors。缺失通过 ErrNotFound 区分，空切片是有效值。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `// Get 返回值的独立副本；未命中返回 ErrNotFound。空值返回非 nil 的空切片。 func (s *Store) Get(key []byte) ([]byte, error)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `// GetAppend 把值追加到 dst 并返回；dst 容量足够时不分配，且只需一次 cgo 往返。 // 未命中返回 (dst, ErrNotFound)。 func (s *Store) GetAppend(dst, key []byte) ([]byte, error)` | 读取对应的数据或状态 |

</details>

## 2. 新增、保存、单个字符串和整数

```go
if err := st.Put([]byte("name"), []byte("Alice")); err != nil { return err }
var bytes [8]byte
binary.LittleEndian.PutUint64(bytes[:], uint64(95))
if err := st.Put([]byte("score"), bytes[:]); err != nil { return err }
```

数字例子使用 encoding/binary。同键 Put 覆盖旧值；实体编码写法见接入文档。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `// Put 写入键值（无过期）。 func (s *Store) Put(key, value []byte) error` | 保存或设置数据 |
| `fengqudb.Store` | `// PutExpiring 写入键值，expiresAtMs 为绝对 Unix 毫秒，0 表示不过期。 func (s *Store) PutExpiring(key, value []byte, expiresAtMs uint64) error` | 保存或设置数据 |

</details>

## 3. 修改成绩

```go
data, err := st.Get([]byte("student/7")); if err != nil { return err }
row, err := fqmodels.StudentFqDecode(data); if err != nil { return err }
row.Value.Score = 96
data, err = row.FqEncode(); if err != nil { return err }
if err = st.Put([]byte("student/7"), data); err != nil { return err }
```

读改写用记录的 FqEncode 保留未知字段。

## 4. 删除

```go
if err := st.Delete([]byte("student/7")); err != nil { return err }
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `// Delete 删除键。 func (s *Store) Delete(key []byte) error` | 删除记录或成员 |

</details>

## 5. 事务和原子批写

```go
tx, err := st.Begin(0); if err != nil { return err }; defer tx.Close()
if err = tx.Put([]byte("a"), []byte("one")); err != nil { return err }
if err = tx.Put([]byte("b"), []byte("two")); err != nil { return err }
_, err = tx.Commit(); if err != nil { return err }
```

未提交即 Close 会丢弃。CONFLICT 时重新创建事务、重新读取计算。PutMany 将一组键值放进同一事务。

<details>
<summary>函数与参数（9项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `// Begin 开始事务；timeoutMs 为 0 表示没有期限。 func (s *Store) Begin(timeoutMs uint64) (*Txn, error)` | 事务操作 |
| `fengqudb.Txn` | `// Close 关闭事务（未提交则放弃）；重复调用无副作用。 func (t *Txn) Close()` | 释放对象或借用的资源 |
| `fengqudb.Txn` | `// Commit 提交事务并返回提交版本；提交后仍需 Close。 func (t *Txn) Commit() (uint64, error)` | 提交事务或查看提交结果 |
| `fengqudb.Txn` | `// Delete 在事务中删除键。 func (t *Txn) Delete(key []byte) error` | 删除记录或成员 |
| `fengqudb.Txn` | `// Get 读取事务内可见的值（含本事务的私有写入），返回副本；未命中返回 ErrNotFound。 func (t *Txn) Get(key []byte) ([]byte, error)` | 读取对应的数据或状态 |
| `fengqudb.Txn` | `// Put 在事务中写入键值。 func (t *Txn) Put(key, value []byte) error` | 保存或设置数据 |
| `fengqudb.Txn` | `// PutExpiring 在事务中写入键值并设置绝对 Unix 毫秒过期时间。 func (t *Txn) PutExpiring(key, value []byte, expiresAtMs uint64) error` | 保存或设置数据 |
| `fengqudb.Txn` | `// PutMany 一次提交多个写入；键与值先复制进 C 内存，核心库整体校验。 func (t *Txn) PutMany(items []PutItem) error` | 保存或设置数据 |
| `fengqudb.Txn` | `// SelectRequest 在事务内执行 SELECT/EXPLAIN（可见私有写入并登记冲突依赖）。 func (t *Txn) SelectRequest(request []byte, out *Result) error` | 事务操作 |

</details>

## 6. 条件查询与排序

```go
score := fengqudb.S64(fqmodels.StudentFieldScore)
name := fengqudb.Text(fqmodels.StudentFieldName)
rows, err := fengqudb.From("student/").Where(score.Ge(90)).
    Select(name,score).OrderBy(score.Desc()).Limit(10).Run(st)
if err != nil { return err }; defer rows.Close()
```

And/Or/Not 组合条件，GroupBy/Having 分组统计，Join 关联，Explain 查看查询计划。

<details>
<summary>函数与参数（84项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `func Avg(c Col) Agg` | 查询构建或结果读取 |
| `fengqudb` | `func Count() Agg` | 查询构建或结果读取 |
| `fengqudb` | `func CountDistinct(c Col) Agg` | 查询构建或结果读取 |
| `fengqudb` | `func CountOf(c Col) Agg` | 查询构建或结果读取 |
| `fengqudb` | `func F32(n uint64) Col` | 查询构建或结果读取 |
| `fengqudb` | `func F64(n uint64) Col` | 查询构建或结果读取 |
| `fengqudb` | `// From 以字符串前缀开始构建。 func From(prefix string) *Builder` | 查询构建或结果读取 |
| `fengqudb` | `func I64(n uint64) Col` | 查询构建或结果读取 |
| `fengqudb` | `func Max(c Col) Agg` | 查询构建或结果读取 |
| `fengqudb` | `func Min(c Col) Agg` | 查询构建或结果读取 |
| `fengqudb` | `// NewQuery 返回带默认限额的查询（与 Python Query 默认值相同）。 func NewQuery(prefix []byte) *Query` | 查询构建或结果读取 |
| `fengqudb` | `func S64(n uint64) Col` | 查询构建或结果读取 |
| `fengqudb` | `func Sum(c Col) Agg` | 查询构建或结果读取 |
| `fengqudb` | `func Text(n uint64) Col` | 查询构建或结果读取 |
| `fengqudb` | `// 字段工厂：名称对应 protobuf 线上类型。 func U64(n uint64) Col` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Asc(nulls ...bool) Sort` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Desc(nulls ...bool) Sort` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Eq(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Ge(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Gt(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Le(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Lt(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Agg` | `func (a Agg) Ne(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) After(cursor []byte) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) AllowScan(on bool) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `// Build 返回等价的底层 Query。 func (b *Builder) Build() (*Query, error)` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) CountTotal(on bool) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Distinct(on bool) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `// Explain 返回访问计划。 func (b *Builder) Explain(db Database) (*Result, error)` | 查看查询计划 |
| `fengqudb.Builder` | `func (b *Builder) GroupBy(cols ...Col) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Having(c Cond) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Index(name string) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `// Join 追加一个连接：left 为主源（或之前来源）的字段，right 为被连接前缀的字段。 func (b *Builder) Join(prefix string, left, right Col, kind JoinKind) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Limit(n uint64) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Memory(n uint64) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Offset(n uint64) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) OrderBy(items ...Orderable) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `// Pages 逐页执行并对每页调用 fn（见 Query.Pages）。 func (b *Builder) Pages(db Database, fn func(*Rows) error) error` | 查询构建或结果读取 |
| `fengqudb.Builder` | `// Prepare 编码一次，之后可反复执行。 func (b *Builder) Prepare() (*PreparedQuery, error)` | 查询构建或结果读取 |
| `fengqudb.Builder` | `// Run 执行查询。返回的 Rows 用完需 Close。 func (b *Builder) Run(db Database) (*Rows, error)` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Select(items ...Selectable) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) TimeoutMs(n uint64) *Builder` | 查询构建或结果读取 |
| `fengqudb.Builder` | `func (b *Builder) Where(c Cond) *Builder` | 查询构建或结果读取 |
| `fengqudb.Col` | `// Any 把字段标记为 protobuf repeated：任一元素满足即为真。 func (c Col) Any() Col` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Asc(nulls ...bool) Sort` | 查询构建或结果读取 |
| `fengqudb.Col` | `// Between 等价于 Ge(low) AND Le(high)。 func (c Col) Between(low, high any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Contains(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Desc(nulls ...bool) Sort` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) EndsWith(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Eq(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Exists() Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `// Fuzzy 判断整个字段（UTF-8）与 s 的编辑距离不超过 distance（0..8）。 func (c Col) Fuzzy(s string, distance int) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Ge(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Gt(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) IContains(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) ILike(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `// In 判断字段值是否在给定集合内。 func (c Col) In(values ...any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `// InSource 返回引用第 source 个来源（0 为主前缀，1..7 为连接）的同一字段。 func (c Col) InSource(source uint64) Col` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) IsNull() Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Le(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Like(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Lt(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Ne(v any) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) Regex(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Col` | `func (c Col) StartsWith(s string) Cond` | 查询构建或结果读取 |
| `fengqudb.Cond` | `// And 返回 c AND o。 func (c Cond) And(o Cond) Cond` | 查询构建或结果读取 |
| `fengqudb.Cond` | `// Not 返回 NOT c。 func (c Cond) Not() Cond` | 查询构建或结果读取 |
| `fengqudb.Cond` | `// Or 返回 c OR o。 func (c Cond) Or(o Cond) Cond` | 查询构建或结果读取 |
| `fengqudb.PreparedQuery` | `// Execute 执行查询并返回新的 Result（用完 Close）。 func (p *PreparedQuery) Execute(db Database) (*Result, error)` | 查询构建或结果读取 |
| `fengqudb.PreparedQuery` | `// ExecuteInto 执行查询，结果写入可复用的 out；热路径零分配。 func (p *PreparedQuery) ExecuteInto(db Database, out *Result) error` | 查询构建或结果读取 |
| `fengqudb.PreparedQuery` | `// Explain 返回访问计划（EXPLAIN）。 func (p *PreparedQuery) Explain(db Database) (*Result, error)` | 查看查询计划 |
| `fengqudb.PreparedQuery` | `// Request 返回 SELECT（explain=false）或 EXPLAIN 的完整请求字节。 func (p *PreparedQuery) Request(explain bool) []byte` | 查询构建或结果读取 |
| `fengqudb.Query` | `// AppendEncode 把 Query 编码追加到 b。 func (q *Query) AppendEncode(b []byte) ([]byte, error)` | 查询构建或结果读取 |
| `fengqudb.Query` | `// Encode 返回 Query 的编码字节。 func (q *Query) Encode() ([]byte, error)` | 查询构建或结果读取 |
| `fengqudb.Query` | `// Execute 编码并执行。 func (q *Query) Execute(db Database) (*Result, error)` | 查询构建或结果读取 |
| `fengqudb.Query` | `// Explain 编码并执行 EXPLAIN。 func (q *Query) Explain(db Database) (*Result, error)` | 查看查询计划 |
| `fengqudb.Query` | `// Pages 从 q.After 起逐页执行：每页调用 fn，用返回的游标作为下一页的 After， // 直到游标为空。fn 返回错误时终止。q 本身不被修改。fn 之后 Result 即被关闭。 func (q *Query) Pages(db Database, fn func(*Result) error) error` | 查询构建或结果读取 |
| `fengqudb.Query` | `// Prepare 编码查询并生成 SELECT / EXPLAIN 请求。 func (q *Query) Prepare() (*PreparedQuery, error)` | 查询构建或结果读取 |
| `fengqudb.Row` | `// ExpiresAtMs 返回过期的绝对 Unix 毫秒，0 表示不过期。 func (r *Row) ExpiresAtMs() uint64` | 查询构建或结果读取 |
| `fengqudb.Row` | `// Found 区分命中与未命中（空值也算命中）。 func (r *Row) Found() bool` | 查询构建或结果读取 |
| `fengqudb.Row` | `// Value 返回值的只读视图；未命中返回 nil，空值返回非 nil 的空切片。 func (r *Row) Value() []byte` | 查询构建或结果读取 |
| `fengqudb.Row` | `// Version 返回值的版本号。 func (r *Row) Version() uint64` | 查询构建或结果读取 |
| `fengqudb.Rows` | `// Value 返回当前行中被 Select 的列的值。 func (r *Rows) Value(item Selectable) (Value, error)` | 查询构建或结果读取 |
| `fengqudb.Store` | `// SelectRequest 以完整的外层请求字节（见 EncodeRequest）执行只读 SELECT/EXPLAIN， // 结果保存在原生缓冲 out 中；out 之前的内容会先被释放。 func (s *Store) SelectRequest(request []byte, out *Result) error` | 查询构建或结果读取 |

</details>

## 7. 索引

DefineCompoundIndex 为经常使用的字段筛选建立索引；DropIndex 删除索引。条件查询会利用可用索引。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `// DefineCompoundIndex 为以 prefix 开头的键定义复合索引。 func (s *Store) DefineCompoundIndex(name, prefix []byte, fields []IndexField, flags uint32) error` | 定义或使用索引 |
| `fengqudb.Store` | `// DropIndex 删除索引。 func (s *Store) DropIndex(name []byte) error` | 定义或使用索引 |

</details>

## 8. 等待可靠落盘

默认性能模式为 50 ms / 1 MiB，普通写成功表示内存生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```go
if err := st.Put([]byte("saved"), []byte("yes")); err != nil { return err }
if err := st.Sync(); err != nil { return err }
```

当前 Go 包没有单笔 forceReliable 参数；Sync 是显式落盘屏障，不要把 Put 单独返回当成可靠确认。整库可靠模式在写入前设置：

```go
options, err := st.WriteOptions(); if err != nil { return err }
options.Mode = fengqudb.WriteReliable
if err := st.ConfigureWrite(options); err != nil { return err }
```

WriteOptions 读取当前配置，ResetWriteOptions 恢复默认性能模式。配置会保存，重开后保留。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `// DefaultWriteOptions 返回新数据库的默认值：性能模式、50 ms、1 MiB。 func DefaultWriteOptions() WriteOptions` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `// ConfigureWrite 切换并保存写入模式；可能等待待写数据落盘。 // 保留未修改参数时，先用 WriteOptions 读取，再修改所需字段。 func (s *Store) ConfigureWrite(options WriteOptions) error` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `// ResetWriteOptions 恢复并保存默认性能模式、50 ms、1 MiB。 func (s *Store) ResetWriteOptions() error` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `// Sync 把已提交数据刷到持久存储。 func (s *Store) Sync() error` | 控制可靠确认或提交等待 |
| `fengqudb.Store` | `// WriteOptions 读取当前数据库的写入配置。 func (s *Store) WriteOptions() (WriteOptions, error)` | 控制可靠确认或提交等待 |

</details>

## 9. 临时数据

PutExpiring 的到期时间为绝对 Unix 毫秒，0 表示不过期。适合验证码、临时状态和短期缓存。

## 10. 减少热点读取分配

```go
shared, err := st.GetShared([]byte("student/7"))
if err != nil { return err }; defer shared.Close()
```

共享视图需要 Close。准备键/读取器可复用键和批量行，读取器行只保留到下次 Read/Release/Close。

<details>
<summary>函数与参数（17项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.PreparedKey` | `// Close 释放准备键；重复调用无副作用。已取得的 Shared 不受影响。 func (p *PreparedKey) Close()` | 释放对象或借用的资源 |
| `fengqudb.PreparedKey` | `// GetShared 返回当前值的共享视图；未命中返回 ErrNotFound。使用完 Close。 func (p *PreparedKey) GetShared() (Shared, error)` | 读取对应的数据或状态 |
| `fengqudb.Reader` | `// Acquire 读取整批并取得独立所有权的结果集。 func (rd *Reader) Acquire() (*Results, error)` | 复用键、缓冲或共享字节 |
| `fengqudb.Reader` | `// Close 关闭读取器；重复调用无副作用。 func (rd *Reader) Close()` | 释放对象或借用的资源 |
| `fengqudb.Reader` | `// Read 读取整批键，返回与键顺序一致的行。 func (rd *Reader) Read() ([]Row, error)` | 读取对应的数据或状态 |
| `fengqudb.Reader` | `// Release 释放当前保留的结果，保留准备好的键。 func (rd *Reader) Release()` | 释放对象或借用的资源 |
| `fengqudb.Shared` | `// Bytes 返回指向原生内存的只读切片（空值返回非 nil 的空切片）。 func (s *Shared) Bytes() []byte` | 复用键、缓冲或共享字节 |
| `fengqudb.Shared` | `// Close 归还租约；对同一个 Shared 重复调用无副作用。 func (s *Shared) Close()` | 释放对象或借用的资源 |
| `fengqudb.Shared` | `// Escape 把租约的所有权转移到一个带 finalizer 的堆对象（会产生一次分配）。 // 调用后原 Shared 不再有效，不要再对它调用 Close。 func (s *Shared) Escape() *SharedValue` | 复用键、缓冲或共享字节 |
| `fengqudb.Shared` | `// ExpiresAtMs 返回过期的绝对 Unix 毫秒，0 表示不过期。 func (s *Shared) ExpiresAtMs() uint64` | 复用键、缓冲或共享字节 |
| `fengqudb.Shared` | `// Len 返回值的字节数。 func (s *Shared) Len() int` | 复用键、缓冲或共享字节 |
| `fengqudb.Shared` | `// Text 把值当作字符串零拷贝返回；与 Bytes 同样在 Close 前有效。 func (s *Shared) Text() string` | 复用键、缓冲或共享字节 |
| `fengqudb.Shared` | `// Version 返回该值的版本号。 func (s *Shared) Version() uint64` | 复用键、缓冲或共享字节 |
| `fengqudb.SharedValue` | `// Close 归还租约并取消 finalizer。 func (v *SharedValue) Close()` | 释放对象或借用的资源 |
| `fengqudb.Store` | `// GetShared 返回指向原生内存的只读视图，未命中返回 ErrNotFound。使用完必须 Shared.Close。 func (s *Store) GetShared(key []byte) (Shared, error)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `// PrepareKey 为 key 创建准备键。 func (s *Store) PrepareKey(key []byte) (*PreparedKey, error)` | 复用键、缓冲或共享字节 |
| `fengqudb.Store` | `// PrepareReader 为 1..4096 个键创建批量读取器，保留顺序、重复键与空值。 func (s *Store) PrepareReader(keys [][]byte) (*Reader, error)` | 复用键、缓冲或共享字节 |

</details>

## 11. 一致快照

Snapshot 用于多次读取同一版本，使用后 Close。不要长时间保留不再需要的快照。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Result` | `// Cursor 返回续页游标（原生视图）；空表示已无更多页。续页时把它复制到 Query.After。 func (r *Result) Cursor() ([]byte, error)` | 一致读取或范围遍历 |
| `fengqudb.Snapshot` | `// Close 关闭快照；已取得的共享视图仍然有效。 func (sn *Snapshot) Close()` | 释放对象或借用的资源 |
| `fengqudb.Snapshot` | `// Get 返回快照中值的副本。 func (sn *Snapshot) Get(key []byte) ([]byte, error)` | 读取对应的数据或状态 |
| `fengqudb.Snapshot` | `// GetShared 返回快照中值的共享视图。 func (sn *Snapshot) GetShared(key []byte) (Shared, error)` | 读取对应的数据或状态 |
| `fengqudb.Store` | `// Snapshot 创建快照；快照的过期时间在创建时刻评估。 func (s *Store) Snapshot() (*Snapshot, error)` | 一致读取或范围遍历 |

</details>

## maintenance

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb.Store` | `// Info 返回 Store 统计信息。 func (s *Store) Info() (Info, error)` | 查看状态或维护数据库 |

</details>

## 12. 实体编解码

StudentFqEncode/StudentFqDecode 使用原 models.Student。StudentFqEncodeInto 接收可复用切片，传 dst[:0] 复用容量。编号锁随业务源码提交，已有字段不要重新分配编号。

当前 Go 客户端没有备份、维护、集合和变更订阅的高层方法；本文件列出的接口才是可直接调用的 Go API。

## 其他辅助接口

<details>
<summary>函数与参数（44项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `fengqudb` | `// ABIVersion 返回已加载核心库的 ABI 版本（本绑定面向 2）。 func ABIVersion() uint32` | 辅助操作 |
| `fengqudb` | `func BoolValue(v bool) Value` | 辅助操作 |
| `fengqudb` | `func Boolean(n uint64) Col` | 辅助操作 |
| `fengqudb` | `func BytesValue(v []byte) Value` | 辅助操作 |
| `fengqudb` | `// ColumnField 引用第 index 个输出列。 func ColumnField(index int) Field` | 辅助操作 |
| `fengqudb` | `// EncodeRequest 构造 fqdb_store_select / txn_execute 的外层请求： // 0x08 varint(op)、字段 2/3/4 为 bytes（scope、key、value）、argument=0、score=0、limit=256、flags=0。 func EncodeRequest(operation uint64, scope, key, value []byte) []byte` | 辅助操作 |
| `fengqudb` | `func Fixed32(n uint64) Col` | 辅助操作 |
| `fengqudb` | `func Fixed64(n uint64) Col` | 辅助操作 |
| `fengqudb` | `func FloatValue(v float64) Value` | 辅助操作 |
| `fengqudb` | `// FromBytes 以字节前缀开始构建。 func FromBytes(prefix []byte) *Builder` | 辅助操作 |
| `fengqudb` | `func IntValue(v int64) Value` | 辅助操作 |
| `fengqudb` | `// Key 引用记录键。 func Key() Col` | 辅助操作 |
| `fengqudb` | `// KeyField 引用记录键本身。 func KeyField(source uint64) Field` | 辅助操作 |
| `fengqudb` | `// Nested 引用嵌套路径上的字段，type 为叶子的线上类型。 func Nested(path []uint64, typ uint64) Col` | 辅助操作 |
| `fengqudb` | `// 字面量构造函数。IntValue 与 Python 绑定一致：非负数编码为 uint，负数编码为 zigzag sint。 func NullValue() Value` | 辅助操作 |
| `fengqudb` | `// Open 打开（或按 Flags 创建）目录 dir 下的数据库。 func Open(dir string, opts *Options) (*Store, error)` | 辅助操作 |
| `fengqudb` | `// PrepareEncoded 由已编码的 Query 字节生成 PreparedQuery。 func PrepareEncoded(encoded []byte) *PreparedQuery` | 辅助操作 |
| `fengqudb` | `// RecordField 引用整条记录。 func RecordField(source uint64) Field` | 辅助操作 |
| `fengqudb` | `func Sfixed32(n uint64) Col` | 辅助操作 |
| `fengqudb` | `func Sfixed64(n uint64) Col` | 辅助操作 |
| `fengqudb` | `func StringValue(v string) Value` | 辅助操作 |
| `fengqudb` | `func UintValue(v uint64) Value` | 辅助操作 |
| `fengqudb.Error` | `func (e *Error) Error() string` | 辅助操作 |
| `fengqudb.Error` | `// Is 让 errors.Is(err, ErrNotFound) 等按状态码比较。 func (e *Error) Is(target error) bool` | 辅助操作 |
| `fengqudb.Field` | `// Any 返回标记为 repeated 的副本。 func (f Field) Any() Field` | 辅助操作 |
| `fengqudb.Result` | `// Access 解码访问计划（字符串会复制）。 func (r *Result) Access() ([]Access, error)` | 辅助操作 |
| `fengqudb.Result` | `// AppendValues 把当前行的全部值解码并追加到 dst（dst 容量足够时不分配）。 func (r *Result) AppendValues(dst []Value) ([]Value, error)` | 辅助操作 |
| `fengqudb.Result` | `// Close 释放原生缓冲；可重复调用，之后可再次用于执行。 func (r *Result) Close()` | 释放对象或借用的资源 |
| `fengqudb.Result` | `// Err 返回解析过程中遇到的错误（Next 返回 false 后检查）。 func (r *Result) Err() error` | 辅助操作 |
| `fengqudb.Result` | `// Key 返回当前行的键（原生视图）。对聚合/分组行，键是分组键的编码。 func (r *Result) Key() []byte` | 辅助操作 |
| `fengqudb.Result` | `// Next 前进到下一行；没有更多行或响应格式错误时返回 false（后者见 Err）。 func (r *Result) Next() bool` | 辅助操作 |
| `fengqudb.Result` | `// NumValues 返回当前行的值个数。 func (r *Result) NumValues() int` | 辅助操作 |
| `fengqudb.Result` | `// Raw 返回原生响应字节（只读视图）。 func (r *Result) Raw() []byte` | 辅助操作 |
| `fengqudb.Result` | `// Stats 解码统计信息。 func (r *Result) Stats() (Stats, error)` | 辅助操作 |
| `fengqudb.Result` | `// Value 解码当前行的第 i 个值；i 越界或格式错误时返回 ErrMalformed。 func (r *Result) Value(i int) (Value, error)` | 辅助操作 |
| `fengqudb.Results` | `// Close 释放结果集。 func (r *Results) Close()` | 释放对象或借用的资源 |
| `fengqudb.Results` | `// Rows 返回结果行，Close 之前有效。 func (r *Results) Rows() []Row` | 辅助操作 |
| `fengqudb.Store` | `// Close 关闭 Store。重复调用无副作用。 func (s *Store) Close() error` | 释放对象或借用的资源 |
| `fengqudb.Value` | `// Bool 返回布尔值。 func (v Value) Bool() bool` | 辅助操作 |
| `fengqudb.Value` | `// Bytes 返回字节值（原生视图，见 Value 文档）。 func (v Value) Bytes() []byte` | 辅助操作 |
| `fengqudb.Value` | `// Float 返回 double 值。 func (v Value) Float() float64` | 辅助操作 |
| `fengqudb.Value` | `// Int 返回整数值（uint 超过 int64 范围时按补码截断）。 func (v Value) Int() int64` | 辅助操作 |
| `fengqudb.Value` | `// IsNull 报告是否为 null。 func (v Value) IsNull() bool` | 辅助操作 |
| `fengqudb.Value` | `// Uint 返回无符号整数值。 func (v Value) Uint() uint64` | 辅助操作 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>fengqudb</summary>

| 声明 |
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
| `// ErrClosed 表示对已关闭的 Store、事务、读取器等对象继续调用。 ErrClosed = errors.New("fengqudb: use of closed object")` |
| `ErrConflict = statusTable[StatusConflict]` |
| `ErrLimit = statusTable[StatusLimit]` |
| `// ErrMalformed 表示查询响应的 protobuf 内容被截断或无效。 ErrMalformed = errors.New("fengqudb: malformed query response")` |
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
| `WritePerformance WriteMode = 1 // 内存生效后返回，按周期或字节阈值刷盘。` |
| `WriteReliable WriteMode = 0 // 等待可靠落盘后返回。` |

</details>

<details>
<summary>fengqudb.Access</summary>

| 声明 |
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

| 声明 |
| --- |
| `` |

</details>

<details>
<summary>fengqudb.Column</summary>

| 声明 |
| --- |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Error</summary>

| 声明 |
| --- |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Expr</summary>

| 声明 |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Field</summary>

| 声明 |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.IndexField</summary>

| 声明 |
| --- |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Info</summary>

| 声明 |
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

| 声明 |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Options</summary>

| 声明 |
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

| 声明 |
| --- |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.PreparedQuery</summary>

| 声明 |
| --- |
| `` |

</details>

<details>
<summary>fengqudb.PutItem</summary>

| 声明 |
| --- |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.Query</summary>

| 声明 |
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

| 声明 |
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

| 声明 |
| --- |
| `` |
| `` |
| `` |
| `` |
| `` |

</details>

<details>
<summary>fengqudb.WriteOptions</summary>

| 声明 |
| --- |
| `` |
| `` |
| `` |

</details>
