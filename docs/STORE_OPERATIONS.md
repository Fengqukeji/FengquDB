# Store 原子与批量接口（开发版）

普通单键put/delete/patch在无扩展特性时使用轻量准备路径；有索引或约束时自动保留事务校验。显式事务的原始快照和冲突语义不变，提交后的值缓存及性能验证见[提交路径优化](STORE_HOT_WRITE_REVIEW_20261109.md)。

默认性能模式支持全部同步写入与原子操作，内存生效后确认；关键写入传force_reliable／forceReliable，或配置Reliable模式。批量与多键事务整笔选择保证级别，详见[写入策略](STORE_WRITE_POLICY.md)。

C、C++、C#、Java、Python 使用同一引擎，新增接口需要本批源码构建的库。目录与 Unity 接入见 [目录规范](STORE_LAYOUT.md)。

本批实测、审查和未完成范围见 [验证记录](STORE_OPERATIONS_REVIEW_20261106.md)。

## 原子条件操作

```cpp
auto db = fengqudb::Store::open_at("/application/save/db", "main");
db.put_if_absent("player/42", "initial bytes");
auto value = db.get("player/42");
if (value) {
    auto changed = db.compare_exchange("player/42", value->version(), "updated bytes");
    // matched == 0: another writer changed/deleted/expired the record.
}
db.put_if_changed("settings", "same bytes");
auto balance = db.add_i64("tokens", 1, 0, 100);
```

条件在写队列中基于最新候选视图检查，同组前序写入对后续条件可见，不依赖业务 OCC 重试。沿用 SYNC 合组刷盘或 BUFFERED 语义；一组不是跨请求的原子事务。

- CAS 期望版本 0 表示逻辑不存在，包括删除和 TTL 到期；正版本必须匹配存活记录。
- 条件不满足仍返回 OK，`matched=changed=0`，不产生新版本。
- `put_if_changed` 比较值字节和 TTL，两者相同才跳过写入。
- `delete_if_version` 匹配且存在时删除；期望 0 且不存在时匹配但不写。
- `add_i64` 使用恰好 8 字节小端有符号整数，缺失从 0 开始，保留现有 TTL。类型错误为 INVALID_ARGUMENT，溢出或超出闭区间为 CONSTRAINT。已有值加 0 不写，缺失加 0 创建计数器。
- `version` 是请求完成时的提交视图版本；`observed_version` 是观察到的存储版本，可能包括到期记录/墓碑。值句柄的 version/TTL 与该值严格属于同一视图，适合随后 CAS。
- 错误包括 COMMIT_UNKNOWN 时输出清零；不能把未知提交当成安全重试。期限覆盖排队，不能硬中断已开始的系统刷盘。

多键业务原子性仍用显式事务。字段索引与约束照常生效。

## 批量与复用

```cpp
db.put_many({{"a", "one"}, {"b", "two"}});
auto values = db.get_many({"a", "missing", "a"});
auto key = db.prepare_key("a");
char buffer[64]; uint64_t required = 0;
key.get_into(buffer, sizeof(buffer), required);
auto batch = db.prepare_batch({{"template", "large immutable bytes"}});
auto tx = db.transaction();
tx.put_batch(batch);
tx.patch("template", 0, 5, "small");
auto result = tx.commit();
```

每次最多 4096 项，输入/保留结果数据最多 64 MiB，仍受事务和固定内存预算限制。批量读捕获同一视图和 TTL 时间；事务读取包括私有写入并登记依赖，私有值版本为 0。保留顺序、重复键、空值和未命中。原生切片有效到 results_close；C++ StoreValues 用 RAII 管理。

C#/Java/Python 的 GetMany/getMany/get_many 返回值数组，未命中为 null/None；GetEntries/getEntries/get_entries 显式请求元数据。原生端提供连续结果元数据，减少逐行语言边界调用；返回托管 bytes 仍会复制值。

批量写先检查整批参数；资源/暂存失败后须 rollback/close，不能提交前缀。Store 便利方法以单事务提交；事务内的方法只暂存。重复键最后一项生效。

准备键绑定原 Store，复用键字节和仅对精确不可变视图有效的弱缓存。GetInto/getInto/get_into 使用调用者缓冲；过小返回 BUFFER_TOO_SMALL、设置 required 且不部分复制。支持并发读，原生关闭不能与调用竞态，托管绑定有生命周期租约。

准备批次复制输入一次，预排序、合并重复键、预计算 CRC；应用时共享不可变值，仍逐键暂存并检查版本，不是 O(1) 接管。准备数据计 pin 预算，事务保守计保留输入预算；保存点回滚恢复保留链，事务结束释放计费。关闭批次句柄不会使已采用它的事务悬空。

共享准备值首次修改时隔离成完整私有缓冲，之后沿用 0.3.1 减法裁剪/增量补丁策略，不原地改变其他事务使用的准备数据。只调用一次的小输入未必受益，须按实际模式测量。
