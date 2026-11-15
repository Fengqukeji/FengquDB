# FengquDB Rust 绑定

`bindings/rust/` 是一个 cargo workspace，位于 C ABI（`include/fengqudb_store.h`）之上：

新用户先看[当前接入指南](../../docs/languages/rust.md)。新库默认性能模式 50 ms / 1 MiB；普通写成功表示内存生效，sync 是显式可靠屏障。下方测试/基准命令针对开发仓库；精简 SDK 不附测试和 dev-dependency。

| crate | 内容 |
| --- | --- |
| `fengqudb-sys` | 手写 `extern "C"` 声明与 `#[repr(C)]` 结构体，不依赖 bindgen/libclang。`build.rs` 负责链接。 |
| `fengqudb` | 安全封装：Store、事务、快照、共享读取、准备键、批量读取器、索引、底层 Query 与类型化构建器。运行时无第三方依赖，`serde_json` 只是 dev-dependency（读一致性向量）。 |

## 构建与链接

库目录由环境变量 `FQDB_LIB_DIR` 指定（里面要有 `libfengqudb.so` / `.dylib` / Windows 导入库）。未设置时，`build.rs`
依次尝试仓库内的 `build/lang/linux/lib`、`build/lib`，再退回系统默认搜索路径（`/usr/local/lib` 等）。
`FQDB_STATIC=1` 改为静态链接。构建脚本没有设置 rpath，运行时需要 `LD_LIBRARY_PATH`（或系统路径）能找到共享库。

```sh
FQDB_LIB_DIR=/path/to/lib LD_LIBRARY_PATH=/path/to/lib cargo build --release
```

## 在 fqdb-lang-dev:1 容器里测试与基准

```powershell
# 宿主 Windows（仓库根目录下的相对路径由脚本解析）
bindings\rust\scripts\run-tests.ps1          # cargo test --release + cargo clippy -D warnings
bindings\rust\scripts\run-bench.ps1 3 3      # 每项 3 秒、3 轮，C 基线与 Rust 交替运行
```

等价的原始命令：

```sh
docker run --rm --entrypoint sh -v $REPO/build/lang:/lang -v $REPO/bindings/rust:/src -w /src fqdb-lang-dev:1 \
  -c "FQDB_LIB_DIR=/lang/linux/lib LD_LIBRARY_PATH=/lang/linux/lib FQDB_CONFORMANCE=/lang/conformance.json cargo test --release"
```

`FQDB_CONFORMANCE` 指向 `build/lang/conformance.json`；未设置时回退到仓库相对路径，找不到则测试失败（不会静默跳过）。
镜像里没有 clippy 组件，`scripts/test.sh` 会在缺失时 `rustup component add clippy`（需要联网，容器是一次性的所以每次都装）。

## 用法

```rust
use fengqudb::{Options, Store, Durability};

let db = Store::open("/tmp/db", &Options { durability: Durability::Buffered, ..Options::default() })?;
db.put(b"player/42", b"payload")?;
assert_eq!(db.get(b"player/42")?.as_deref(), Some(&b"payload"[..]));   // 复制，Vec<u8>

// 共享（零拷贝）读取：SharedValue 拥有租约，Drop 时 fqdb_store_value_close
if let Some(v) = db.get_shared(b"player/42")? {
    consume(v.as_bytes());        // &[u8]，生命周期受 v 约束
}

// 热点读取：准备键（可多线程共享）与批量读取器（单线程使用）
let key = db.prepare_key(b"player/42")?;
let v = key.get_shared()?;
let mut reader = db.reader(&[b"player/42", b"player/43"])?;
let rows = reader.read()?;        // Rows<'_> 独占借用 reader
for row in rows.iter() { if row.found() { consume(row.value()); } }
// reader.read() 再次调用之前，rows 必须已不再使用，否则编译失败

// 事务
let mut txn = db.begin(0)?;
txn.put(b"a", b"1")?;
assert_eq!(txn.get(b"a")?.as_deref(), Some(&b"1"[..]));   // 读到自己的写入
txn.commit()?;
```

### 查询

```rust
use fengqudb::builder::*;
use fengqudb::query::JoinKind;

let (uid, cls, level, gold) = (uint64(1, 0), text(2, 0), uint64(3, 0), sint64(4, 0));
let result = from_("p/")
    .where_(cls.eq("mage") & level.between(5, 15) & !gold.lt(0))
    .select(&[&uid, &level])
    .order_by([level.desc(), uid.asc()])
    .limit(20)
    .count_total(true)
    .run(&db)?;                                   // Store：只读快照；Transaction：可见私有写入
for row in result.rows() {
    let row = row?;
    println!("{:?} {:?}", row.get(&uid)?, row.get(&level)?);   // 值在此刻才解码
}
let cursor = result.cursor()?;                    // 空表示没有更多页
```

* 比较用方法 `eq/ne/lt/le/gt/ge`，组合用 `&`、`|`、`!`（Rust 不能重载 `==`）。`in_`、`like`、`regex`、`icontains`、
  `between`、`is_null`、`exists`、`fuzzy`、`.any()`（repeated 字段）、`join(prefix, &left, &right, kind)` 与 Python 对应。
* 聚合：`count()`、`count_of`、`count_all_of(&col)`、`count_distinct`、`sum/avg/min/max`，配合 `group_by` / `having`。
  聚合出现在 `where_` 或 HAVING 引用了没有选择的聚合时，`build()` 返回错误（同 Python 的 ValueError）。
* 底层：`fengqudb::query::Query`（字段与 Python 的 `fengqudb_select.Query` 一一对应）、`Query::encode()`、
  `Query::request(explain)`；`Query::execute / explain / execute_pages`（按游标逐页，直到游标为空）。
* 响应 `QueryResult` 拥有原生 `fqdb_buffer`，`rows()`、`ResultRow::values()`、`access()` 都是在缓冲上按需解码的迭代器；
  需要逃逸时自行复制。`QueryResult` 释放时才调用 `fqdb_buffer_free`。

## 生命周期与线程规则

| 类型 | 规则 |
| --- | --- |
| `Store` | `Clone`（共享同一原生句柄）、`Send + Sync`。`close()` 是**逻辑**关闭：之后经由该 Store（及其克隆）的调用返回 `Error::Closed`。原生 `fqdb_store_close` 在最后一个克隆和所有子句柄释放时执行（头文件约定：子句柄保持底层库存活）。 |
| `SharedValue` | 拥有租约，`Send + Sync`。按头文件，写入、压缩、Store 关闭之后仍有效；它不持有 Store 引用。Drop 归还 `pinned_bytes`。 |
| `PreparedKey` | 持有 Store 引用；`Send + Sync`，可并发读取。 |
| `Reader` | 持有 Store 引用；`Send` 但不是 `Sync`。`read(&mut self) -> Rows<'_>`，借用检查保证行在下一次 `read/release` 之前失效（有 `compile_fail` 文档测试）。 |
| `Transaction<'s>` / `Snapshot<'s>` | 生命周期绑定 Store；`Send`，不是 `Sync`。Drop 关闭（未提交即回滚）。 |
| `QueryResult` / `Buffer` | 拥有原生缓冲，`Send + Sync`；行和值是借用它的视图。 |

`unsafe` 只出现在 `error.rs`、`buffer.rs`、`store.rs`、`shared.rs`（每处都有 `SAFETY` 注释），查询编码/解码是纯安全代码。

## 测试

`cargo test --release`（容器内）实际结果：

| 测试文件 | 用例数 | 内容 |
| --- | --- | --- |
| `tests/conformance.rs` | 7 | 17 个向量的底层 Query 与类型化构建器编码字节 == `encoded`/`request`；16 个用例在 dataset 上执行，行/值类型/游标/matched/complete/result_total/策略/索引名一致；分页 8 页拼接 == `paginate.values`；事务内查询可见私有写入；EXPLAIN；编码边界 |
| `tests/store.rs` | 13 | 读写/删除/未命中/空值、过期、事务（读自己的写、回滚、`put_many`、冲突状态码）、快照、关闭后报错、打开错误码、共享视图在写入与 Store 关闭后仍有效、`pinned_bytes` 归还、读取器与准备键在 Store 关闭后可用、8 线程各自读取器并发读 + 写线程、复合索引、Send/Sync 断言 |
| `tests/no_alloc.rs` | 1 | 计数分配器断言：准备键共享读、按键共享读、读取器 `read`、未命中、`select` + 惰性行解码，分配次数均为 0 |
| doc-tests | 3 | `compile_fail`：旧行在下一次 `read` 后使用、`Reader` 不是 `Sync`、`SharedValue` 视图不能比所有者活得久 |

`cargo clippy --release --all-targets -- -D warnings` 无警告。

## 基准（`examples/bench.rs`）

数据与操作与 `tools/lang_conformance/bench_c.c` 相同（32768 条 256 B 记录，键 `'k'`+8 字节大端序号，相同的预算与访问序列，
`select_topn` 使用 `topn_mage_level` 的请求字节和同样的 64 条 `p/` 文档）。输出同样的 JSON 行。
`--allocs` 统计 Rust 侧堆分配次数，`--extra` 额外输出 `get_copy_buffer`。

本机、同一容器、3 轮交替运行（每项 3 秒）的 ns/op，括号内是各轮的最小到最大：

| op | C 基线 | Rust | Rust / C | Rust 每操作堆分配 |
| --- | --- | --- | --- | --- |
| get_shared_prepared | 42.0 (41.7–42.5) | 42.0 (41.8–42.2) | 1.00 | 0 |
| get_shared_key | 394.9 (377.7–416.4) | 392.4 (375.6–406.9) | 0.99 | 0 |
| get_copy | 415.4 (401.0–425.1) | 422.5 (417.8–431.0) | 1.02 | 1 |
| reader32_per_record | 0.77 (0.7–0.8) | 0.9 (0.9–0.9) | 约 1.15 | 0 |
| select_topn | 11 403 (11 301–11 464) | 11 312 (11 280–11 355) | 0.99 | 0 |
| get_copy_buffer（附加项，无对应 C 行） | 与 get_copy 同路径 | 409.7 (403.0–413.6) | | 0 |

分析：

* **共享读与查询**与 C 持平（差异小于 C 自身多轮之间的波动，`get_shared_key` 在 C 一侧就有 377–416 的跨度）。
  Rust 侧只是一次 FFI 调用加栈上结构体，没有分配，也没有复制载荷。
* **get_copy 约慢 2%**：规范要求 `get` 返回 `Vec<u8>`，所以在原生复制之外多一次 Rust 堆分配和 256 B 复制，再释放原生缓冲。
  这部分成本被原生调用的 400 ns 掩盖，处于噪声范围。`get_buffer` 直接把原生缓冲交给调用方，没有额外分配，
  测得 410 ns，和 C 基线相同量级。
* **reader32_per_record 约慢 15%（绝对差约 0.1 ns/条）**：这是唯一明确落后的项。该项每条记录只读 1 个字节，
  成本已降到 1 ns 以下，Rust 版本的 `Row::value` 对每行多做 `found`、长度、空指针判断和 `[0]` 的边界检查，
  `Rows::iter` 也经过切片迭代器。C 版本直接读 `rows[j].value.data[0]`。没有堆分配，没有额外 FFI 调用。
  计时分辨率为 0.1 ns，所以这个倍数有 ±0.1 ns 的量化误差，按“约 1.15”报告。
* 单次运行之间有约 ±5% 的波动，表中倍数不应读作精确的 1.00/0.99。

## 已知限制

* 只封装了规范 §1 要求的入口：打开/关闭、put/delete、事务（含 `put_many`）、复制读、共享读（Store、准备键、快照）、
  批量读取器、复合索引、SELECT/EXPLAIN（Store 与 Transaction）。未封装：`patch`、`compare_exchange`、`add_i64`、
  `get_many`、`reader_acquire`、投影读取器、游标、watch、备份/维护、事务取消与保存点。
* `Store::close()` 是逻辑关闭（见上表），不会让仍被子句柄使用的原生句柄提前释放，因此关闭后立即用同一路径重新
  `open` 要先释放所有子句柄，否则可能得到 `LOCKED` 一类错误。
* 在 Linux 容器里验证；Windows/macOS 只写了 `build.rs` 的链接逻辑，没有实际构建。
* `Options` 的 `abi_version` 固定为 1（与 `bench_c.c` 相同）。
