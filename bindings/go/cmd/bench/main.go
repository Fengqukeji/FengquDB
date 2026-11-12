// 命令 bench 与 tools/lang_conformance/bench_c.c 使用同样的数据与操作，
// 输出同样格式的 JSON 行：{"op":...,"ops":...,"ns_per_op":...,"sink":...}
//
// 用法: bench DIR [SECONDS] [-extra]
// 环境变量 FQDB_BENCH_SELECT_HEX：select_topn 的完整请求（conformance.json 里 topn_mage_level 的 request）。
package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"strconv"
	"time"

	"fengqudb"
)

const records = 32768

func makeKey(k []byte, id uint64) {
	k[0] = 'k'
	for i := 0; i < 8; i++ {
		k[1+i] = byte(id >> (56 - 8*i))
	}
}

func check(err error, what string) {
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s: %v\n", what, err)
		os.Exit(2)
	}
}

func report(op string, ops uint64, d time.Duration, sink uint64) {
	fmt.Printf("{\"op\":\"%s\",\"ops\":%d,\"ns_per_op\":%.1f,\"sink\":%d}\n", op, ops, float64(d.Nanoseconds())/float64(ops), sink)
}

// run 按 bench_c 的方式计时：每轮执行 batch 次操作后检查一次时钟，累计到 seconds 为止。
func run(op string, seconds float64, perRound uint64, round func() uint64) {
	var sink, ops uint64
	t0 := time.Now()
	var d time.Duration
	for {
		sink += round()
		ops += perRound
		d = time.Since(t0)
		if d.Seconds() >= seconds {
			break
		}
	}
	report(op, ops, d, sink)
}

func putVarint(b []byte, v uint64) []byte {
	for v >= 128 {
		b = append(b, byte(v)|128)
		v >>= 7
	}
	return append(b, byte(v))
}

func main() {
	args := os.Args[1:]
	extra := false
	var pos []string
	for _, a := range args {
		if a == "-extra" {
			extra = true
		} else {
			pos = append(pos, a)
		}
	}
	dir := "/tmp/fqdb-bench-go"
	seconds := 1.0
	if len(pos) > 0 {
		dir = pos[0]
	}
	if len(pos) > 1 {
		seconds, _ = strconv.ParseFloat(pos[1], 64)
	}
	// 预算取值与 bench_c.c 相同。
	db, err := fengqudb.Open(dir, &fengqudb.Options{Durability: fengqudb.DurabilityBuffered, Flags: fengqudb.FlagCreate,
		CacheBytes: 2 << 30, DeltaBytes: 16 << 20, WriterBytes: 32 << 20, TransactionBytes: 64 << 20,
		AllTransactionBytes: 256 << 20, PinnedBytes: 2 << 30, MaxSnapshots: 1024})
	check(err, "open")
	defer db.Close()

	value := make([]byte, 256)
	key := make([]byte, 9)
	for i := uint64(0); i < records; i++ {
		for j := range value {
			value[j] = byte(i & 0xff)
		}
		makeKey(key, i)
		check(db.Put(key, value), "put")
	}
	check(db.Sync(), "sync")

	// 1. 准备键 + 共享读取（零拷贝视图）
	{
		makeKey(key, 4242)
		pk, err := db.PrepareKey(key)
		check(err, "prepare")
		run("get_shared_prepared", seconds, 1000, func() (sink uint64) {
			for i := 0; i < 1000; i++ {
				sv, err := pk.GetShared()
				check(err, "get_shared")
				b := sv.Bytes()
				sink += uint64(b[len(b)-1])
				sv.Close()
			}
			return
		})
		pk.Close()
	}
	// 2. 按键共享读取（每次传键）
	{
		var id uint64
		run("get_shared_key", seconds, 1000, func() (sink uint64) {
			for i := 0; i < 1000; i++ {
				makeKey(key, (id*2654435761)%records)
				id++
				sv, err := db.GetShared(key)
				check(err, "get_shared")
				sink += uint64(sv.Bytes()[0])
				sv.Close()
			}
			return
		})
	}
	// 3. 复制读取（调用方拥有副本）
	{
		var id uint64
		run("get_copy", seconds, 1000, func() (sink uint64) {
			for i := 0; i < 1000; i++ {
				makeKey(key, (id*2654435761)%records)
				id++
				v, err := db.Get(key)
				check(err, "get")
				sink += uint64(v[0])
			}
			return
		})
	}
	// 4. 32 键准备读取器：每个操作 = 1 条记录
	{
		keys := make([][]byte, 32)
		for i := range keys {
			keys[i] = make([]byte, 9)
			makeKey(keys[i], uint64(i)*997)
		}
		rd, err := db.PrepareReader(keys)
		check(err, "reader_create")
		run("reader32_per_record", seconds, 200*32, func() (sink uint64) {
			for i := 0; i < 200; i++ {
				rows, err := rd.Read()
				check(err, "reader_read")
				for j := range rows {
					sink += uint64(rows[j].Value()[0])
				}
			}
			return
		})
		rd.Close()
	}
	// 5. 额外的对照（仅 -extra）：复用缓冲的复制读取
	if extra {
		var id uint64
		buf := make([]byte, 0, 512)
		run("get_copy_append", seconds, 1000, func() (sink uint64) {
			for i := 0; i < 1000; i++ {
				makeKey(key, (id*2654435761)%records)
				id++
				var err error
				buf, err = db.GetAppend(buf[:0], key)
				check(err, "get_append")
				sink += uint64(buf[0])
			}
			return
		})
	}
	// 6. 查询
	if h := os.Getenv("FQDB_BENCH_SELECT_HEX"); h != "" {
		for i := uint64(0); i < 64; i++ {
			buf := []byte{0x08}
			buf = putVarint(buf, i)
			cls := "warrior"
			if i%2 == 1 {
				cls = "mage"
			}
			buf = append(buf, 0x12, byte(len(cls)))
			buf = append(buf, cls...)
			buf = append(buf, 0x18)
			buf = putVarint(buf, i%20)
			check(db.Put([]byte(fmt.Sprintf("p/%d", i)), buf), "put p")
		}
		req, err := hex.DecodeString(h)
		check(err, "hex")
		var res fengqudb.Result
		run("select_topn", seconds, 200, func() (sink uint64) {
			for i := 0; i < 200; i++ {
				check(db.SelectRequest(req, &res), "select")
				sink += uint64(len(res.Raw()))
			}
			return
		})
		res.Close()
	}
}
