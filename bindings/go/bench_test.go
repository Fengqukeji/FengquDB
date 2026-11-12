package fengqudb

import (
	"os"
	"sync"
	"testing"
)

// 基准数据与 tools/lang_conformance/bench_c.c 相同：32768 条 256 B 记录，键 'k' + 8 字节大端序号。
const benchRecords = 32768

var (
	benchOnce  sync.Once
	benchDir   string
	benchStore *Store
)

func TestMain(m *testing.M) {
	code := m.Run()
	if benchStore != nil {
		benchStore.Close()
	}
	if benchDir != "" {
		os.RemoveAll(benchDir)
	}
	os.Exit(code)
}

func benchKey(k []byte, id uint64) {
	k[0] = 'k'
	for i := 0; i < 8; i++ {
		k[1+i] = byte(id >> (56 - 8*i))
	}
}

func benchDB(b *testing.B) *Store {
	benchOnce.Do(func() {
		dir, err := os.MkdirTemp("", "fqdb-go-bench")
		if err != nil {
			b.Fatal(err)
		}
		benchDir = dir
		st, err := Open(dir, testOptions())
		if err != nil {
			b.Fatal(err)
		}
		value := make([]byte, 256)
		key := make([]byte, 9)
		for i := uint64(0); i < benchRecords; i++ {
			for j := range value {
				value[j] = byte(i & 0xff)
			}
			benchKey(key, i)
			if err := st.Put(key, value); err != nil {
				b.Fatal(err)
			}
		}
		if err := st.Sync(); err != nil {
			b.Fatal(err)
		}
		benchStore = st
	})
	return benchStore
}

func BenchmarkGetSharedPrepared(b *testing.B) {
	db := benchDB(b)
	key := make([]byte, 9)
	benchKey(key, 4242)
	pk, err := db.PrepareKey(key)
	if err != nil {
		b.Fatal(err)
	}
	defer pk.Close()
	b.ReportAllocs()
	b.ResetTimer()
	var sink uint64
	for i := 0; i < b.N; i++ {
		sv, err := pk.GetShared()
		if err != nil {
			b.Fatal(err)
		}
		v := sv.Bytes()
		sink += uint64(v[len(v)-1])
		sv.Close()
	}
	_ = sink
}

func BenchmarkGetSharedKey(b *testing.B) {
	db := benchDB(b)
	key := make([]byte, 9)
	b.ReportAllocs()
	b.ResetTimer()
	var sink, id uint64
	for i := 0; i < b.N; i++ {
		benchKey(key, (id*2654435761)%benchRecords)
		id++
		sv, err := db.GetShared(key)
		if err != nil {
			b.Fatal(err)
		}
		sink += uint64(sv.Bytes()[0])
		sv.Close()
	}
	_ = sink
}

// GetCopy 每次返回新分配的副本：1 次分配是语义要求（调用方拥有副本）。
func BenchmarkGetCopy(b *testing.B) {
	db := benchDB(b)
	key := make([]byte, 9)
	b.ReportAllocs()
	b.ResetTimer()
	var sink, id uint64
	for i := 0; i < b.N; i++ {
		benchKey(key, (id*2654435761)%benchRecords)
		id++
		v, err := db.Get(key)
		if err != nil {
			b.Fatal(err)
		}
		sink += uint64(v[0])
	}
	_ = sink
}

// GetAppend 复用调用方缓冲：一次 cgo 往返，0 次分配。
func BenchmarkGetAppend(b *testing.B) {
	db := benchDB(b)
	key := make([]byte, 9)
	buf := make([]byte, 0, 512)
	b.ReportAllocs()
	b.ResetTimer()
	var sink, id uint64
	for i := 0; i < b.N; i++ {
		benchKey(key, (id*2654435761)%benchRecords)
		id++
		var err error
		buf, err = db.GetAppend(buf[:0], key)
		if err != nil {
			b.Fatal(err)
		}
		sink += uint64(buf[0])
	}
	_ = sink
}

// 每次操作 = 一次 32 键整批读取；ns/record 由 ns/op 除以 32 得到。
func BenchmarkReader32(b *testing.B) {
	db := benchDB(b)
	keys := make([][]byte, 32)
	for i := range keys {
		keys[i] = make([]byte, 9)
		benchKey(keys[i], uint64(i)*997)
	}
	rd, err := db.PrepareReader(keys)
	if err != nil {
		b.Fatal(err)
	}
	defer rd.Close()
	b.ReportAllocs()
	b.ResetTimer()
	var sink uint64
	for i := 0; i < b.N; i++ {
		rows, err := rd.Read()
		if err != nil {
			b.Fatal(err)
		}
		for j := range rows {
			sink += uint64(rows[j].Value()[0])
		}
	}
	_ = sink
}

func BenchmarkSelectTopN(b *testing.B) {
	v := loadVectors(b)
	var req []byte
	for _, c := range v.Cases {
		if c.ID == "topn_mage_level" {
			req = unhex(b, c.Request)
		}
	}
	st := conformanceStore(b, v)
	defer st.Close()
	var res Result
	defer res.Close()
	b.ReportAllocs()
	b.ResetTimer()
	var sink int
	for i := 0; i < b.N; i++ {
		if err := st.SelectRequest(req, &res); err != nil {
			b.Fatal(err)
		}
		sink += len(res.Raw())
	}
	_ = sink
}

// 解码 7 行结果：惰性解码直接在原生缓冲上进行，不应产生分配。
func BenchmarkSelectTopNDecode(b *testing.B) {
	v := loadVectors(b)
	var req []byte
	for _, c := range v.Cases {
		if c.ID == "topn_mage_level" {
			req = unhex(b, c.Request)
		}
	}
	st := conformanceStore(b, v)
	defer st.Close()
	var res Result
	defer res.Close()
	vals := make([]Value, 0, 8)
	b.ReportAllocs()
	b.ResetTimer()
	var sink uint64
	for i := 0; i < b.N; i++ {
		if err := st.SelectRequest(req, &res); err != nil {
			b.Fatal(err)
		}
		for res.Next() {
			vals, _ = res.AppendValues(vals[:0])
			sink += vals[0].U + uint64(len(res.Key()))
		}
	}
	_ = sink
}

// 最空的 cgo 往返（fqdb_abi_version）：用于估算每次 cgo 调用的固有开销。
func BenchmarkCgoCallBaseline(b *testing.B) {
	b.ReportAllocs()
	var s uint32
	for i := 0; i < b.N; i++ {
		s += ABIVersion()
	}
	_ = s
}
