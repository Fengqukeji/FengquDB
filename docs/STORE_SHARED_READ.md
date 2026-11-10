# 外部接口共享读取

C/C++、C#、Python、Java 共用同一个原生引擎。普通 `Get/get` 返回调用方拥有的副本；要省掉值字节的复制，请显式选择共享接口。共享的是完整、不可变的序列化字节，业务模型的解析仍由业务序列化器完成。写入、删除、补丁、缓存淘汰和整理不会改变已持有的旧数据。

共享读取使用 `pinned_bytes`（C# 为 `PinnedBytes`）预算，默认 128 MiB，包含保守的元数据和载荷计费，共享对象可能重复计费。这与默认 64 MiB 的普通读取缓存分别计费，均按需占用。`cache_bytes=0` 也允许显式持有共享结果。超出固定预算返回 `LIMIT`；普通复制 `Get/get` 在临时共享租约无法计费时退回原复制路径，保持原有行为。

## C++17

原有 `db.get(key)` / `get_many(keys)` 已返回独立持有的只读共享值，值对象存活期间可以使用 `string_view`。本轮新增准备读取器，键复制一次；每次读取检查当前发布视图和 TTL，未变化时复用值和整批结果描述，不重新分配、查找或复制载荷。

```cpp
auto reader = db.prepare_reader(std::vector<std::string_view>{"player/42", "player/43"});
auto rows = reader.read();
if (rows[0].value) consume(*rows[0].value);
// 需要跨越下一次读取继续使用时，取得独立所有权：
auto held = reader.acquire();
reader.release(); // 释放当前结果，保留准备好的键
```

`read()` 的行描述和字节只保证有效到此读取器下一次 `read/acquire/release`（包含失败）或析构。不要跨越这个边界保留 `string_view`。`acquire()` 返回的 `StoreValues` 则独立有效，直到它析构。一个读取器由调用方串行使用，多个读取器可并发；它们都在父 Store 关闭后继续持有底层库，全部子句柄释放后目录租约才释放。

C 对应 `fqdb_store_reader_create/read/acquire/release/close`。`fqdb_store_get_shared`、`snapshot_get_shared` 和 `key_get_shared` 在一次调用中返回值所有者、字节和版本/到期信息，用 `fqdb_store_value_close` 释放 owner。

反复读取相同的一组键时，复用一个准备读取器；少数组合反复切换时，可在业务层使用有界读取器缓存并及时关闭淘汰项。不要每次调用都重新准备，也不建议为大量一次性随机组合无限建缓存。一次读32份档案的三轮调用方式对照见 [批读研究](STORE_ADAPTIVE_WAIT_20261110.md)。

普通get_many及新建结果的公共路径另已优化：批内复用不可变索引页中的查找位置，减少临时Generation计数，并用有界、已计费的预留额度合并预算原子操作；返回前归还未用额度，预算不足时重试精确需求。适用范围和当前验证见 [普通批读优化](STORE_BATCH32_REVIEW_20261110.md)。

## C#

```csharp
using var reader = db.PrepareReader(Encoding.UTF8.GetBytes("player/42"));
var player = reader.Read(batch =>
    batch[0].Found ? PlayerSave.Parser.ParseFrom(batch[0].Value) : null);
```

`Value` 是 `ReadOnlySpan<byte>`，无中间 `byte[]`。`StoreReadBatch` 和 `StoreSharedEntry` 是 `ref struct`，正常安全 C# 代码无法将其捕获到堆中或跨异步等待保留。`Read` 的同步回调结束后不要保留原生视图；DTO 或业务自己复制的数组可继续使用。重复热点读取请复用读取器和非捕获委托，不要每次重新准备键或创建捕获闭包。

`PrepareReader(byte[][])` 批量准备最多 4096 个键，保留顺序、重复键和空值；通过 `Found` 区分未命中和空值。整批使用同一个发布视图和 TTL 时间。`Release()` 释放保留结果，`Dispose()` 还释放键及读取器。回调与本读取器的关闭互斥；回调内递归读取、释放或关闭同一读取器会抛异常，其他线程关闭会等回调退出。回调可以修改数据库或使用其他读取器，当前字节仍保持原值。

一次性读取可用 `db.ReadShared(key, row => ...)`；重复读取优先准备读取器。原 `Get/GetMany/GetEntry` 保持独立数组的所有权，其中 `Get` 少一次中间载荷复制，批量复制读取直接读取原生行描述，元数据读取合并原生调用。已有 `PrepareKey().GetInto(target)` 继续适合需要可写目标缓冲的业务。

`examples/protobuf/csharp/PlayerSaves.cs` 的 `Load` 已示范直接解析共享 Span。核心托管程序集不依赖 Google.Protobuf。Unity 仍需宿主提供支持相应 Span 解析的业务 Protobuf 运行时；本轮没有 Unity IL2CPP 真机验证。

## Python

```python
value = db.get_shared(b"player/42")
if value is not None:
    with value:
        consume(value.value)  # readonly memoryview，无载荷复制

with db.prepare_reader([b"player/42", b"player/43"]) as reader:
    with reader.read_shared() as batch:
        if batch[0] is not None:
            consume(batch[0].value)
```

未命中返回 `None`，先判断再进入共享值上下文。

也有 `get_many_shared(keys)` 和准备键的 `get_shared()`。Python 允许 memoryview 及切片逃逸，所以它们各自保留原生租约；外层对象关闭、读取器再次读取、父 Store 关闭后，已导出的视图仍有效。批量中任一导出行会保留该整批结果。只有所有导出引用都释放后才解除其原生固定预算。ctypes、Python 包装对象和循环仍有调用成本，不承诺达到 C++ 的每次调用耗时。需要固定、可写目标时继续用 `bind_buffer`。

## Java

```java
try (Store.SharedValue value = db.getShared(key)) {
    if (value != null) consume(value.buffer()); // read-only direct ByteBuffer
}
try (Store.PreparedReader reader = db.prepareReader(new byte[][] {key1, key2});
     Store.SharedBatch batch = reader.readShared()) {
    if (batch.get(0) != null) consume(batch.get(0).buffer());
}
```

准备键也有 `getShared()`。`buffer()` 返回只读 direct 视图；它的 duplicate、slice 及类型视图应遵循 JVM 的缓冲别名约定。JNI 在运行时检查只读/duplicate/slice 是否通过附件保留原始 direct 缓冲。检查通过才共享，并通过 PhantomReference/ReferenceQueue 在最后一个缓冲别名不可达后释放原生租约。`close()` 解除包装对象的引用，不能提前释放仍被外部缓冲别名使用的原生字节。GC 与队列处理决定最终释放时点，不保证立刻归还预算。

不支持这种附件关系的 JVM 自动复制到 JVM 管理的 direct 缓冲，避免将原生租约绑定到错误的对象生命周期；`isZeroCopy()` 可查询结果。可用 `-Dio.fengqudb.shared.copy=true` 显式选择复制路径。已实际验证 Windows OpenJDK 的共享及强制复制两条路径；Android/iOS 真机不属于本轮验证范围。Java 热循环若要求确定的内存上限和释放时点，继续使用准备键加复用 `getIntoDirect`，这条路径只做一次载荷复制。

## 性能与验证边界

热点读取现在用发布序号判断复用：视图未变化且 TTL 有效时，准备读取器无需重复取得 View 引用。普通 `get/get_shared` 另有每线程一个键的弱引用记忆，仅在开启数据缓存、键不超过 256 字节时使用；不隐藏保留数据库、文件代或完整值，缓存关闭时仍走原校验路径。发布中、序号耗尽、时间回退或到期时自动回到正常解析路径。

发布序号变化后，读取器在同一个新视图和 TTL 时间内核对每条记录的完整引用及文件代，仅替换变化的结果；无关写入不再重建全部描述或投影。版本变化即使字节相同也会刷新，到期、删除、重插入和文件代转换仍按新状态处理。整批刷新中若发生错误，会清除读取器结果，防止下一次返回半更新状态。预算紧张时允许释放失效结果后在同一视图完整重建一次。普通共享租约也减少了多余的预算所有权引用和载荷引用操作，预算上限、跨线程关闭和关闭父库后的子句柄存活约定不变。

只需要部分 Protobuf 字段时可复用投影读取器：

```cpp
auto reader = db.prepare_project_reader({"player/42", "player/43"}, {1, 2});
auto rows = reader.read(); // 每行 value 是只含字段 1、2 的 Protobuf
```

C 对应 `fqdb_store_project_reader_create`；C# 为 `PrepareProjectReader(byte[][], uint[])`，Java 为 `prepareProjectReader(byte[][], int[])`，Python 为 `prepare_project_reader(keys, fields)`。读取、释放、并发与共享别名的生命周期沿用上文。同一字段计划准备一次，未变化时复用已投影结果；初次及失效后的投影仍读取完整源值并验证完整外层 wire。最多 256 个字段号，重复字段号忽略，空计划产生空 Protobuf。结果使用固定内存预算，不修改原始共享二进制。

共享接口省掉的是载荷副本、重复键封送和结果重建；它不省略事务或持久化步骤，不使任意语言的对象解析免费，也不会把随机磁盘读取变成内存读取。准备读取器在视图变化、TTL 到期或时间回退时刷新；频繁写入、冷数据及每次重建读取器不具有不变热数据的复用条件。

原始对比及验证见 [本轮记录](STORE_SHARED_READ_REVIEW_20261108.md)。原生回归包含 TTL 边界、时间回退、空值/未命中、原子整批发布、关闭后的子句柄、COW 和 pin 失败；语言测试包含派生视图、回调异常/重入、并发关闭、包装分配失败和 GC 释放。

准备键的读取还会在发布序号稳定、TTL有效时复用已核对元数据和弱值，减少重复取得视图及进入值缓存；新增固定元数据计入准备键预算。命中不强持有载荷或旧文件代，快照解析与最新值记忆隔离。`get_into`仍在容量不足时跳过冷载荷读取。收益要求复用句柄；每次调用重新准备键会重复付出构造成本。普通按键接口继续使用原有路径，实际范围和验证见[按键读写后续优化](STORE_KEY_CRUD_MERGE_20261110.md)。

原生测试入口为 `paged_bulk_path`；C# 独立入口为 `tests/csharp/StoreSharedSmoke.csproj`。运行 `scripts/verify-store-buffers.py --library <核心> --java-bin <JDK/bin> --output <新目录> --csharp` 可集中执行复制缓冲及共享路径测试，要求核心与 JNI 来自同一源码构建。
