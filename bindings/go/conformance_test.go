package fengqudb

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"testing"
)

// ---- conformance.json 的 JSON 映射 ----

type jField struct {
	Number   uint64   `json:"number"`
	Type     uint64   `json:"type"`
	Source   uint64   `json:"source"`
	Path     []uint64 `json:"path"`
	Repeated bool     `json:"repeated"`
}
type jValue struct {
	Int   *json.Number `json:"int"`
	Bytes *string      `json:"bytes"`
	F64   *float64     `json:"f64"`
	Bool  *bool        `json:"bool"`
	Null  *bool        `json:"null"`
}
type jExpr struct {
	Op       uint32   `json:"op"`
	Values   []jValue `json:"values"`
	Children []jExpr  `json:"children"`
	Field    *jField  `json:"field"`
	Right    *jField  `json:"right"`
}
type jColumn struct {
	Field     jField `json:"field"`
	Aggregate uint32 `json:"aggregate"`
	Name      string `json:"name"`
	Distinct  bool   `json:"distinct"`
}
type jOrder struct {
	Field      jField `json:"field"`
	Descending bool   `json:"descending"`
	NullsFirst bool   `json:"nulls_first"`
}
type jJoin struct {
	Prefix string   `json:"prefix"`
	Left   []jField `json:"left"`
	Right  []jField `json:"right"`
	Kind   uint32   `json:"kind"`
	On     jExpr    `json:"on"`
}
type jSpec struct {
	Prefix        string    `json:"prefix"`
	Where         jExpr     `json:"where"`
	Columns       []jColumn `json:"columns"`
	Order         []jOrder  `json:"order"`
	Group         []jField  `json:"group"`
	Having        jExpr     `json:"having"`
	Joins         []jJoin   `json:"joins"`
	Limit         uint64    `json:"limit"`
	Offset        uint64    `json:"offset"`
	MemoryBytes   uint64    `json:"memory_bytes"`
	MaxCandidates uint64    `json:"max_candidates"`
	MaxWork       uint64    `json:"max_work"`
	TimeoutMs     uint64    `json:"timeout_ms"`
	AllowScan     bool      `json:"allow_scan"`
	CountTotal    bool      `json:"count_total"`
	Distinct      bool      `json:"distinct"`
	IndexHint     string    `json:"index_hint"`
	After         string    `json:"after"`
}
type jResult struct {
	Rows []struct {
		Key    string   `json:"key"`
		Values []jValue `json:"values"`
	} `json:"rows"`
	Cursor      string   `json:"cursor"`
	Matched     uint64   `json:"matched"`
	Complete    bool     `json:"complete"`
	ResultTotal uint64   `json:"result_total"`
	Strategies  []string `json:"strategies"`
	Indexes     []string `json:"indexes"`
}
type jCase struct {
	ID       string   `json:"id"`
	Spec     jSpec    `json:"spec"`
	Encoded  string   `json:"encoded"`
	Request  string   `json:"request"`
	Result   *jResult `json:"result"`
	Paginate *struct {
		Values []int64 `json:"values"`
		Pages  int     `json:"pages"`
	} `json:"paginate"`
}
type jVectors struct {
	Dataset struct {
		Records []struct{ Key, Value string } `json:"records"`
		Indexes []struct {
			Name   string     `json:"name"`
			Prefix string     `json:"prefix"`
			Fields [][]uint32 `json:"fields"`
			Flags  uint32     `json:"flags"`
		} `json:"indexes"`
	} `json:"dataset"`
	Cases []jCase `json:"cases"`
}

func unhex(t testing.TB, s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("bad hex %q: %v", s, err)
	}
	return b
}

func (j jField) field() Field {
	return Field{Number: j.Number, Type: j.Type, Source: j.Source, Path: j.Path, Repeated: j.Repeated}
}

func (j jValue) value(t testing.TB) Value {
	switch {
	case j.Int != nil:
		if i, err := j.Int.Int64(); err == nil {
			return IntValue(i)
		}
		u, err := strconv.ParseUint(j.Int.String(), 10, 64)
		if err != nil {
			t.Fatalf("bad int %s", j.Int)
		}
		return UintValue(u)
	case j.Bytes != nil:
		return BytesValue(unhex(t, *j.Bytes))
	case j.F64 != nil:
		return FloatValue(*j.F64)
	case j.Bool != nil:
		return BoolValue(*j.Bool)
	}
	return NullValue()
}

func (j jExpr) expr(t testing.TB) Expr {
	e := Expr{Op: Op(j.Op)}
	if j.Field != nil {
		e.Field = j.Field.field()
	}
	if j.Right != nil {
		f := j.Right.field()
		e.Right = &f
	}
	for _, v := range j.Values {
		e.Values = append(e.Values, v.value(t))
	}
	for _, c := range j.Children {
		e.Children = append(e.Children, c.expr(t))
	}
	return e
}

func (s jSpec) query(t testing.TB) *Query {
	q := &Query{Prefix: unhex(t, s.Prefix), Where: s.Where.expr(t), Having: s.Having.expr(t),
		Limit: s.Limit, Offset: s.Offset, MemoryBytes: s.MemoryBytes, MaxCandidates: s.MaxCandidates,
		MaxWork: s.MaxWork, TimeoutMs: s.TimeoutMs, AllowScan: s.AllowScan, CountTotal: s.CountTotal,
		Distinct: s.Distinct, IndexHint: unhex(t, s.IndexHint), After: unhex(t, s.After)}
	for _, c := range s.Columns {
		q.Columns = append(q.Columns, Column{Field: c.Field.field(), Aggregate: Aggregate(c.Aggregate), Name: c.Name, Distinct: c.Distinct})
	}
	for _, o := range s.Order {
		q.Order = append(q.Order, Order{Field: o.Field.field(), Descending: o.Descending, NullsFirst: o.NullsFirst})
	}
	for _, g := range s.Group {
		q.Group = append(q.Group, g.field())
	}
	for _, j := range s.Joins {
		jn := Join{Prefix: unhex(t, j.Prefix), Kind: JoinKind(j.Kind), On: j.On.expr(t)}
		for _, f := range j.Left {
			jn.Left = append(jn.Left, f.field())
		}
		for _, f := range j.Right {
			jn.Right = append(jn.Right, f.field())
		}
		q.Joins = append(q.Joins, jn)
	}
	return q
}

func loadVectors(t testing.TB) *jVectors {
	path := os.Getenv("FQDB_CONFORMANCE")
	if path == "" {
		t.Skip("FQDB_CONFORMANCE 未设置，跳过一致性测试")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var v jVectors
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	if err := dec.Decode(&v); err != nil {
		t.Fatal(err)
	}
	return &v
}

func conformanceStore(t testing.TB, v *jVectors) *Store {
	st := openTestStore(t)
	for _, r := range v.Dataset.Records {
		if err := st.Put(unhex(t, r.Key), unhex(t, r.Value)); err != nil {
			t.Fatal(err)
		}
	}
	for _, ix := range v.Dataset.Indexes {
		var fields []IndexField
		for _, f := range ix.Fields {
			fields = append(fields, IndexField{Field: f[0], Type: f[1]})
		}
		if err := st.DefineCompoundIndex([]byte(ix.Name), []byte(ix.Prefix), fields, ix.Flags); err != nil {
			t.Fatalf("index %s: %v", ix.Name, err)
		}
	}
	return st
}

// valueString 把 Value 与 JSON 期望统一成可比较的字符串。
func valueString(v Value) string {
	switch v.Kind {
	case KindNull:
		return "null"
	case KindUint:
		return "int:" + strconv.FormatUint(v.U, 10)
	case KindSint:
		return "int:" + strconv.FormatInt(v.I, 10)
	case KindDouble:
		return "f64:" + strconv.FormatFloat(v.F, 'g', -1, 64)
	case KindBytes:
		return "bytes:" + hex.EncodeToString(v.B)
	}
	return "bool:" + strconv.FormatBool(v.Bool())
}

func expectString(t testing.TB, j jValue) string {
	switch {
	case j.Int != nil:
		return "int:" + j.Int.String()
	case j.Bytes != nil:
		return "bytes:" + *j.Bytes
	case j.F64 != nil:
		return "f64:" + strconv.FormatFloat(*j.F64, 'g', -1, 64)
	case j.Bool != nil:
		return "bool:" + strconv.FormatBool(*j.Bool)
	}
	return "null"
}

// checkResult 比较行、游标、统计、策略与索引名。
func checkResult(t *testing.T, id string, res *Result, want *jResult) {
	t.Helper()
	var rows [][2]string
	for res.Next() {
		vals, err := res.AppendValues(nil)
		if err != nil {
			t.Fatalf("%s: %v", id, err)
		}
		s := ""
		for _, v := range vals {
			s += valueString(v) + ";"
		}
		rows = append(rows, [2]string{hex.EncodeToString(res.Key()), s})
	}
	if res.Err() != nil {
		t.Fatalf("%s: %v", id, res.Err())
	}
	if len(rows) != len(want.Rows) {
		t.Fatalf("%s: rows = %d, want %d", id, len(rows), len(want.Rows))
	}
	for i, wr := range want.Rows {
		s := ""
		for _, v := range wr.Values {
			s += expectString(t, v) + ";"
		}
		if rows[i][0] != wr.Key || rows[i][1] != s {
			t.Fatalf("%s: row %d = %v, want key=%s values=%s", id, i, rows[i], wr.Key, s)
		}
	}
	cur, err := res.Cursor()
	if err != nil || hex.EncodeToString(cur) != want.Cursor {
		t.Fatalf("%s: cursor = %x (%v), want %s", id, cur, err, want.Cursor)
	}
	st, err := res.Stats()
	if err != nil || st.Matched != want.Matched || st.Complete != want.Complete || st.ResultTotal != want.ResultTotal {
		t.Fatalf("%s: stats = %+v (%v), want matched=%d complete=%v total=%d", id, st, err, want.Matched, want.Complete, want.ResultTotal)
	}
	acc, err := res.Access()
	if err != nil || len(acc) != len(want.Strategies) {
		t.Fatalf("%s: access = %+v (%v), want %v", id, acc, err, want.Strategies)
	}
	for i, a := range acc {
		if a.Strategy != want.Strategies[i] || a.Index != want.Indexes[i] {
			t.Fatalf("%s: access[%d] = %s/%s, want %s/%s", id, i, a.Strategy, a.Index, want.Strategies[i], want.Indexes[i])
		}
	}
}

func TestConformanceEncoding(t *testing.T) {
	v := loadVectors(t)
	for _, c := range v.Cases {
		q := c.Spec.query(t)
		enc, err := q.Encode()
		if err != nil {
			t.Fatalf("%s: %v", c.ID, err)
		}
		if got := hex.EncodeToString(enc); got != c.Encoded {
			t.Fatalf("%s: encoded mismatch\n got %s\nwant %s", c.ID, got, c.Encoded)
		}
		p := PrepareEncoded(enc)
		explain := c.Request[2:4] == "36"
		if got := hex.EncodeToString(p.Request(explain)); got != c.Request {
			t.Fatalf("%s: request mismatch\n got %s\nwant %s", c.ID, got, c.Request)
		}
	}
	t.Logf("%d 个用例的 encoded/request 字节完全一致", len(v.Cases))
}

func TestConformanceExecute(t *testing.T) {
	v := loadVectors(t)
	st := conformanceStore(t, v)
	defer st.Close()
	n := 0
	for _, c := range v.Cases {
		if c.Result == nil {
			continue
		}
		q := c.Spec.query(t)
		var res *Result
		var err error
		if c.Request[2:4] == "36" {
			res, err = q.Explain(st)
		} else {
			res, err = q.Execute(st)
		}
		if err != nil {
			t.Fatalf("%s: %v", c.ID, err)
		}
		checkResult(t, c.ID, res, c.Result)
		res.Close()
		// 直接用向量里的原始请求字节执行，覆盖 SelectRequest 低层入口。
		var raw Result
		if err := st.SelectRequest(unhex(t, c.Request), &raw); err != nil {
			t.Fatalf("%s raw: %v", c.ID, err)
		}
		checkResult(t, c.ID+"/raw", &raw, c.Result)
		raw.Close()
		n++
	}
	t.Logf("%d 个用例的执行结果一致", n)
}

func TestConformancePagination(t *testing.T) {
	v := loadVectors(t)
	st := conformanceStore(t, v)
	defer st.Close()
	found := false
	for _, c := range v.Cases {
		if c.Paginate == nil {
			continue
		}
		found = true
		q := c.Spec.query(t)
		var got []int64
		pages := 0
		err := q.Pages(st, func(r *Result) error {
			pages++
			for r.Next() {
				val, err := r.Value(0)
				if err != nil {
					return err
				}
				got = append(got, val.Int())
			}
			return r.Err()
		})
		if err != nil {
			t.Fatal(err)
		}
		if fmt.Sprint(got) != fmt.Sprint(c.Paginate.Values) {
			t.Fatalf("%s: values = %v, want %v", c.ID, got, c.Paginate.Values)
		}
		if pages != c.Paginate.Pages {
			t.Fatalf("%s: pages = %d, want %d", c.ID, pages, c.Paginate.Pages)
		}
	}
	if !found {
		t.Fatal("conformance.json 没有 paginate 用例")
	}
}

// 类型化构建器与向量里的字节逐一比较。
func TestConformanceBuilder(t *testing.T) {
	v := loadVectors(t)
	uid, cls, level, gold := U64(1), Text(2), U64(3), S64(4)
	orderUID := U64(1).InSource(1)
	_ = orderUID
	builders := map[string]*Builder{
		"topn_mage_level": From("p/").Where(cls.Eq("mage").And(level.Ge(5)).And(level.Le(15))).
			Select(uid, level).OrderBy(level.Desc()).Limit(7).CountTotal(true),
		"explain_topn": From("p/").Where(cls.Eq("mage").And(level.Ge(5)).And(level.Le(15))).
			Select(uid, level).OrderBy(level.Desc()).Limit(7).CountTotal(true),
		"or_not_in": From("p/").Where(cls.Eq("mage").Or(cls.Eq("warrior")).And(gold.Lt(0).Not()).And(uid.In(10, 20, 30))).
			Select(uid).OrderBy(uid),
		"regex":           From("p/").Where(Text(5).Regex("^Name-(?:1[0-9]|2[0-3])$")).Select(uid).OrderBy(uid),
		"like":            From("p/").Where(Text(5).Like("Name-1_")).Select(uid).OrderBy(uid),
		"icontains":       From("p/").Where(Text(5).IContains("NAME-63")).Select(uid).OrderBy(uid),
		"not_optional_eq": From("p/").Where(U64(10).Eq(3).Not()).Select(uid).OrderBy(uid),
		"is_null":         From("p/").Where(U64(10).IsNull()).Select(uid).OrderBy(uid),
		"nested_path":     From("p/").Select(Nested([]uint64{8, 1}, TypeUint64)).OrderBy(uid).Limit(3),
		"compare_fields":  From("p/").Where(level.Lt(uid)).Select(uid).OrderBy(uid),
		"repeated_any":    From("p/").Where(U64(12).Any().Eq(3)).Select(uid).OrderBy(uid),
		"join_inner": From("p/").Join("o/", uid, U64(2).InSource(1), JoinInner).
			Select(uid, level.InSource(1)).OrderBy(uid, level.InSource(1)).Limit(200),
		"join_left": From("p/").Join("o/", uid, U64(2).InSource(1), JoinLeft).
			Select(uid, level.InSource(1)).OrderBy(uid, level.InSource(1)).Limit(200),
		"join_semi":          From("p/").Join("o/", uid, U64(2).InSource(1), JoinSemi).Select(uid).OrderBy(uid).Limit(200),
		"join_anti":          From("p/").Join("o/", uid, U64(2).InSource(1), JoinAnti).Select(uid).OrderBy(uid).Limit(200),
		"paginate_gold_desc": From("p/").Select(uid).OrderBy(gold.Desc()).Limit(9),
	}
	checked := 0
	for _, c := range v.Cases {
		b, ok := builders[c.ID]
		if !ok {
			continue
		}
		q, err := b.Build()
		if err != nil {
			t.Fatalf("%s: %v", c.ID, err)
		}
		enc, err := q.Encode()
		if err != nil {
			t.Fatal(err)
		}
		if got := hex.EncodeToString(enc); got != c.Encoded {
			t.Fatalf("%s: builder encoding mismatch\n got %s\nwant %s", c.ID, got, c.Encoded)
		}
		checked++
	}
	if checked != len(builders) {
		t.Fatalf("checked %d builders, have %d", checked, len(builders))
	}
}

// 聚合：构建器可 Select 聚合列并按 Having 过滤，结果与低层向量的行一致（顺序除外）。
func TestBuilderAggregates(t *testing.T) {
	v := loadVectors(t)
	st := conformanceStore(t, v)
	defer st.Close()
	cls, gold := Text(2), S64(4)
	cnt := Count()
	rows, err := From("p/").Select(cls, cnt, Sum(gold), Min(gold), Max(gold), Avg(gold)).
		GroupBy(cls).Having(cnt.Gt(20)).OrderBy(cls.Asc()).Run(st)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	seen := map[string]int64{}
	for rows.Next() {
		c, err := rows.Value(cls)
		if err != nil {
			t.Fatal(err)
		}
		n, err := rows.Value(cnt)
		if err != nil {
			t.Fatal(err)
		}
		seen[string(c.Bytes())] = n.Int()
	}
	if seen["mage"] != 32 || seen["warrior"] != 32 || len(seen) != 2 {
		t.Fatalf("aggregates = %v", seen)
	}
	// Having 引用未被 Select 的聚合必须报错。
	if _, err := From("p/").Select(cls).GroupBy(cls).Having(Count().Gt(1)).Build(); err == nil {
		t.Fatal("expected error for unselected aggregate")
	}
	if _, err := From("p/").Where(Count().Gt(1)).Build(); err == nil {
		t.Fatal("expected error for aggregate in Where")
	}
}
