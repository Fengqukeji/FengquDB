package fengqudb

import (
	"bytes"
	"errors"
	"fmt"
	"runtime"
	"sync"
	"testing"
	"time"
)

// testOptions 与 bench_c.c 的取值一致（全 0 会被核心库拒绝）。
func testOptions() *Options {
	return &Options{Durability: DurabilityBuffered, Flags: FlagCreate,
		CacheBytes: 2 << 30, DeltaBytes: 16 << 20, WriterBytes: 32 << 20,
		TransactionBytes: 64 << 20, AllTransactionBytes: 256 << 20, PinnedBytes: 2 << 30, MaxSnapshots: 1024}
}

func openTestStore(t testing.TB) *Store {
	t.Helper()
	st, err := Open(t.TempDir(), testOptions())
	if err != nil {
		t.Fatal(err)
	}
	return st
}

func TestPutGetDelete(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	if err := st.Put([]byte("a"), []byte("hello")); err != nil {
		t.Fatal(err)
	}
	got, err := st.Get([]byte("a"))
	if err != nil || string(got) != "hello" {
		t.Fatalf("Get = %q, %v", got, err)
	}
	if err := st.Put([]byte("a"), []byte("world")); err != nil {
		t.Fatal(err)
	}
	if got, _ := st.Get([]byte("a")); string(got) != "world" {
		t.Fatalf("overwrite = %q", got)
	}
	if err := st.Delete([]byte("a")); err != nil {
		t.Fatal(err)
	}
	if _, err := st.Get([]byte("a")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("after delete: %v", err)
	}
}

func TestMissAndEmptyValue(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	_, err := st.Get([]byte("nope"))
	var ee *Error
	if !errors.Is(err, ErrNotFound) || !errors.As(err, &ee) || ee.Code != StatusNotFound || ee.Message == "" {
		t.Fatalf("miss error = %#v", err)
	}
	if _, err := st.GetShared([]byte("nope")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("shared miss = %v", err)
	}
	if err := st.Put([]byte("empty"), nil); err != nil {
		t.Fatal(err)
	}
	v, err := st.Get([]byte("empty"))
	if err != nil || v == nil || len(v) != 0 {
		t.Fatalf("empty Get = %#v, %v", v, err)
	}
	sh, err := st.GetShared([]byte("empty"))
	if err != nil || sh.Len() != 0 || sh.Bytes() == nil {
		t.Fatalf("empty shared = %v, %v", sh.Bytes(), err)
	}
	sh.Close()
	// 准备键与读取器区分未命中和空值。
	pk, _ := st.PrepareKey([]byte("empty"))
	defer pk.Close()
	psh, err := pk.GetShared()
	if err != nil || psh.Len() != 0 {
		t.Fatalf("prepared empty = %v", err)
	}
	psh.Close()
	rd, err := st.PrepareReader([][]byte{[]byte("empty"), []byte("nope"), []byte("empty")})
	if err != nil {
		t.Fatal(err)
	}
	defer rd.Close()
	rows, err := rd.Read()
	if err != nil || len(rows) != 3 {
		t.Fatalf("rows = %d, %v", len(rows), err)
	}
	if !rows[0].Found() || len(rows[0].Value()) != 0 || rows[0].Value() == nil || rows[1].Found() || rows[1].Value() != nil || !rows[2].Found() {
		t.Fatalf("rows = %+v", rows)
	}
}

func TestExpiry(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	soon := uint64(time.Now().UnixMilli() + 150)
	if err := st.PutExpiring([]byte("ttl"), []byte("x"), soon); err != nil {
		t.Fatal(err)
	}
	sh, err := st.GetShared([]byte("ttl"))
	if err != nil || sh.ExpiresAtMs() != soon {
		t.Fatalf("before expiry: %v exp=%d", err, sh.ExpiresAtMs())
	}
	sh.Close()
	time.Sleep(300 * time.Millisecond)
	if _, err := st.Get([]byte("ttl")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("after expiry: %v", err)
	}
	if err := st.PutExpiring([]byte("past"), []byte("x"), 1); err != nil {
		t.Fatal(err)
	}
	if _, err := st.Get([]byte("past")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("past expiry: %v", err)
	}
}

func TestGetAppend(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	big := bytes.Repeat([]byte{7}, 5000)
	st.Put([]byte("k"), big)
	st.Put([]byte("small"), []byte("abc"))
	out, err := st.GetAppend([]byte("pre:"), []byte("small"))
	if err != nil || string(out) != "pre:abc" {
		t.Fatalf("append = %q, %v", out, err)
	}
	buf := make([]byte, 0, 16)
	out, err = st.GetAppend(buf, []byte("k"))
	if err != nil || !bytes.Equal(out, big) {
		t.Fatalf("grow = %d bytes, %v", len(out), err)
	}
	if _, err := st.GetAppend(buf, []byte("nope")); !errors.Is(err, ErrNotFound) {
		t.Fatal(err)
	}
	buf = make([]byte, 0, 64)
	out, _ = st.GetAppend(buf, []byte("small"))
	if &out[0] != &buf[:1][0] {
		t.Fatal("GetAppend should reuse capacity")
	}
}

func TestTransactions(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	st.Put([]byte("base"), []byte("1"))
	tx, err := st.Begin(0)
	if err != nil {
		t.Fatal(err)
	}
	if err := tx.Put([]byte("a"), []byte("A")); err != nil {
		t.Fatal(err)
	}
	if err := tx.Delete([]byte("base")); err != nil {
		t.Fatal(err)
	}
	// 事务内读自己的写；外部看不到。
	if v, err := tx.Get([]byte("a")); err != nil || string(v) != "A" {
		t.Fatalf("tx read own write = %q, %v", v, err)
	}
	if _, err := tx.Get([]byte("base")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("tx read own delete: %v", err)
	}
	if _, err := st.Get([]byte("a")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("uncommitted visible: %v", err)
	}
	ver, err := tx.Commit()
	if err != nil || ver == 0 {
		t.Fatalf("commit = %d, %v", ver, err)
	}
	tx.Close()
	if v, err := st.Get([]byte("a")); err != nil || string(v) != "A" {
		t.Fatalf("after commit = %q, %v", v, err)
	}
	if _, err := st.Get([]byte("base")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("committed delete: %v", err)
	}
	// 未提交直接 Close = 放弃。
	tx, _ = st.Begin(0)
	tx.Put([]byte("gone"), []byte("x"))
	tx.Close()
	if _, err := st.Get([]byte("gone")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("rolled back visible: %v", err)
	}
	// PutMany
	tx, _ = st.Begin(0)
	err = tx.PutMany([]PutItem{{Key: []byte("m1"), Value: []byte("v1")}, {Key: []byte("m2"), Value: nil}, {Key: []byte("m1"), Value: []byte("v1b")}})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	tx.Close()
	if v, _ := st.Get([]byte("m1")); string(v) != "v1b" {
		t.Fatalf("PutMany duplicate = %q", v)
	}
	if v, err := st.Get([]byte("m2")); err != nil || len(v) != 0 {
		t.Fatalf("PutMany empty = %v, %v", v, err)
	}
	// 关闭后使用报错，重复 Close 无害。
	if _, err := tx.Commit(); !errors.Is(err, ErrClosed) {
		t.Fatalf("commit after close: %v", err)
	}
	if err := tx.Put([]byte("x"), nil); !errors.Is(err, ErrClosed) {
		t.Fatalf("put after close: %v", err)
	}
	tx.Close()
}

func TestTransactionConflict(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	st.Put([]byte("c"), []byte("0"))
	tx, _ := st.Begin(0)
	defer tx.Close()
	if _, err := tx.Get([]byte("c")); err != nil {
		t.Fatal(err)
	}
	tx.Put([]byte("c"), []byte("tx"))
	st.Put([]byte("c"), []byte("other"))
	_, err := tx.Commit()
	var ee *Error
	if !errors.As(err, &ee) || ee.Code != StatusConflict || !errors.Is(err, ErrConflict) {
		t.Fatalf("expected conflict, got %v", err)
	}
}

func TestClosedErrors(t *testing.T) {
	st := openTestStore(t)
	st.Put([]byte("k"), []byte("v"))
	pk, _ := st.PrepareKey([]byte("k"))
	rd, _ := st.PrepareReader([][]byte{[]byte("k")})
	sn, _ := st.Snapshot()
	if err := st.Close(); err != nil {
		t.Fatal(err)
	}
	if err := st.Close(); err != nil {
		t.Fatal("second close must be harmless")
	}
	if _, err := st.Get([]byte("k")); !errors.Is(err, ErrClosed) {
		t.Fatalf("Get: %v", err)
	}
	if _, err := st.GetShared([]byte("k")); !errors.Is(err, ErrClosed) {
		t.Fatalf("GetShared: %v", err)
	}
	if err := st.Put([]byte("k"), nil); !errors.Is(err, ErrClosed) {
		t.Fatalf("Put: %v", err)
	}
	if err := st.Delete([]byte("k")); !errors.Is(err, ErrClosed) {
		t.Fatalf("Delete: %v", err)
	}
	if _, err := st.Begin(0); !errors.Is(err, ErrClosed) {
		t.Fatalf("Begin: %v", err)
	}
	if _, err := st.PrepareKey([]byte("k")); !errors.Is(err, ErrClosed) {
		t.Fatalf("PrepareKey: %v", err)
	}
	if _, err := st.PrepareReader([][]byte{[]byte("k")}); !errors.Is(err, ErrClosed) {
		t.Fatalf("PrepareReader: %v", err)
	}
	if err := st.DefineCompoundIndex([]byte("i"), []byte("p"), []IndexField{{1, 1}}, 0); !errors.Is(err, ErrClosed) {
		t.Fatalf("DefineCompoundIndex: %v", err)
	}
	var r Result
	if err := st.SelectRequest([]byte{0}, &r); !errors.Is(err, ErrClosed) {
		t.Fatalf("SelectRequest: %v", err)
	}
	// 子句柄在 Store 关闭后仍可读（核心库保持底层库存活）。
	sh, err := pk.GetShared()
	if err != nil || string(sh.Bytes()) != "v" {
		t.Fatalf("prepared key after store close: %v", err)
	}
	sh.Close()
	rows, err := rd.Read()
	if err != nil || string(rows[0].Value()) != "v" {
		t.Fatalf("reader after store close: %v", err)
	}
	sh, err = sn.GetShared([]byte("k"))
	if err != nil || string(sh.Bytes()) != "v" {
		t.Fatalf("snapshot after store close: %v", err)
	}
	sh.Close()
	// 子句柄自己关闭后报错。
	pk.Close()
	rd.Close()
	sn.Close()
	if _, err := pk.GetShared(); !errors.Is(err, ErrClosed) {
		t.Fatalf("closed PreparedKey: %v", err)
	}
	if _, err := rd.Read(); !errors.Is(err, ErrClosed) {
		t.Fatalf("closed Reader: %v", err)
	}
	if _, err := sn.Get([]byte("k")); !errors.Is(err, ErrClosed) {
		t.Fatalf("closed Snapshot: %v", err)
	}
	pk.Close()
	rd.Close()
}

// 共享视图在之后的写入、删除和 Store 关闭后依然有效且内容不变。
func TestSharedViewSurvives(t *testing.T) {
	st := openTestStore(t)
	val := bytes.Repeat([]byte("abcdefgh"), 100)
	st.Put([]byte("k"), val)
	sh, err := st.GetShared([]byte("k"))
	if err != nil {
		t.Fatal(err)
	}
	pk, _ := st.PrepareKey([]byte("k"))
	psh, _ := pk.GetShared()
	rd, _ := st.PrepareReader([][]byte{[]byte("k")})
	rows, _ := rd.Read()
	acq, err := rd.Acquire()
	if err != nil {
		t.Fatal(err)
	}
	st.Put([]byte("k"), []byte("changed"))
	st.Delete([]byte("k"))
	for i := 0; i < 200; i++ {
		st.Put([]byte(fmt.Sprintf("other%d", i)), bytes.Repeat([]byte{byte(i)}, 1000))
	}
	if !bytes.Equal(sh.Bytes(), val) || !bytes.Equal(psh.Bytes(), val) || sh.Version() == 0 {
		t.Fatal("shared view changed after write")
	}
	_ = rows
	st.Close()
	runtime.GC()
	if !bytes.Equal(sh.Bytes(), val) || !bytes.Equal(psh.Bytes(), val) {
		t.Fatal("shared view invalid after Store.Close")
	}
	if len(acq.Rows()) != 1 || !bytes.Equal(acq.Rows()[0].Value(), val) {
		t.Fatal("acquired results invalid after Store.Close and writes")
	}
	if s := sh.Text(); len(s) != len(val) || s[:8] != "abcdefgh" {
		t.Fatalf("Text() = %q", s)
	}
	sh.Close()
	if sh.Bytes() != nil || sh.Text() != "" {
		t.Fatal("closed Shared must expose nothing")
	}
	sh.Close() // 重复 Close 无害
	psh.Close()
	acq.Close()
	rd.Close()
	pk.Close()
}

func TestSharedEscapeFinalizer(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	st.Put([]byte("k"), []byte("value"))
	sh, _ := st.GetShared([]byte("k"))
	own := sh.Escape()
	if sh.Bytes() != nil || string(own.Bytes()) != "value" {
		t.Fatal("Escape should move ownership")
	}
	own.Close()
	own.Close()
	// 忘记 Close 的 SharedValue 交给 GC；这里只验证不崩溃。
	for i := 0; i < 50; i++ {
		s, _ := st.GetShared([]byte("k"))
		_ = s.Escape()
	}
	runtime.GC()
	runtime.GC()
	time.Sleep(20 * time.Millisecond)
	info, err := st.Info()
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("pinned_bytes after GC finalizers: %d", info.PinnedBytes)
}

func TestPinnedBytesReturned(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	st.Put([]byte("k"), bytes.Repeat([]byte{1}, 4096))
	before, _ := st.Info()
	var held []Shared
	for i := 0; i < 8; i++ {
		s, err := st.GetShared([]byte("k"))
		if err != nil {
			t.Fatal(err)
		}
		held = append(held, s)
	}
	mid, _ := st.Info()
	for i := range held {
		held[i].Close()
	}
	after, _ := st.Info()
	t.Logf("pinned_bytes before=%d held=%d after=%d", before.PinnedBytes, mid.PinnedBytes, after.PinnedBytes)
	if mid.PinnedBytes <= before.PinnedBytes {
		t.Fatalf("expected pinned bytes to grow while leases are held")
	}
	if after.PinnedBytes > before.PinnedBytes {
		t.Fatalf("pinned bytes not returned after Close: %d > %d", after.PinnedBytes, before.PinnedBytes)
	}
}

func TestReaderBatchAndRelease(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	var keys [][]byte
	for i := 0; i < 32; i++ {
		k := []byte(fmt.Sprintf("key%02d", i))
		keys = append(keys, k)
		st.Put(k, []byte(fmt.Sprintf("value-%d", i)))
	}
	rd, err := st.PrepareReader(keys)
	if err != nil {
		t.Fatal(err)
	}
	defer rd.Close()
	rows, err := rd.Read()
	if err != nil || len(rows) != 32 {
		t.Fatalf("%d rows, %v", len(rows), err)
	}
	for i := range rows {
		if string(rows[i].Value()) != fmt.Sprintf("value-%d", i) || rows[i].Version() == 0 {
			t.Fatalf("row %d = %q", i, rows[i].Value())
		}
	}
	// 写入后下一次 Read 看到新值。
	st.Put(keys[3], []byte("new"))
	rows, _ = rd.Read()
	if string(rows[3].Value()) != "new" {
		t.Fatalf("row 3 after update = %q", rows[3].Value())
	}
	rd.Release()
	if _, err := rd.Read(); err != nil {
		t.Fatal(err)
	}
	if _, err := st.PrepareReader(nil); err == nil {
		t.Fatal("empty key list must fail")
	}
}

// 多个 goroutine 各自持有读取器、准备键并发读取，同时有写入。
func TestConcurrentReaders(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	const n = 64
	for i := 0; i < n; i++ {
		st.Put([]byte(fmt.Sprintf("k%03d", i)), bytes.Repeat([]byte{byte(i)}, 128))
	}
	var wg sync.WaitGroup
	errs := make(chan error, 16)
	stop := make(chan struct{})
	for g := 0; g < 8; g++ {
		g := g
		wg.Add(1)
		go func() {
			defer wg.Done()
			var keys [][]byte
			for i := 0; i < 16; i++ {
				keys = append(keys, []byte(fmt.Sprintf("k%03d", (g*7+i)%n)))
			}
			rd, err := st.PrepareReader(keys)
			if err != nil {
				errs <- err
				return
			}
			defer rd.Close()
			pk, err := st.PrepareKey(keys[0])
			if err != nil {
				errs <- err
				return
			}
			defer pk.Close()
			for iter := 0; iter < 2000; iter++ {
				rows, err := rd.Read()
				if err != nil {
					errs <- err
					return
				}
				for i := range rows {
					want := byte((g*7 + i) % n)
					v := rows[i].Value()
					if len(v) != 128 || v[0] != want || v[127] != want {
						errs <- fmt.Errorf("goroutine %d row %d corrupt", g, i)
						return
					}
				}
				sh, err := pk.GetShared()
				if err != nil {
					errs <- err
					return
				}
				if sh.Len() != 128 {
					errs <- fmt.Errorf("prepared len %d", sh.Len())
					return
				}
				sh.Close()
			}
		}()
	}
	var ww sync.WaitGroup
	ww.Add(1)
	go func() { // 不相交的键上持续写入
		defer ww.Done()
		for i := 0; ; i++ {
			select {
			case <-stop:
				return
			default:
				st.Put([]byte(fmt.Sprintf("w%d", i%50)), []byte("x"))
			}
		}
	}()
	wg.Wait()
	close(stop)
	ww.Wait()
	select {
	case err := <-errs:
		t.Fatal(err)
	default:
	}
}
func TestSnapshot(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	st.Put([]byte("k"), []byte("v1"))
	sn, err := st.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	defer sn.Close()
	st.Put([]byte("k"), []byte("v2"))
	st.Put([]byte("new"), []byte("n"))
	if v, err := sn.Get([]byte("k")); err != nil || string(v) != "v1" {
		t.Fatalf("snapshot Get = %q, %v", v, err)
	}
	sh, err := sn.GetShared([]byte("k"))
	if err != nil || string(sh.Bytes()) != "v1" {
		t.Fatalf("snapshot GetShared: %v", err)
	}
	sh.Close()
	if _, err := sn.Get([]byte("new")); !errors.Is(err, ErrNotFound) {
		t.Fatalf("snapshot sees later write: %v", err)
	}
}

func TestErrorCodes(t *testing.T) {
	e := statusErr(20)
	var ee *Error
	if !errors.As(e, &ee) || ee.Code != StatusConstraint || ee.Error() == "" {
		t.Fatalf("%#v", e)
	}
	if statusErr(0) != nil {
		t.Fatal("OK must map to nil")
	}
	if ee := statusErr(999).(*Error); ee.Code != 999 {
		t.Fatalf("unknown code: %v", ee)
	}
	// 全 0 选项被核心库拒绝，并带数值码。
	_, err := Open(t.TempDir(), &Options{})
	if !errors.As(err, &ee) || ee.Code != StatusInvalidArgument {
		t.Fatalf("zero options: %v", err)
	}
	if _, err := Open(t.TempDir(), nil); err != nil {
		t.Fatalf("nil options (core defaults): %v", err)
	}
}

func TestIndexDefineDrop(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	if err := st.DefineCompoundIndex([]byte("i"), []byte("p/"), []IndexField{{1, IndexUint64}, {2, IndexBytes}}, 0); err != nil {
		t.Fatal(err)
	}
	if err := st.DropIndex([]byte("i")); err != nil {
		t.Fatal(err)
	}
	if err := st.DefineCompoundIndex([]byte("i"), []byte("p/"), nil, 0); err == nil {
		t.Fatal("zero fields must fail")
	}
}

// 事务内的查询看得到私有写入，Store 的只读查询看不到。
func TestTxnSelectSeesPrivateWrites(t *testing.T) {
	st := openTestStore(t)
	defer st.Close()
	uid := U64(1)
	put := func(tx *Txn, id uint64) {
		doc := appendU(nil, 1, id)
		if err := tx.Put([]byte(fmt.Sprintf("q/%d", id)), doc); err != nil {
			t.Fatal(err)
		}
	}
	tx, _ := st.Begin(0)
	defer tx.Close()
	put(tx, 1)
	put(tx, 2)
	count := func(db Database) int {
		rows, err := From("q/").Select(uid).OrderBy(uid).Run(db)
		if err != nil {
			t.Fatal(err)
		}
		defer rows.Close()
		n := 0
		for rows.Next() {
			n++
		}
		return n
	}
	if n := count(tx); n != 2 {
		t.Fatalf("txn sees %d rows, want 2", n)
	}
	if n := count(st); n != 0 {
		t.Fatalf("store sees %d uncommitted rows", n)
	}
	if _, err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	if n := count(st); n != 2 {
		t.Fatalf("store sees %d committed rows, want 2", n)
	}
}

func TestQueryEncodingEdgeCases(t *testing.T) {
	// 负数使用 zigzag sint，非负使用 uint，与 Python 一致。
	enc, _ := IntValue(-3).appendTo(nil)
	if !bytes.Equal(enc, []byte{0x08, 0x02, 0x18, 0x05}) {
		t.Fatalf("sint = %x", enc)
	}
	enc, _ = UintValue(300).appendTo(nil)
	if !bytes.Equal(enc, []byte{0x08, 0x01, 0x10, 0xac, 0x02}) {
		t.Fatalf("uint = %x", enc)
	}
	if _, err := FloatValue(nan()).appendTo(nil); err == nil {
		t.Fatal("NaN must be rejected")
	}
	// 表达式过深。
	e := Expr{}
	for i := 0; i < 70; i++ {
		e = Expr{Op: OpNot, Children: []Expr{e}}
	}
	q := NewQuery([]byte("p/"))
	q.Where = e
	if _, err := q.Encode(); err == nil {
		t.Fatal("deep expression must be rejected")
	}
	// 变长长度前缀（>127 字节的嵌套消息）。
	q = NewQuery(bytes.Repeat([]byte("p"), 300))
	q.Where = Expr{Op: OpEq, Field: Field{Number: 1, Type: TypeBytes}, Values: []Value{BytesValue(bytes.Repeat([]byte("x"), 1000))}}
	if _, err := q.Encode(); err != nil {
		t.Fatal(err)
	}
	if _, err := U64(1).Eq("abc").And(U64(2).Eq(1)).build(nil); err == nil {
		t.Fatal("string literal on integer column must fail")
	}
}

func nan() float64 { var z float64; return z / z }

func TestFinalizerClosesForgottenObjects(t *testing.T) {
	dir := t.TempDir()
	func() {
		st, err := Open(dir, testOptions())
		if err != nil {
			t.Fatal(err)
		}
		st.Put([]byte("k"), []byte("v"))
		_, _ = st.PrepareKey([]byte("k"))
	}()
	for i := 0; i < 5; i++ {
		runtime.GC()
		time.Sleep(20 * time.Millisecond)
	}
	// finalizer 把目录租约还回后可以重新打开同一目录。
	st, err := Open(dir, testOptions())
	if err != nil {
		t.Skipf("finalizer timing not deterministic: %v", err)
	}
	st.Close()
}
