# FengquDB Go 绑定

`package fengqudb`（module `fengqudb`）是 `include/fengqudb_store.h` C ABI 之上的 cgo 薄封装。
运行时除 libfengqudb 外没有第三方依赖，使用标准 cgo 工具链。

新用户先看[当前接入指南](../../docs/languages/go.md)。新库默认性能模式 50 ms / 1 MiB，普通成功表示内存生效；需要可靠水位时显式 Sync。下面测试、基准和 Docker 命令针对开发仓库，精简 SDK 不附测试数据或基准程序。

## 构建与测试

客户端 SDK 自动搜索相邻 include/lib。开发仓库或自定义布局可用环境变量提供头文件与库位置：

```sh
export CGO_CFLAGS=-I/lang/linux/include
export CGO_LDFLAGS='-L/lang/linux/lib -lfengqudb'
export LD_LIBRARY_PATH=/lang/linux/lib        # 运行时
export FQDB_CONFORMANCE=/lang/conformance.json # 一致性测试；未设置时这些测试被跳过
go vet ./... && go test ./...
go test -run XXX -bench . -benchmem .          # Go 基准（0 allocs/op 证明）
```

在宿主 Windows 上通过 Docker 运行：

```sh
docker run --rm --entrypoint sh \
  -v $REPO/build/lang:/lang -v $REPO/bindings/go:/src -w /src fqdb-lang-dev:1 -c \
  "export GOCACHE=/tmp/gocache GOPATH=/tmp/gopath GOFLAGS=-buildvcs=false; \
   CGO_CFLAGS=-I/lang/linux/include CGO_LDFLAGS='-L/lang/linux/lib -lfengqudb' \
   LD_LIBRARY_PATH=/lang/linux/lib FQDB_CONFORMANCE=/lang/conformance.json go test ./..."
```

与 C 基线同口径的基准程序（输出 `bench_c` 相同的 JSON 行；`-extra` 额外输出 `get_copy_append`）：

```sh
FQDB_BENCH_SELECT_HEX=<conformance.json 中 topn_mage_level 的 request> go run ./cmd/bench /tmp/bg 2 [-extra]
```

## 用法

```go
st, err := fengqudb.Open(dir, &fengqudb.Options{ /* 预算，全 0 会被核心库拒绝 */ }) // nil = 核心库默认值
defer st.Close()

st.Put(key, value); v, err := st.Get(key)          // Get 返回副本；未命中 errors.Is(err, fengqudb.ErrNotFound)

// 共享读：零拷贝视图
sv, err := st.GetShared(key); use(sv.Bytes()); sv.Close()
pk, _ := st.PrepareKey(key); sv, err = pk.GetShared() // 键只封送一次

// 批量读取器
rd, _ := st.PrepareReader(keys)                       // 1..4096 个键
rows, err := rd.Read()                                // []Row，指向原生内存
for i := range rows { if rows[i].Found() { use(rows[i].Value()) } }

// 事务
tx, _ := st.Begin(0); tx.Put(k, v); ver, err := tx.Commit(); tx.Close()

// 索引与查询
st.DefineCompoundIndex([]byte("class-level"), []byte("p/"), []fengqudb.IndexField{{2, fengqudb.IndexBytes}, {3, fengqudb.IndexUint64}}, fengqudb.IndexRequired)
uid, cls, level := fengqudb.U64(1), fengqudb.Text(2), fengqudb.U64(3)
rows, err := fengqudb.From("p/").Where(cls.Eq("mage").And(level.Between(5, 15))).
    Select(uid, level).OrderBy(level.Desc()).Limit(7).Run(st)   // st 或 *Txn
defer rows.Close()
for rows.Next() { v, _ := rows.Value(uid); ... }
```

类型化构建器等价于 Python `fengqudb_query.py`：`U64/I64/F64/Text/S64/F32/Fixed64/Sfixed64/Fixed32/Sfixed32/Boolean/Key/Nested`，
`Eq/Ne/Lt/Le/Gt/Ge/Between/In/IsNull/Exists/Contains/StartsWith/EndsWith/Like/Regex/IContains/ILike/Fuzzy`，
`.Any()`（repeated）、`.InSource(n)`（连接来源）、`Count/CountOf/CountDistinct/Sum/Avg/Min/Max`、`GroupBy/Having`、`Join`、
`And/Or/Not`。底层是 `Query` 结构（`Encode()` 与 Python `Query.encode` 逐字节一致）、`PreparedQuery`、`EncodeRequest`。
`Execute/Explain` 在 `*Store`（只读快照）或 `*Txn`（可见私有写入）上运行。分页用 `Query.Pages(db, fn)` / `Builder.Pages`，
或手动把 `Result.Cursor()` 复制到 `Query.After`，直到游标为空。

错误：非 OK 状态返回 `*fengqudb.Error{Code, Message}`，`errors.Is(err, fengqudb.ErrNotFound/ErrConflict/ErrLimit)`，
其他码用 `errors.As` 取 `Code`（`StatusXxx` 常量）。关闭后的对象返回 `ErrClosed`。

## 生命周期规则（必读）

| 对象 | 规则 |
| --- | --- |
| `Shared`（`GetShared`） | 值类型，持有一份固定内存（`pinned_bytes`）租约，**必须 `Close()`**。`Bytes()/Text()` 在 `Close` 前有效；之后写入、删除、整理甚至 `Store.Close` 都不影响它。`Close` 后不得再用先前取得的切片。切片只读语义：不要写入（容量等于长度，`append` 会复制）。拷贝 `Shared` 值后只对其中一个调用 `Close`。 |
| `SharedValue`（`Shared.Escape()`） | 堆对象，带 `runtime.SetFinalizer` 兜底；忘记 `Close` 时由 GC 归还。触发时点不确定，预算可能迟迟不释放，仅作兜底。 |
| `Reader.Read()` 的 `[]Row` | 行描述和字节只到该读取器下一次 `Read/Acquire/Release/Close`（含失败）为止。需要保留用 `Acquire()`（独立 `Results`，`Close` 前有效）。一个 `Reader` 串行使用，多个 goroutine 各建各的。 |
| `PreparedKey` | 可被多 goroutine 并发 `GetShared`；`Close` 不得与读取并发。 |
| `Result`（查询结果） | 持有原生响应缓冲，行和值在其上惰性解码；`Key()`、`Value.Bytes()`、`Cursor()` 等切片在 `Result.Close()`（或把同一个 `Result` 再次用于执行）前有效。可复用：`var r Result; p.ExecuteInto(db, &r)`。 |
| `Store/Txn/Reader/PreparedKey/Snapshot/Results` | 都有 `Close`（幂等）和 finalizer 兜底。`Close` 不得与同一对象上的其他调用并发。`Store.Close` 后其子句柄（`PreparedKey`、`Reader`、`Snapshot`、已取得的视图）仍可用，核心库按引用计数保持底层库。`Txn` 提交后仍需 `Close`。 |

cgo 指针规则：传给 C 的只有 Go 的 `[]byte` 数据指针（不含 Go 指针）和标量；`PrepareReader`/`PutMany`/`DefineCompoundIndex`
先把数据复制进 `C.malloc` 内存再调用，C 侧不会持有 Go 指针；C 返回的指针只用 `unsafe.Slice` 包装，不转 `uintptr`。
参数里的 `[]byte` 会被 cgo 视为逃逸：热路径请复用键缓冲区，不要每次调用新建局部数组。

## 验证记录

在 `fqdb-lang-dev:1`（AMD Ryzen 9 9950X3D，容器内 32 线程）里实际运行：

- `go vet ./...` 无输出。
- `go test ./...`：23 个测试全部通过；其中 5 个依赖 `conformance.json`：17 个用例的 `encoded`/`request` 字节与 `Query.Encode`/`EncodeRequest` 完全一致，
  16 个带 `result` 的用例（含 EXPLAIN 和原始请求入口）行/游标/统计/策略/索引名一致，分页 8 页 64 个值一致，
  类型化构建器对 16 个用例逐字节等于向量里的 `encoded`（聚合用例的向量排序按输出列号，构建器版本单独验证结果）。
- 同样的 `go test` 还在 `GODEBUG=cgocheck=1`、`GOEXPERIMENT=cgocheck2` 和 `-race` 下各跑一遍，均通过。

## 性能：相对 C 基线（`bench_c`）

同一容器、同一库、同一数据（32768 条 256 B 记录；`select_topn` 为 64 条 `p/` 文档），每项运行 2 秒，各跑 3 轮，表中取中位数（括号为 3 轮范围）。
C 为 `cc -O2 bench_c.c`，Go 为 `cmd/bench`。

| op | C (ns/op) | Go (ns/op) | Go/C |
| --- | --- | --- | --- |
| get_shared_prepared | 41.8 (41.7–41.8) | 135.4 (134.6–135.9) | 3.2× |
| get_shared_key | 375 (358–400) | 535 (518–565) | 1.4× |
| get_copy | 413 (384–423) | 643 (641–683) | 1.6× |
| reader32_per_record | 0.8 (0.7–0.8) | 2.1 (2.1–2.1) | 2.6× |
| select_topn | 11 532 (11 246–11 582) | 12 185 (12 101–12 190) | 1.06× |
| （附加）get_copy_append，复用缓冲 | — | 487 (470–528) | 对 C get_copy 1.2× |

`go test -bench -benchmem`（同一容器，`-benchtime=1s`，2 轮）：GetSharedPrepared 134–136 ns/op、GetSharedKey 498–505、GetAppend 479–484、
Reader32 66.4–66.8 ns/op（每次 32 条，约 2.1 ns/条）、SelectTopN 9.15 µs、SelectTopNDecode（逐行解码全部值）9.8 µs，**全部 0 B/op、0 allocs/op**。
唯一有分配的是 `GetCopy`：1 次 256 B/op，这是"调用方拥有副本"的语义所要求的。
空 cgo 往返（`BenchmarkCgoCallBaseline`，调用 `fqdb_abi_version`）是 26 ns/op。
`go test` 里的 SelectTopN 数据集是 conformance 的 96 条记录，与 `bench_c` 的数据不同，因此 µs 数不能直接对 C 基线；对比以 `cmd/bench` 为准。

### 原因分析（没有隐瞒的差距）

- **get_shared_prepared 3.2×（最差项）。** C 的 41.8 ns 里是读取和租约开销本身。Go 每次操作是两次 cgo 往返（`GetShared` 和 `Close`），
  实测每次空往返 26 ns，两次约 52 ns；41.8 + 52 ≈ 94 ns，实测 135 ns，**剩下约 40 ns 我没有剖析**
  （猜测与经 cgo 参数帧按值返回 48 字节结构体、Go 侧封装有关，未验证）。这个差距在单次调用的形态下无法消除；
  `GetShared` 与 `Close` 是两个独立的生命周期事件，不能合并成一次往返而不改变租约语义。
- **reader32_per_record 2.6×。** 一次 `Read()` 一次 cgo 往返，32 行共摊 26 ns 往返（约 0.8 ns/条），
  C 本身 0.75 ns/条，再加上 Go 侧遍历 32 行并读首字节的循环，合计 66 ns/批 ≈ 2.1 ns/条。绝对值很小，倍数大是因为 C 基线几乎只是数组遍历。
  批越大，往返成本摊得越薄。
- **get_shared_key / get_copy 1.4× / 1.6×。** C 的 360–410 ns 以核心库的查找和 16 MiB 级别缓存访问为主（键随机），
  Go 额外 ≈ 2 次 cgo 往返和结构体返回。`get_copy` 另有一次 256 B 的 Go 堆分配和 `memmove`；
  `GetAppend` 把复制放进一次 cgo 往返并复用缓冲，降到约 1.2×。
- **select_topn 1.06×。** 工作量在核心库里（11.5 µs），往返和 `Result` 复用开销可以忽略；响应不整体复制，直接在原生缓冲上解析。
- 以上没有 Go 侧调度或 GC 开销的体现：热路径无分配，基准期间不触发 GC。每次 cgo 调用的 26 ns 是 Go 运行时的下限（切换到系统栈、
  记录 `syscall` 状态），绑定层无法绕开。

## 已知限制与说明

- `Shared`/`Reader` 的"只读"靠文档约定：Go 没有只读切片类型（`Text()` 返回的字符串是不可变语义，更安全）。
- 为保证 0 分配，`GetShared` 返回值类型 `Shared`，因此没有 finalizer；需要兜底的场景用 `Escape()`（会分配一次）。
- 未覆盖的 C 接口：`put_submit/txn_submit` 异步提交、watch、history hold、compare_exchange/add_i64、patch、backup/restore/compact/verify、
  `project`/投影读取器、`hash_get_view`、`txn_cursor`/`cursor`、`get_many`/`batch_create`。
  它们不在规范 §1 的必须清单里，未实现。
- `Close` 与同一对象上其他调用并发是未定义行为（和 C 契约一致），Go 侧不加锁以免拖慢热路径。
- Windows/macOS 未验证，只在 Linux 容器里测试。
