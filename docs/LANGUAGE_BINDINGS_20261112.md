# 语言绑定与高级查询覆盖（2026-11-12）

所有语言都是同一个 C ABI（`include/fengqudb_store.h`）之上的封装，高级查询走 `fqdb_store_select`（操作 53 查询 / 54 EXPLAIN）。

| 语言 | 目录 | 高级查询 | 共享读（零拷贝） | 验证 |
| --- | --- | --- | --- | --- |
| C / C++ | `include/` | `fengqudb_select.hpp`、`fengqudb_query.hpp` | `fqdb_store_get_shared`、准备读取器 | 原生 ctest 32/32（排除已有失败的 `diagnostics`） |
| Python | `bindings/python` | `fengqudb_select.py`、`fengqudb_query.py` | `get_shared`、`prepare_reader` | unittest 2/2 |
| C# | `bindings/csharp` | `StoreSelect.cs`、`StoreQueryBuilder.cs` | `PrepareReader`、`ReadShared` | QueryTests 1/1 |
| Java | `bindings/java` | `Select.java`、`Q.java` | `getShared`、`prepareReader` | `SelectSmoke` 通过（需先由 Python 造夹具） |
| Go | `bindings/go` | `query.go`、`builder.go` | `GetShared`、`PrepareKey`、`PrepareReader` | go test 23/23，`go vet`、`-race`、cgocheck 通过 |
| Rust | `bindings/rust` | `query.rs`、`builder.rs` | `get_shared`、`PreparedKey`、`Reader` | cargo test 24/24，clippy `-D warnings` 无警告 |
| Node.js / TypeScript | `bindings/node` | `query.ts`、`builder.ts` | `getShared`、`withShared`、`prepareKey`、`prepareReader` | node:test 67/67 |

Go、Rust、Node 三个绑定用同一份一致性向量 `conformance.json`（17 个查询用例、96 条记录、3 个索引）验证：
查询编码字节与 Python 参考实现逐字节相同，结果行、游标、统计和策略相同，分页拼接相同。
向量由 `tools/lang_conformance/gen_vectors.py` 在同一核心库上生成。一键验证：`tools/lang_conformance/run_all.sh`（在 `fqdb-lang-dev:1` 容器内全新构建核心并跑完三个绑定）。
规范见 `tools/lang_conformance/BINDING_SPEC.md`。

## 性能（相对 C 的开销，Linux 容器 2 vCPU，每项 2 秒，C 与各语言交替测量）

| 操作 | C | Rust | Go | Node |
| --- | ---: | ---: | ---: | ---: |
| 准备键共享读（256 B） | 42 ns | 42 ns | 136 ns（3.2×） | 1085 ns（约 26×） |
| 按键共享读 | 360–400 ns | 392 ns | 540 ns（1.4×） | 1900 ns |
| 复制读 | 370–415 ns | 410–425 ns | 610–630 ns（1.6×） | 1110 ns（3×） |
| 32 键读取器，每条记录 | 0.7 ns | 0.8–0.9 ns | 2.1 ns | 833 ns（逐行建视图）；`copyValue` 33 ns |
| 查询 top-N | 11.3–11.5 µs | 11.4–12.0 µs | 12.2–12.4 µs（1.08×） | 13.9 µs（1.2×） |

- Rust 与 C 持平。热路径没有 Rust 堆分配（有计数分配器的测试）。
- Go 的差距来自每次 cgo 往返约 26 ns；共享读需要「取」和「释放」两次往返。热路径 0 allocs/op。约 40 ns 的剩余部分没有剖析。
- Node 创建指向原生内存的 `ArrayBuffer`/`Uint8Array` 视图约 1 µs，这是 V8 的固有成本，不是封装可以消除的。
  对 256 B 这种小值，`getInto(复用缓冲)` 只要 59 ns，反而比零拷贝视图快；热点路径请用 `getInto`、`copyValue` 或 `peekByte`，
  零拷贝视图更适合大值。查询路径在三个新语言上都只比 C 慢 1.08–1.2 倍，因为时间主要花在核心库。
- 新绑定没有声称「没有性能衰减」：Go 与 Node 的小值读取存在可测的调用边界成本，已如实记录，并在各自 README 说明如何避开。

## 范围与未验证项

- 新绑定只在 Linux 容器里构建和测试。Windows（MSVC）和 macOS 的构建脚本存在但未验证；Node 在 Electron（V8 沙箱）下不可用。
- 新绑定没有封装全部 C 接口。未覆盖：patch、compare-exchange、原子计数、watch、备份与整理、历史保留、`get_many`、投影读取器、异步提交。各 README 列出了清单。
- `durability: buffered` 下关闭 Store 不会 sync（Node 实测重开后数据丢失），需要先调用 `sync()`。
- Java 绑定的 `SelectSmoke` 依赖 Python 先写入夹具，单独运行会断言失败。
- `diagnostics` ctest 在原版代码上同样失败（`diagnostics_tests.cpp` 第 96 行），与这些改动无关。
- 未做 CI 集成：`.github/workflows/build.yml` 尚未加入 Go、Rust、Node 的任务。
