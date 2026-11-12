package fengqudb

import (
	"errors"
	"fmt"
	"math"
)

// 类型化查询构建器，等价于 bindings/python/fengqudb_query.py：
//
//	uid, cls, level := fengqudb.U64(1), fengqudb.Text(2), fengqudb.U64(3)
//	rows, err := fengqudb.From("p/").
//	    Where(cls.Eq("mage").And(level.Between(5, 15))).
//	    Select(uid, level).OrderBy(level.Desc()).Limit(7).Run(store)

type castKind uint8

const (
	castNone castKind = iota
	castInt
	castFloat
	castBool
)

// literal 按列类型把 Go 值转换为查询字面量（规则同 Python 的 _literal）。
func literal(cast castKind, v any) (Value, error) {
	switch x := v.(type) {
	case nil:
		return NullValue(), nil
	case Value:
		return x, nil
	case bool:
		return BoolValue(x), nil
	}
	var iv int64
	var uv uint64
	isU, isNum := false, true
	switch x := v.(type) {
	case int:
		iv = int64(x)
	case int8:
		iv = int64(x)
	case int16:
		iv = int64(x)
	case int32:
		iv = int64(x)
	case int64:
		iv = x
	case uint:
		uv, isU = uint64(x), true
	case uint8:
		uv, isU = uint64(x), true
	case uint16:
		uv, isU = uint64(x), true
	case uint32:
		uv, isU = uint64(x), true
	case uint64:
		uv, isU = x, true
	case float32:
		return floatLiteral(cast, float64(x))
	case float64:
		return floatLiteral(cast, x)
	default:
		isNum = false
	}
	if isNum {
		switch cast {
		case castFloat:
			if isU {
				return FloatValue(float64(uv)), nil
			}
			return FloatValue(float64(iv)), nil
		case castBool:
			if isU {
				return BoolValue(uv != 0), nil
			}
			return BoolValue(iv != 0), nil
		}
		if isU {
			return UintValue(uv), nil
		}
		return IntValue(iv), nil
	}
	if cast == castInt || cast == castFloat || cast == castBool {
		switch v.(type) {
		case string, []byte:
			return Value{}, fmt.Errorf("fengqudb: cannot use %T as numeric query literal", v)
		}
	}
	switch x := v.(type) {
	case string:
		return StringValue(x), nil
	case []byte:
		return BytesValue(x), nil
	}
	return Value{}, fmt.Errorf("fengqudb: unsupported query literal type %T", v)
}

func floatLiteral(cast castKind, f float64) (Value, error) {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return Value{}, errNonFinite
	}
	switch cast {
	case castInt:
		if f >= 0 && f < 1<<63 {
			return IntValue(int64(f)), nil
		}
		if f < 0 && f >= -(1<<63) {
			return IntValue(int64(f)), nil
		}
		return Value{}, errors.New("fengqudb: float literal outside int64")
	case castBool:
		return BoolValue(f != 0), nil
	}
	return FloatValue(f), nil
}

// Cond 是延迟解析的条件：构建 Query 时才会按已选列解析聚合引用。
type Cond struct {
	build func(cols []Column) (Expr, error)
	agg   bool // 含聚合引用，只能用于 Having
}

func fixed(e Expr, err error) Cond {
	return Cond{build: func([]Column) (Expr, error) { return e, err }}
}

// And 返回 c AND o。
func (c Cond) And(o Cond) Cond { return combine(OpAnd, c, o) }

// Or 返回 c OR o。
func (c Cond) Or(o Cond) Cond { return combine(OpOr, c, o) }

// Not 返回 NOT c。
func (c Cond) Not() Cond {
	return Cond{agg: c.agg, build: func(cols []Column) (Expr, error) {
		a, err := c.build(cols)
		return Expr{Op: OpNot, Children: []Expr{a}}, err
	}}
}

func combine(op Op, a, b Cond) Cond {
	return Cond{agg: a.agg || b.agg, build: func(cols []Column) (Expr, error) {
		x, err := a.build(cols)
		if err != nil {
			return Expr{}, err
		}
		y, err := b.build(cols)
		return Expr{Op: op, Children: []Expr{x, y}}, err
	}}
}

// Col 是某个来源（0 = 主前缀，1..7 = 按顺序的连接）的类型化字段。
type Col struct {
	Field Field
	cast  castKind
}

// 字段工厂：名称对应 protobuf 线上类型。
func U64(n uint64) Col      { return Col{Field: Field{Number: n, Type: TypeUint64}, cast: castInt} }
func I64(n uint64) Col      { return Col{Field: Field{Number: n, Type: TypeInt64}, cast: castInt} }
func F64(n uint64) Col      { return Col{Field: Field{Number: n, Type: TypeDouble}, cast: castFloat} }
func Text(n uint64) Col     { return Col{Field: Field{Number: n, Type: TypeBytes}} }
func S64(n uint64) Col      { return Col{Field: Field{Number: n, Type: TypeSint64}, cast: castInt} }
func F32(n uint64) Col      { return Col{Field: Field{Number: n, Type: TypeFloat}, cast: castFloat} }
func Fixed64(n uint64) Col  { return Col{Field: Field{Number: n, Type: TypeFixed64}, cast: castInt} }
func Sfixed64(n uint64) Col { return Col{Field: Field{Number: n, Type: TypeSfixed64}, cast: castInt} }
func Fixed32(n uint64) Col  { return Col{Field: Field{Number: n, Type: TypeFixed32}, cast: castInt} }
func Sfixed32(n uint64) Col { return Col{Field: Field{Number: n, Type: TypeSfixed32}, cast: castInt} }
func Boolean(n uint64) Col  { return Col{Field: Field{Number: n, Type: TypeBool}, cast: castBool} }

// Key 引用记录键。
func Key() Col { return Col{Field: KeyField(0)} }

// Nested 引用嵌套路径上的字段，type 为叶子的线上类型。
func Nested(path []uint64, typ uint64) Col {
	return Col{Field: Field{Type: typ, Path: append([]uint64(nil), path...)}}
}

// InSource 返回引用第 source 个来源（0 为主前缀，1..7 为连接）的同一字段。
func (c Col) InSource(source uint64) Col {
	c.Field.Source = source
	return c
}

// Any 把字段标记为 protobuf repeated：任一元素满足即为真。
func (c Col) Any() Col {
	c.Field = c.Field.Any()
	return c
}

func (c Col) test(op Op, values ...any) Cond {
	vs := make([]Value, 0, len(values))
	for _, v := range values {
		lit, err := literal(c.cast, v)
		if err != nil {
			return fixed(Expr{}, err)
		}
		vs = append(vs, lit)
	}
	return fixed(Expr{Op: op, Field: c.Field, Values: vs}, nil)
}

// cmp 的参数是 Col 时生成字段与字段的比较。
func (c Col) cmp(op Op, other any) Cond {
	if o, ok := other.(Col); ok {
		f := o.Field
		return fixed(Expr{Op: op, Field: c.Field, Right: &f}, nil)
	}
	return c.test(op, other)
}

func (c Col) Eq(v any) Cond { return c.cmp(OpEq, v) }
func (c Col) Ne(v any) Cond { return c.cmp(OpNe, v) }
func (c Col) Lt(v any) Cond { return c.cmp(OpLt, v) }
func (c Col) Le(v any) Cond { return c.cmp(OpLe, v) }
func (c Col) Gt(v any) Cond { return c.cmp(OpGt, v) }
func (c Col) Ge(v any) Cond { return c.cmp(OpGe, v) }

// Between 等价于 Ge(low) AND Le(high)。
func (c Col) Between(low, high any) Cond { return c.Ge(low).And(c.Le(high)) }

// In 判断字段值是否在给定集合内。
func (c Col) In(values ...any) Cond { return c.test(OpIn, values...) }

func (c Col) IsNull() Cond { return fixed(Expr{Op: OpIsNull, Field: c.Field}, nil) }
func (c Col) Exists() Cond { return fixed(Expr{Op: OpExists, Field: c.Field}, nil) }

func (c Col) Contains(s string) Cond   { return c.test(OpContains, s) }
func (c Col) StartsWith(s string) Cond { return c.test(OpStartsWith, s) }
func (c Col) EndsWith(s string) Cond   { return c.test(OpEndsWith, s) }
func (c Col) Like(s string) Cond       { return c.test(OpLike, s) }
func (c Col) Regex(s string) Cond      { return c.test(OpRegex, s) }
func (c Col) IContains(s string) Cond  { return c.test(OpIContains, s) }
func (c Col) ILike(s string) Cond      { return c.test(OpILike, s) }

// Fuzzy 判断整个字段（UTF-8）与 s 的编辑距离不超过 distance（0..8）。
func (c Col) Fuzzy(s string, distance int) Cond {
	return fixed(Expr{Op: OpFuzzy, Field: c.Field, Values: []Value{StringValue(s), IntValue(int64(distance))}}, nil)
}

func (c Col) column() Column { return Column{Field: c.Field} }

// Sort 是带方向的排序项。
type Sort struct {
	build func(cols []Column) (Order, error)
}

// Orderable 可用于 OrderBy：Col、Agg、Sort。
type Orderable interface{ sort() Sort }

func nullsFirst(v []bool) bool { return len(v) == 0 || v[0] }

func (c Col) sort() Sort { return c.Asc() }
func (c Col) Asc(nulls ...bool) Sort {
	nf := nullsFirst(nulls)
	return Sort{func([]Column) (Order, error) { return Order{Field: c.Field, NullsFirst: nf}, nil }}
}
func (c Col) Desc(nulls ...bool) Sort {
	nf := nullsFirst(nulls)
	return Sort{func([]Column) (Order, error) { return Order{Field: c.Field, Descending: true, NullsFirst: nf}, nil }}
}
func (s Sort) sort() Sort { return s }

// Agg 是聚合输出列；比较只用于 Having，也可用于 OrderBy（必须同时被 Select）。
type Agg struct{ col Column }

func Count() Agg        { return Agg{Column{Field: KeyField(0), Aggregate: AggCountAll}} }
func CountOf(c Col) Agg { return Agg{Column{Field: c.Field, Aggregate: AggCount}} }
func CountDistinct(c Col) Agg {
	return Agg{Column{Field: c.Field, Aggregate: AggCount, Distinct: true}}
}
func Sum(c Col) Agg { return Agg{Column{Field: c.Field, Aggregate: AggSum}} }
func Avg(c Col) Agg { return Agg{Column{Field: c.Field, Aggregate: AggAvg}} }
func Min(c Col) Agg { return Agg{Column{Field: c.Field, Aggregate: AggMin}} }
func Max(c Col) Agg { return Agg{Column{Field: c.Field, Aggregate: AggMax}} }

func (a Agg) column() Column { return a.col }

func outputIndex(cols []Column, want Column) (int, error) {
	for i, c := range cols {
		if c.Field.equal(want.Field) && c.Aggregate == want.Aggregate && c.Distinct == want.Distinct {
			return i, nil
		}
	}
	return 0, errors.New("fengqudb: aggregate used in having/order_by must also be selected")
}

func (a Agg) cmp(op Op, v any) Cond {
	lit, err := literal(castNone, v)
	return Cond{agg: true, build: func(cols []Column) (Expr, error) {
		if err != nil {
			return Expr{}, err
		}
		i, err := outputIndex(cols, a.col)
		if err != nil {
			return Expr{}, err
		}
		return Expr{Op: op, Field: ColumnField(i), Values: []Value{lit}}, nil
	}}
}

func (a Agg) Eq(v any) Cond { return a.cmp(OpEq, v) }
func (a Agg) Ne(v any) Cond { return a.cmp(OpNe, v) }
func (a Agg) Lt(v any) Cond { return a.cmp(OpLt, v) }
func (a Agg) Le(v any) Cond { return a.cmp(OpLe, v) }
func (a Agg) Gt(v any) Cond { return a.cmp(OpGt, v) }
func (a Agg) Ge(v any) Cond { return a.cmp(OpGe, v) }

func (a Agg) sort() Sort { return a.Asc() }
func (a Agg) order(desc, nf bool) Sort {
	return Sort{func(cols []Column) (Order, error) {
		i, err := outputIndex(cols, a.col)
		return Order{Field: ColumnField(i), Descending: desc, NullsFirst: nf}, err
	}}
}
func (a Agg) Asc(nulls ...bool) Sort  { return a.order(false, nullsFirst(nulls)) }
func (a Agg) Desc(nulls ...bool) Sort { return a.order(true, nullsFirst(nulls)) }

// Selectable 可用于 Select：Col 或 Agg。
type Selectable interface{ column() Column }

// Builder 是链式查询构建器。方法记录第一个错误，Build/Run 时统一返回。
type Builder struct {
	q             Query
	where, having *Cond
	order         []Sort
	err           error
	selected      []Column
}

// From 以字符串前缀开始构建。
func From(prefix string) *Builder { return FromBytes([]byte(prefix)) }

// FromBytes 以字节前缀开始构建。
func FromBytes(prefix []byte) *Builder {
	b := &Builder{}
	b.q = *NewQuery(append([]byte(nil), prefix...))
	return b
}

func (b *Builder) fail(err error) {
	if b.err == nil {
		b.err = err
	}
}

func (b *Builder) Where(c Cond) *Builder {
	if c.agg {
		b.fail(errors.New("fengqudb: aggregates belong in Having()"))
	}
	b.where = &c
	return b
}
func (b *Builder) Select(items ...Selectable) *Builder {
	b.selected = b.selected[:0]
	for _, it := range items {
		b.selected = append(b.selected, it.column())
	}
	return b
}
func (b *Builder) GroupBy(cols ...Col) *Builder {
	b.q.Group = b.q.Group[:0]
	for _, c := range cols {
		b.q.Group = append(b.q.Group, c.Field)
	}
	return b
}
func (b *Builder) Having(c Cond) *Builder { b.having = &c; return b }
func (b *Builder) OrderBy(items ...Orderable) *Builder {
	b.order = b.order[:0]
	for _, it := range items {
		b.order = append(b.order, it.sort())
	}
	return b
}

// Join 追加一个连接：left 为主源（或之前来源）的字段，right 为被连接前缀的字段。
func (b *Builder) Join(prefix string, left, right Col, kind JoinKind) *Builder {
	b.q.Joins = append(b.q.Joins, Join{Prefix: []byte(prefix), Left: []Field{left.Field}, Right: []Field{right.Field}, Kind: kind})
	return b
}
func (b *Builder) Limit(n uint64) *Builder      { b.q.Limit = n; return b }
func (b *Builder) Offset(n uint64) *Builder     { b.q.Offset = n; return b }
func (b *Builder) After(cursor []byte) *Builder { b.q.After = append([]byte(nil), cursor...); return b }
func (b *Builder) Distinct(on bool) *Builder    { b.q.Distinct = on; return b }
func (b *Builder) CountTotal(on bool) *Builder  { b.q.CountTotal = on; return b }
func (b *Builder) AllowScan(on bool) *Builder   { b.q.AllowScan = on; return b }
func (b *Builder) Index(name string) *Builder   { b.q.IndexHint = []byte(name); return b }
func (b *Builder) Memory(n uint64) *Builder     { b.q.MemoryBytes = n; return b }
func (b *Builder) TimeoutMs(n uint64) *Builder  { b.q.TimeoutMs = n; return b }

// Build 返回等价的底层 Query。
func (b *Builder) Build() (*Query, error) {
	if b.err != nil {
		return nil, b.err
	}
	q := b.q
	q.Columns = append([]Column(nil), b.selected...)
	q.Group = append([]Field(nil), b.q.Group...)
	q.Joins = append([]Join(nil), b.q.Joins...)
	q.Order = nil
	for _, s := range b.order {
		o, err := s.build(b.selected)
		if err != nil {
			return nil, err
		}
		q.Order = append(q.Order, o)
	}
	if b.where != nil {
		e, err := b.where.build(b.selected)
		if err != nil {
			return nil, err
		}
		q.Where = e
	}
	if b.having != nil {
		e, err := b.having.build(b.selected)
		if err != nil {
			return nil, err
		}
		q.Having = e
	}
	return &q, nil
}

// Rows 是带列映射的查询结果，可用构建时的 Col/Agg 取值。
type Rows struct {
	*Result
	cols []Column
}

// Value 返回当前行中被 Select 的列的值。
func (r *Rows) Value(item Selectable) (Value, error) {
	i, err := outputIndex(r.cols, item.column())
	if err != nil {
		return Value{}, errors.New("fengqudb: column was not selected")
	}
	return r.Result.Value(i)
}

// Run 执行查询。返回的 Rows 用完需 Close。
func (b *Builder) Run(db Database) (*Rows, error) {
	q, err := b.Build()
	if err != nil {
		return nil, err
	}
	res, err := q.Execute(db)
	if err != nil {
		return nil, err
	}
	return &Rows{Result: res, cols: q.Columns}, nil
}

// Explain 返回访问计划。
func (b *Builder) Explain(db Database) (*Result, error) {
	q, err := b.Build()
	if err != nil {
		return nil, err
	}
	return q.Explain(db)
}

// Prepare 编码一次，之后可反复执行。
func (b *Builder) Prepare() (*PreparedQuery, error) {
	q, err := b.Build()
	if err != nil {
		return nil, err
	}
	return q.Prepare()
}

// Pages 逐页执行并对每页调用 fn（见 Query.Pages）。
func (b *Builder) Pages(db Database, fn func(*Rows) error) error {
	q, err := b.Build()
	if err != nil {
		return err
	}
	return q.Pages(db, func(r *Result) error { return fn(&Rows{Result: r, cols: q.Columns}) })
}
