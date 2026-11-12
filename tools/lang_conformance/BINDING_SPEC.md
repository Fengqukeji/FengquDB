# 语言绑定规范（Go / Rust / Node.js·TypeScript，以及对现有语言的核对基准）

所有绑定都是 C ABI（`include/fengqudb_store.h`、`include/fengqudb.h`）之上的薄封装。不得修改核心源码。
本规范与 `build/lang/conformance.json`（由 `tools/lang_conformance/gen_vectors.py` 在同一核心库上生成）配套。

## 1. 必须覆盖的能力

| 能力 | C 入口 | 说明 |
| --- | --- | --- |
| 打开/关闭 | `fqdb_store_open`、`fqdb_store_close` | 选项结构体全 0 会被拒绝；参考 `bench_c.c` 的取值 |
| 写 | `fqdb_store_put/delete`、事务 `txn_begin/put/delete/commit/close` | 事务内可 `put_many` |
| 复制读 | `fqdb_store_get` + `fqdb_buffer_free` | 调用方拥有副本 |
| **共享读（零拷贝）** | `fqdb_store_get_shared`、`key_prepare` + `key_get_shared`、`snapshot_get_shared` | 见 §2 |
| **批量读取器** | `fqdb_store_reader_create/read/release/close` | 见 §2 |
| 索引 | `fqdb_store_index_define_compound`、`index_drop` | 字段类型见头文件 |
| **高级查询** | `fqdb_store_select`（操作 53 查询 / 54 EXPLAIN） | 见 §3，必须全部支持 |
| 错误 | `fqdb_status`、`fqdb_status_message` | 非 OK 映射为语言原生错误，保留数值码 |

## 2. 性能硬约束：共享读，不复制载荷

用户要求每个语言都不能因跨语言调用产生可避免的性能衰减，沿用 `docs/STORE_SHARED_READ.md` 的方案：

1. 共享读返回指向原生内存的只读视图，**不复制值字节**；视图附着一个租约（`fqdb_store_value*`），
   关闭租约才归还固定内存预算（`pinned_bytes`）。视图生命周期必须由语言机制保证安全，
   不得出现悬垂指针（见各语言要求）。
2. 热点读使用 **准备键** 与 **批量读取器**：键只封送一次，之后每次调用不再分配、不再封送键。
3. 查询结果（`fqdb_buffer`）不得先整体复制到语言堆再解析：直接在原生缓冲上解析，
   行和值按需惰性解码；需要逃逸时再复制。解析完成或不再需要时立即 `fqdb_buffer_free`。
4. 热路径不得有每次调用的堆分配（Go/Rust 尤其注意；Node 需避免每次创建新 Buffer 之外的临时对象）。
5. 必须提供与 `tools/lang_conformance/bench_c.c` **同数据同操作**的基准，输出同样的 JSON 行格式
   （`op` 取值：`get_shared_prepared`、`get_shared_key`、`get_copy`、`reader32_per_record`、`select_topn`），
   并报告相对 C 基线的倍数。C 基线（Linux 容器，本机）：
   `get_shared_prepared 41.9 ns`、`get_shared_key 360 ns`、`get_copy 370 ns`、`select_topn 11.3 µs`。
   做不到接近 C 的项必须解释原因（例如 JS 调用边界固有成本），**不得隐瞒或只挑好数字**。

## 3. 查询模型（必须与 `bindings/python/fengqudb_select.py` 逐字节一致）

- 请求外层：`fqdb_store_select` 的 `request` 是 `_feature_request(operation, scope, key, value)`：
  `0x08 varint(op)`、字段 2/3/4 为 bytes（scope、key、value=Query 编码）、
  `0x29 sfixed64(argument)`、`0x31 double(score)`、`0x39 fixed64(limit=256)`、`0x40 varint(flags=0)`。
  操作 53 = SELECT，54 = EXPLAIN。
- `Query/Expr/Field/Column/Order/Join` 的字段号与编码规则见 `fengqudb_select.py`（权威参考）。
  操作符 `Op`、聚合 `Aggregate`、连接类型 `JoinKind` 的数值必须一致。
- 值编码 `QueryValue`：null/uint/sint(zigzag)/double/bytes/bool。
- `Field.repeated=true`（`.any()`）编码为 field 4=1；只能用于主源 WHERE 叶子条件。
- 响应：`fqdb_store_select` 返回的缓冲**外层还有一层**命令响应（`_feature_result`：字段 1 = value bytes、2 = count、
  3 = changed、4 = 行、5 = 游标），真正的查询结果在字段 1 的 bytes 里（Python `PreparedQuery._run` 取 `response.value`），
  必须先解包再解码。内层：字段 1 = 行（key=1, 值=2 重复）；2 = 访问计划；3 = 统计；4 = 续页游标。解码规则见
  `Result._decode`，行内每个值是 `QueryValue`。
- 必须提供：底层 `Query` 结构 + 编码，**以及**类型化构建器（等价于 `fengqudb_query.py` 的 `from_().where().select().order_by().limit()`、
  `count/sum/avg/min/max`、`group_by/having`、`in_/like/between/is_null`、`.any()`、连接）。
- 分页：用返回的 `cursor` 设置 `after` 继续，直到游标为空。

## 4. 一致性测试（必须全部通过）

`build/lang/conformance.json`：
- `dataset.records`（key/value 为 hex）与 `dataset.indexes`：测试先写入并建索引。
- 每个 `cases[i]`：`spec`（语言无关的查询描述）、`encoded`（Query 编码 hex）、`request`（含外层的完整请求 hex）、
  `result`（期望行、游标、统计、策略与索引名）。
- 要求：1) 用你的 `Query` 模型从 `spec` 构造并编码，**字节与 `encoded`/`request` 完全相同**；
  2) 用你的绑定在同一数据集上执行，结果行（key 与每个值的类型和值）与 `result` 相同；
  3) `paginate` 用例的所有页按序拼接后等于 `paginate.values`。
- 另需覆盖：未命中、空值、过期、事务内读自己的写、关闭后使用报错、并发读（多线程/协程各自读取器）、
  共享视图在写入/关闭 Store 之后仍有效（按 `STORE_SHARED_READ.md`）。

## 5. 目录与交付

每个绑定放在 `bindings/<lang>/`，自带 README（安装、构建、用法、生命周期规则、已知限制）、
测试脚本和基准脚本，**可在 `fqdb-lang-dev:1` 镜像里运行**（见 §6）。
不要修改 `bindings/` 下其它语言目录、`src/`、`include/`、根 `CMakeLists.txt`；需要的改动写进你的汇报，由主任务统一集成。
不要提交（commit）、不要切换分支。

## 6. 运行环境

- 宿主 Windows，无 Go/Rust/JDK/gcc。统一使用 Docker 镜像 `fqdb-lang-dev:1`（Ubuntu 26.04，含 cc/cmake/python3、
  Go 1.23、Rust 稳定版、Node 24；`/usr/local/include/node` 有 N-API 头文件）。
- Linux 核心库和头文件已编好：`$REPO/build/lang/linux/lib/libfengqudb.so.2`、
  `$REPO/build/lang/linux/include/`。运行时 `LD_LIBRARY_PATH=/lang/linux/lib`。
- 典型命令：
  `docker run --rm --entrypoint sh -v $REPO/build/lang:/lang -v $REPO/bindings/<lang>:/src fqdb-lang-dev:1 -c "..."`
  （源码目录可写挂载；构建产物放 `/tmp`，不要写进仓库的 `build/` 之外位置）。
- 容器可联网（可用 `go mod`、`cargo`、`npm` 拉依赖），但**运行时不引入第三方依赖**；测试依赖允许（如 Rust 的 serde_json）。
- 本规范里的 C 基线数字来自同一台机器、同一镜像；对比时在同样条件下跑 `bench_c`。
