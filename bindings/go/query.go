package fengqudb

import (
	"encoding/binary"
	"errors"
	"math"
)

// ValueKind 是 QueryValue 的类型。
type ValueKind uint8

// 值类型，数值与线上编码一致。
const (
	KindNull   ValueKind = 0
	KindUint   ValueKind = 1
	KindSint   ValueKind = 2
	KindDouble ValueKind = 3
	KindBytes  ValueKind = 4
	KindBool   ValueKind = 5
)

// Value 是查询中的字面量，也是结果行里的单元值。
// 解码得到的 B 指向原生响应缓冲，随 Result.Close 失效；需要逃逸时自行复制。
type Value struct {
	Kind ValueKind
	U    uint64 // KindUint；KindBool 时 0/1
	I    int64  // KindSint
	F    float64
	B    []byte
}

// 字面量构造函数。IntValue 与 Python 绑定一致：非负数编码为 uint，负数编码为 zigzag sint。
func NullValue() Value           { return Value{} }
func UintValue(v uint64) Value   { return Value{Kind: KindUint, U: v} }
func FloatValue(v float64) Value { return Value{Kind: KindDouble, F: v} }
func BytesValue(v []byte) Value  { return Value{Kind: KindBytes, B: v} }
func StringValue(v string) Value { return Value{Kind: KindBytes, B: []byte(v)} }
func BoolValue(v bool) Value {
	if v {
		return Value{Kind: KindBool, U: 1}
	}
	return Value{Kind: KindBool}
}
func IntValue(v int64) Value {
	if v >= 0 {
		return Value{Kind: KindUint, U: uint64(v)}
	}
	return Value{Kind: KindSint, I: v}
}

// IsNull 报告是否为 null。
func (v Value) IsNull() bool { return v.Kind == KindNull }

// Int 返回整数值（uint 超过 int64 范围时按补码截断）。
func (v Value) Int() int64 {
	if v.Kind == KindSint {
		return v.I
	}
	return int64(v.U)
}

// Uint 返回无符号整数值。
func (v Value) Uint() uint64 {
	if v.Kind == KindSint {
		return uint64(v.I)
	}
	return v.U
}

// Float 返回 double 值。
func (v Value) Float() float64 { return v.F }

// Bool 返回布尔值。
func (v Value) Bool() bool { return v.U != 0 }

// Bytes 返回字节值（原生视图，见 Value 文档）。
func (v Value) Bytes() []byte { return v.B }

// Op 是表达式操作符。
type Op uint32

const (
	OpTrue Op = iota
	OpFalse
	OpAnd
	OpOr
	OpNot
	OpEq
	OpNe
	OpLt
	OpLe
	OpGt
	OpGe
	OpIn
	OpExists
	OpIsNull
	OpContains
	OpStartsWith
	OpEndsWith
	OpLike
	OpRegex
	OpIContains
	OpILike
	OpIRegex
	OpFuzzy
)

// Aggregate 是聚合函数。
type Aggregate uint32

const (
	AggNone Aggregate = iota
	AggCountAll
	AggCount
	AggSum
	AggMin
	AggMax
	AggAvg
)

// JoinKind 是连接类型。
type JoinKind uint32

const (
	JoinInner JoinKind = iota
	JoinLeft
	JoinSemi
	JoinAnti
)

// 字段类型，对应 protobuf 线上类型编号（Type 0 表示整条记录）。
const (
	TypeRecord   = 0
	TypeUint64   = 1
	TypeInt64    = 2
	TypeDouble   = 3
	TypeBytes    = 4
	TypeSint64   = 5
	TypeFloat    = 6
	TypeFixed64  = 7
	TypeSfixed64 = 8
	TypeFixed32  = 9
	TypeSfixed32 = 10
	TypeBool     = 11
)

// ColumnSource 是输出列引用（HAVING / ORDER BY 聚合）使用的 source 号。
const ColumnSource = 255

// Field 引用记录的一个字段。Source 0 为主前缀，1..7 为按顺序的连接。
// Path 非空时表示嵌套路径，否则使用 Number。Repeated 对应 protobuf repeated 字段
// （任一元素满足即为真，只能用于主源 WHERE 的叶子条件）。
type Field struct {
	Number   uint64
	Type     uint64
	Source   uint64
	Path     []uint64
	Repeated bool
}

// KeyField 引用记录键本身。
func KeyField(source uint64) Field { return Field{Source: source} }

// RecordField 引用整条记录。
func RecordField(source uint64) Field { return Field{Type: TypeRecord, Source: source} }

// ColumnField 引用第 index 个输出列。
func ColumnField(index int) Field {
	return Field{Number: uint64(index) + 1, Type: 0, Source: ColumnSource}
}

// Any 返回标记为 repeated 的副本。
func (f Field) Any() Field {
	f.Repeated = true
	return f
}

func (f Field) equal(o Field) bool {
	if f.Number != o.Number || f.Type != o.Type || f.Source != o.Source || f.Repeated != o.Repeated || len(f.Path) != len(o.Path) {
		return false
	}
	for i := range f.Path {
		if f.Path[i] != o.Path[i] {
			return false
		}
	}
	return true
}

// Expr 是表达式树。零值是恒真条件。
type Expr struct {
	Op       Op
	Field    Field
	Values   []Value
	Children []Expr
	Right    *Field // 字段与字段比较
}

// Column 是输出列。
type Column struct {
	Field     Field
	Aggregate Aggregate
	Name      string
	Distinct  bool
}

// Order 是排序项。
type Order struct {
	Field      Field
	Descending bool
	NullsFirst bool
}

// Join 是连接。
type Join struct {
	Prefix []byte
	Left   []Field
	Right  []Field
	Kind   JoinKind
	On     Expr
}

// Query 是底层查询结构，编码与 bindings/python/fengqudb_select.py 逐字节一致。
type Query struct {
	Prefix        []byte
	Where         Expr
	Columns       []Column
	Order         []Order
	Group         []Field
	Having        Expr
	Joins         []Join
	Limit         uint64
	Offset        uint64
	MemoryBytes   uint64
	MaxCandidates uint64
	MaxWork       uint64
	TimeoutMs     uint64
	AllowScan     bool
	CountTotal    bool
	Distinct      bool
	IndexHint     []byte
	After         []byte
}

// NewQuery 返回带默认限额的查询（与 Python Query 默认值相同）。
func NewQuery(prefix []byte) *Query {
	return &Query{Prefix: prefix, Limit: 100, MemoryBytes: 16 << 20, MaxCandidates: 1000000,
		MaxWork: 100000000, TimeoutMs: 1000, AllowScan: true}
}

var errTooDeep = errors.New("fengqudb: query expression too deep")
var errNonFinite = errors.New("fengqudb: non-finite query number")

func appendVarint(b []byte, v uint64) []byte {
	for v >= 128 {
		b = append(b, byte(v)|128)
		v >>= 7
	}
	return append(b, byte(v))
}

func appendU(b []byte, n uint64, v uint64) []byte {
	return appendVarint(appendVarint(b, n*8), v)
}

func appendBool(b []byte, n uint64, v bool) []byte {
	if v {
		return appendU(b, n, 1)
	}
	return appendU(b, n, 0)
}

func appendBytesField(b []byte, n uint64, v []byte) []byte {
	b = appendVarint(b, n*8+2)
	b = appendVarint(b, uint64(len(v)))
	return append(b, v...)
}

// appendNested 在 b 末尾写入长度前缀嵌套消息，body 把内容追加到同一缓冲，避免临时切片。
func appendNested(b []byte, n uint64, body func([]byte) ([]byte, error)) ([]byte, error) {
	b = appendVarint(b, n*8+2)
	// 先按最多 5 字节预留长度位置再回填（消息上限 < 4 GiB）。
	lenAt := len(b)
	b = append(b, 0, 0, 0, 0, 0)
	start := len(b)
	b, err := body(b)
	if err != nil {
		return b, err
	}
	size := uint64(len(b) - start)
	var tmp [10]byte
	enc := binary.PutUvarint(tmp[:], size)
	// 把正文向前挪到紧凑的长度字节之后。
	copy(b[lenAt+enc:], b[start:])
	copy(b[lenAt:], tmp[:enc])
	return b[:len(b)-(5-enc)], nil
}

func (v Value) appendTo(b []byte) ([]byte, error) {
	switch v.Kind {
	case KindNull:
		return appendU(b, 1, 0), nil
	case KindUint:
		return appendU(appendU(b, 1, 1), 2, v.U), nil
	case KindSint:
		return appendU(appendU(b, 1, 2), 3, uint64(v.I<<1)^uint64(v.I>>63)), nil
	case KindDouble:
		if math.IsNaN(v.F) || math.IsInf(v.F, 0) {
			return b, errNonFinite
		}
		b = appendU(b, 1, 3)
		b = append(b, 0x21)
		return binary.LittleEndian.AppendUint64(b, math.Float64bits(v.F)), nil
	case KindBytes:
		return appendBytesField(appendU(b, 1, 4), 5, v.B), nil
	case KindBool:
		return appendU(appendU(b, 1, 5), 2, v.U&1), nil
	}
	return b, errors.New("fengqudb: invalid query value kind")
}

func (f Field) appendTo(b []byte) []byte {
	b = appendU(b, 1, f.Source)
	b = appendU(b, 2, f.Type)
	if len(f.Path) > 0 {
		for _, n := range f.Path {
			b = appendU(b, 3, n)
		}
	} else if f.Number != 0 {
		b = appendU(b, 3, f.Number)
	}
	if f.Repeated {
		b = appendU(b, 4, 1)
	}
	return b
}

func (e *Expr) appendTo(b []byte, depth int) ([]byte, error) {
	if depth > 64 {
		return b, errTooDeep
	}
	b = appendU(b, 1, uint64(e.Op))
	var err error
	if e.Op >= OpEq {
		b, err = appendNested(b, 2, func(b []byte) ([]byte, error) { return e.Field.appendTo(b), nil })
		if err != nil {
			return b, err
		}
	}
	for _, v := range e.Values {
		v := v
		b, err = appendNested(b, 3, v.appendTo)
		if err != nil {
			return b, err
		}
	}
	for i := range e.Children {
		c := &e.Children[i]
		b, err = appendNested(b, 4, func(b []byte) ([]byte, error) { return c.appendTo(b, depth+1) })
		if err != nil {
			return b, err
		}
	}
	if e.Right != nil {
		b, err = appendNested(b, 5, func(b []byte) ([]byte, error) { return e.Right.appendTo(b), nil })
	}
	return b, err
}

// nested 把 Field 编码为嵌套消息 n。
func nestedField(b []byte, n uint64, f Field) ([]byte, error) {
	return appendNested(b, n, func(b []byte) ([]byte, error) { return f.appendTo(b), nil })
}

func nestedExpr(b []byte, n uint64, e *Expr) ([]byte, error) {
	return appendNested(b, n, func(b []byte) ([]byte, error) { return e.appendTo(b, 0) })
}

// AppendEncode 把 Query 编码追加到 b。
func (q *Query) AppendEncode(b []byte) ([]byte, error) {
	var err error
	b = appendBytesField(b, 1, q.Prefix)
	if b, err = nestedExpr(b, 2, &q.Where); err != nil {
		return b, err
	}
	for _, c := range q.Columns {
		c := c
		b, err = appendNested(b, 3, func(b []byte) ([]byte, error) {
			b, err := nestedField(b, 1, c.Field)
			if err != nil {
				return b, err
			}
			b = appendU(b, 2, uint64(c.Aggregate))
			b = appendBytesField(b, 3, []byte(c.Name))
			return appendBool(b, 4, c.Distinct), nil
		})
		if err != nil {
			return b, err
		}
	}
	for _, o := range q.Order {
		o := o
		b, err = appendNested(b, 4, func(b []byte) ([]byte, error) {
			b, err := nestedField(b, 1, o.Field)
			if err != nil {
				return b, err
			}
			b = appendBool(b, 2, o.Descending)
			return appendBool(b, 3, o.NullsFirst), nil
		})
		if err != nil {
			return b, err
		}
	}
	for _, g := range q.Group {
		if b, err = nestedField(b, 5, g); err != nil {
			return b, err
		}
	}
	if b, err = nestedExpr(b, 6, &q.Having); err != nil {
		return b, err
	}
	for _, j := range q.Joins {
		j := j
		b, err = appendNested(b, 7, func(b []byte) ([]byte, error) {
			b = appendBytesField(b, 1, j.Prefix)
			var err error
			for _, f := range j.Left {
				if b, err = nestedField(b, 2, f); err != nil {
					return b, err
				}
			}
			for _, f := range j.Right {
				if b, err = nestedField(b, 3, f); err != nil {
					return b, err
				}
			}
			b = appendU(b, 4, uint64(j.Kind))
			return nestedExpr(b, 5, &j.On)
		})
		if err != nil {
			return b, err
		}
	}
	for i, v := range []uint64{q.Limit, q.Offset, q.MemoryBytes, q.MaxCandidates, q.MaxWork, q.TimeoutMs} {
		b = appendU(b, uint64(8+i), v)
	}
	b = appendBool(b, 14, q.AllowScan)
	b = appendBool(b, 15, q.CountTotal)
	b = appendBool(b, 16, q.Distinct)
	b = appendBytesField(b, 17, q.IndexHint)
	return appendBytesField(b, 18, q.After), nil
}

// Encode 返回 Query 的编码字节。
func (q *Query) Encode() ([]byte, error) { return q.AppendEncode(nil) }

// 操作号：SELECT 与 EXPLAIN。
const (
	OpSelect        = 53
	OpSelectExplain = 54
)

// EncodeRequest 构造 fqdb_store_select / txn_execute 的外层请求：
// 0x08 varint(op)、字段 2/3/4 为 bytes（scope、key、value）、argument=0、score=0、limit=256、flags=0。
func EncodeRequest(operation uint64, scope, key, value []byte) []byte {
	b := make([]byte, 0, 40+len(scope)+len(key)+len(value))
	b = append(b, 0x08)
	b = appendVarint(b, operation)
	b = appendBytesField(b, 2, scope)
	b = appendBytesField(b, 3, key)
	b = appendBytesField(b, 4, value)
	b = append(b, 0x29, 0, 0, 0, 0, 0, 0, 0, 0)
	b = append(b, 0x31, 0, 0, 0, 0, 0, 0, 0, 0)
	b = append(b, 0x39)
	b = binary.LittleEndian.AppendUint64(b, 256)
	return append(b, 0x40, 0)
}

// PreparedQuery 保存已编码的查询和两份完整请求，重复执行不再编码。
type PreparedQuery struct {
	Encoded []byte
	execute []byte
	explain []byte
}

// Prepare 编码查询并生成 SELECT / EXPLAIN 请求。
func (q *Query) Prepare() (*PreparedQuery, error) {
	enc, err := q.Encode()
	if err != nil {
		return nil, err
	}
	return PrepareEncoded(enc), nil
}

// PrepareEncoded 由已编码的 Query 字节生成 PreparedQuery。
func PrepareEncoded(encoded []byte) *PreparedQuery {
	return &PreparedQuery{Encoded: encoded,
		execute: EncodeRequest(OpSelect, nil, nil, encoded),
		explain: EncodeRequest(OpSelectExplain, nil, nil, encoded)}
}

// Request 返回 SELECT（explain=false）或 EXPLAIN 的完整请求字节。
func (p *PreparedQuery) Request(explain bool) []byte {
	if explain {
		return p.explain
	}
	return p.execute
}

// Database 由 *Store（只读快照）与 *Txn（可见私有写入）实现。
type Database interface {
	SelectRequest(request []byte, out *Result) error
}

// ExecuteInto 执行查询，结果写入可复用的 out；热路径零分配。
func (p *PreparedQuery) ExecuteInto(db Database, out *Result) error {
	return db.SelectRequest(p.execute, out)
}

// Execute 执行查询并返回新的 Result（用完 Close）。
func (p *PreparedQuery) Execute(db Database) (*Result, error) {
	r := newResult()
	if err := p.ExecuteInto(db, r); err != nil {
		return nil, err
	}
	return r, nil
}

// Explain 返回访问计划（EXPLAIN）。
func (p *PreparedQuery) Explain(db Database) (*Result, error) {
	r := newResult()
	if err := db.SelectRequest(p.explain, r); err != nil {
		return nil, err
	}
	return r, nil
}

// Execute 编码并执行。
func (q *Query) Execute(db Database) (*Result, error) {
	p, err := q.Prepare()
	if err != nil {
		return nil, err
	}
	return p.Execute(db)
}

// Explain 编码并执行 EXPLAIN。
func (q *Query) Explain(db Database) (*Result, error) {
	p, err := q.Prepare()
	if err != nil {
		return nil, err
	}
	return p.Explain(db)
}

// Pages 从 q.After 起逐页执行：每页调用 fn，用返回的游标作为下一页的 After，
// 直到游标为空。fn 返回错误时终止。q 本身不被修改。fn 之后 Result 即被关闭。
func (q *Query) Pages(db Database, fn func(*Result) error) error {
	cur := *q
	var res Result
	defer res.Close()
	for {
		p, err := cur.Prepare()
		if err != nil {
			return err
		}
		if err := p.ExecuteInto(db, &res); err != nil {
			return err
		}
		if err := fn(&res); err != nil {
			return err
		}
		next, err := res.Cursor()
		if err != nil {
			return err
		}
		if len(next) == 0 {
			return nil
		}
		cur.After = append([]byte(nil), next...)
	}
}
