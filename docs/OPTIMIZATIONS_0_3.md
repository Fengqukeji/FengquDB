# 0.3.0：减少工作量、共享字节与增量操作

> 历史开发记录：下文旧单文件 Database 的代码与接口已删除，项目尚未发布。当前接入使用 [Store API](STORE_API.md)。


九个优化方向已进入本版。常规路径保持文件格式1/2与C ABI版本1；增量日志使用显式开启的格式3。本文说明最终实现及接口边界，性能结果见 PERFORMANCE_0_3.md。

| 方向 | 实现 | 使用方式与边界 |
| --- | --- | --- |
| 前提未变则跳过验证 | 开始版本等于当前候选版本时，跳过整轮逐键冲突检查 | 自动生效；候选版本包括同一提交组中前面已接受的事务。版本变化时仍完整验证有关键。 |
| 查找结果复用 | 事务首次读取同时取得值与版本，后续读取复用缓存；读写交集只验证一次 | 自动生效；缓存指向被事务基准快照固定的不可变值，自身暂存写优先。 |
| 小事务短路径 | 读集和写集各以内嵌有序数组保存最多4个键，超出后迁移到树映射 | 自动生效；迁移先构造私有候选，失败不破坏原容器，避免大批随机插入退化成数组搬移。 |
| 增量长度与校验复用 | 暂存操作增量维护精确wire长度，提交不再反复统计；大值缓存IEEE CRC32并组合校验 | 普通路径自动生效；长度变更、同键覆盖和删除均调整差额。缓存仅针对不可变值，4 KiB及以上使用缓存CRC组合。 |
| 热键与准备批次 | PreparedKey缓存准确发布状态的查询结果；PreparedBatch复用不可变写集、wire元数据和整批CRC | 显式使用；with_value/with/WithValue生成新批次并共享未变化值，仍需复制键与重建部分元数据，不是O(1)操作。 |
| 日志与内存共享字节 | wire元数据引用已拥有的键和值，直接汇集到写入缓冲或分段写出 | 不再构造另一份完整连续编码载荷；索引、历史及日志I/O复用值存储。调用方传入的普通切片仍须复制，以取得所有权；操作系统内部复制也仍存在。 |
| 按批保护读取生命周期 | read_session/ReadSession捕获一次快照；Python并发句柄使用调用参数所有权，Java使用读写生命周期锁 | 会话始终读同一版本。Python关闭等待在途参数释放；其他解释器及free-threaded构建回退到显式计数租约。C#沿用SafeHandle保护。未引入RCU或未经验证的裸指针回收。 |
| 原生语义与片段修改 | 原生有序整数增量、仅变化时写入、带基准版本检查的二进制splice；可选增量日志 | 增量指令当前是单键独立事务，不可用两个调用替代转账等多记录事务。splice操作字节，不自动理解Protobuf业务字段。 |
| 派生结果按依赖变化重建 | C++/C#/Java/Python DerivedCache，以同一快照的逐键版本判断依赖 | 无关键更新不重建；缺失键、删除版本和数据库实例身份都参与失效。计算抛异常保留旧缓存状态，下一次仍会重试。 |

## C++17例子

```cpp
fengqudb::Options options;
options.delta_log = true; // 只在创建新文件时选择格式3
fengqudb::Database db("new-database.fdb", options);

auto hot = db.prepare_key("player/42");
auto session = db.read_session();
auto borrowed = session.get_borrowed(hot);
// borrowed只在session的同一个底层快照仍有所有者时有效。

fqdb_key_value rows[] = {{{"item/1", 6}, {"value", 5}}};
fengqudb::PreparedBatch batch(rows, 1);
auto changed = batch.with("item/1", "new");
auto tx = db.begin();
tx.put_batch(changed);
tx.commit();

db.add_i64("visit/count", 1);
db.put_if_changed("document/title", "hello");
```

PreparedBatch可复制、共享和并发使用；它不绑定某个数据库，事务会保留它的原生数据。空写集直接采用准备批次；存在其他暂存写入时按普通逐条暂存合并。继续put/delete/patch会先将准备数据转换成事务私有写集。批次生成及修改有一次性成本，主要适合重复使用。

PreparedKey是数据库绑定的所有者，数据库句柄关闭后仍可读，并继续占用源数据库文件锁，直到键句柄也释放。缓存只有在准确的不可变状态相同时命中，任何提交都会使最新状态缓存失效；它不会把旧节点冒充最新值，也不强行固定旧快照。跨数据库快照使用键句柄返回INVALID_ARGUMENT。多个线程共用同一热键句柄会竞争其缓存互斥，是否更快应按负载衡量。

## Python例子

```python
from fengqudb import Database, DerivedCache

with Database("new.fdb", library=LIBRARY, delta_log=True) as db:
    with db.prepare_batch([(b"document/content", b"abcdef")]) as batch:
        with db.transaction() as tx:
            tx.put_batch(batch)
    with db.transaction() as tx:
        tx.patch(b"document/content", 2, 1, b"Z",
                 expected_version=db.key_version(b"document/content"))
    with db.prepare_key(b"document/content") as hot:
        data = hot.get()
    changed, version, data = db.get_if_changed(b"document/content")
    changed, version, data = db.get_if_changed(b"document/content", version)
    # 未变化：changed=False，没有复制返回值。
    cache = DerivedCache([b"document/content", b"embedding/model-version"])
    result = cache.get(db, lambda snapshot: build_owned_result(snapshot))
```

CPython的常规GIL构建利用ctypes调用参数引用保持原生句柄存活，避免每次读取操作共享计数锁；关闭分离公开句柄、阻止新调用，并等待最后一个参数所有者释放。非CPython和free-threaded构建采用计数租约，避免依赖延迟终结的时序。测试在CPython强制执行过回退路径，未声称已运行PyPy或free-threaded发行版。

## C# / Java

C#：PreparedBatch(rows)、WithValue、Transaction.PutBatch/Patch、Database.PrepareKey/ReadSession/AddI64/PutIfChanged/KeyVersion、DerivedCache<T>.Get。

Java：Database.PreparedBatch(keys,values)、withValue、Transaction.putBatch/patch、Database.prepareKey/readSession/addI64/putIfChanged/keyVersion、Database.DerivedCache<T>.get。数据库和快照的只读调用允许共享生命周期读锁，关闭取得写锁等待在途调用；事务及游标继续串行使用。

新接口要求0.3.0原生库；旧C ABI入口保留。所有语言的应用输入、可写返回缓冲均不得在原生调用期间被另一个线程修改。

## 格式3与增量恢复

默认仍创建格式1文件。C/C++通过options.reserved中的FQDB_OPTION_DELTA_LOG或Options::delta_log，Python通过delta_log，C#通过deltaLog，Java通过五参数构造的deltaLog显式选择新文件格式3。开启标志打开已有格式1/2文件返回UNSUPPORTED，不静默升级。格式3文件可用默认选项重新打开，格式由文件头决定。

格式3文件头仍为32字节：format=3、baseline=0、能力字段=2，CRC规则不变。普通事务仍使用现有帧和PUT/DELETE；PATCH操作kind=3，key字段2，插入字节字段3，base_version字段4，offset字段5，remove_bytes字段6。offset/remove省略时按proto3默认0解释；基准必须对应当前已恢复键的修改版本。完整CRC正确但基准、范围或链深度错误仍报CORRUPT，不修剪完整记录。

满足条件时才写增量：该键在事务中尚未暂存其他修改、基准存在、增量加元数据明显小于整值，并且连续增量少于8次。第9次写全量并重置深度；同一事务反复修改同键、后续put/delete覆盖等情况回退为完整最终操作。恢复和内存索引始终保存完整最新值，因此普通查询没有增量链读取成本；代价是patch仍需构造完整私有新值，不能把日志缩小比例当成CPU或内存下降比例。

历史同时维护实际编码载荷与完整物化载荷预算，任一超过max_history_payload_bytes都裁剪旧批次；公开retained_payload_bytes仍报告实际编码量。这样不会因为PATCH日志很小而保留大量完整旧值。现有游标、快照和调用方持有的数据仍可能超出引擎历史预算。

检查点继续导出格式2，含物化完整值，可由旧版本打开。旧库拒绝格式3；目录中保留0.2.3库和SDK。片段暂存先准备元数据，写集暂存失败时移除新元数据；故障注入覆盖失败后重试，避免失败调用留下错误的日志描述。

格式3部分记录截断、完整增量恢复、基准错误、模拟部分写/同步失败、官方protoc省略零字段和旧库拒绝均有直接验证。

## 语义和使用约束

- add_i64使用8字节小端有符号整数，缺失从0开始；长度错误及溢出不改数据，不推进版本。它在写者中基于最新状态执行，避免客户端读改写的冲突重试。COMMIT_UNKNOWN仍需重开核对，不能盲目重试非幂等加数。
- put_if_changed在写者中比较最新值。相同则不产生日志、不刷盘、不推进版本、不生成变更事件；普通put即使值相同仍保留原语义。它不把缓冲写提升成可靠写。
- get_version/key_version对已删除键返回删除版本，对从未出现的键返回0。get_if_changed的UINT64_MAX是强制读取哨兵，即使键版本本身达到UINT64_MAX也强制读取；NOT_MODIFIED=18不分配输出值。
- DerivedCache是进程内辅助缓存，不是持久化派生索引。必须完整声明依赖，包括模型及预处理配置等；builder使用提供的快照，返回自己拥有且按只读方式使用的结果，不递归进入同一个缓存。无关字段若放在同一个值里仍会推进该键版本，需要业务将独立变化的数据拆成不同键。
- 原生C/C++对象的销毁不得与对同一包装对象的调用竞争。已经创建的独立读取会话可在数据库句柄关闭后继续读。所有权固定的视图和借用视图规则不变。
- 这些优化没有提供容量受控的全库缓存、磁盘分页索引或超内存数据集支持；旧版本、准备批次和派生结果也占内存，调用方应及时释放。
