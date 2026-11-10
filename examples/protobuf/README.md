# 业务 Protobuf 模型接入

业务项目维护 `.proto`，用官方 `protoc` 生成 DTO，再由业务序列化器转换为字节交给 Store。数据库不会生成业务类，也不把任意 C#/Java/Python 对象自动映射为记录。数据库内部日志格式与业务 `.proto` 是两件独立的事。

本例使用 `player_v1.proto` 和新增字段的 `player_v2.proto`，覆盖显式零值/空字符串、ZigZag 金币、UTF-8、嵌套背包、repeated、map 和二进制令牌。两个版本的包名不同，仅为让验证程序同时编译两版；实际应用添加字段通常保持原包名。

## 生成与依赖

```sh
protoc -I examples/protobuf \
  --csharp_out=GENERATED_CSHARP \
  --java_out=GENERATED_JAVA \
  --python_out=GENERATED_PYTHON \
  player_v1.proto player_v2.proto
```

输出目录须预先创建。生成文件位于业务工程或构建目录，不要放入数据库核心源码。验证固定使用 protoc 36.2、C# Google.Protobuf 3.36.2、Java protobuf-java 4.36.2、Python protobuf 7.36.2；各语言版本号不完全相同是官方发布方式。第三方运行库由业务应用按需引用，FengquDB 核心与原有 C# 绑定项目不增加这些依赖。

- C#：在业务项目引用 `Google.Protobuf`，同时编译生成的 `.cs` 与 `csharp/PlayerSaves.cs`；桌面验证项目为 `csharp/PlayerExample.csproj`。
- Java：将生成的 `.java` 和业务源码一起编译，类路径包含匹配的 protobuf-java jar；使用数据库时再加载核心与 JNI。
- Python：在独立虚拟环境安装匹配版本的 `protobuf`，让生成的 `*_pb2.py` 和 `bindings/python` 位于模块搜索路径。
- C++：已有独立 wire primitives 可用于简单手写业务编码；也可在业务项目使用官方 C++ 生成器和运行库。它们不是数据库的链接依赖。

完整执行入口：

```text
python scripts/verify-business-protobuf.py
  --protoc PROTOC_36_2
  --java-bin JDK_BIN
  --java-runtime protobuf-java-4.36.2.jar
  --python ISOLATED_PYTHON_WITH_PROTOBUF_7_36_2
  --library ABSOLUTE_FENGQUDB_LIBRARY
  --native-benchmark OPTIONAL_FQDB_STORE_BINDING_BENCH
  --output FRESH_OUTPUT_DIRECTORY
```

这是多行参数示意；实际 Shell 中写为一行或使用对应的续行符。程序生成并保存 DTO/descriptor、编译 C#/Java、执行三语言共库升级流程，再测量应用调用成本。参数需使用开发版 Store 的匹配原生库与 JNI，旧发布 SDK 没有这些接口。

## C# 与 Unity

```csharp
using FengquDB.Examples;
using FengquDB.Examples.V2;

using var saves = new PlayerSaves(persistentRoot);
saves.Save(new PlayerSave { PlayerId = 42, DisplayName = "Alice", Coins = 0 });
PlayerSave? loaded = saves.Load(42); // null 表示无记录。
```

`PlayerSaves` 选择 `<persistentRoot>/db/main`，调用者可设置根目录及库名。Unity 将 `Application.persistentDataPath` 作为根目录；示例为 `examples/unity/TypedFengquSave.cs`。业务程序集需引用生成 DTO、Google.Protobuf、PlayerSaves 和原有 FengquDB 绑定。不要将 .NET 10 桌面验证可执行程序集放进 Unity；应按 Unity 支持的程序集目标编译这些源文件。

这个基础封装的 Save 覆盖一条完整记录。并发读改写应使用 StoreTransaction，或携带 GetEntry 返回的版本调用 CompareExchange；遇到版本冲突后重新加载业务对象，避免用旧 DTO 覆盖其他线程的修改。

Unity IL2CPP/Android/iOS 仍须在目标工具链与设备验证。生成代码使用显式 Parser，无需在每次存取时根据反射查字段；裁剪配置应按实际生成类型与调用路径验证，避免未经测量就保留整个程序集。

## 字段存在性、索引与结构升级

数据库字段索引提取顶层 wire 字段，不加载业务 descriptor。`player_id` 对应字段 1/type 1（uint64），`display_name` 对应字段 2/type 4（bytes/string），`coins` 对应字段 3/type 5（sint64）。示例为前两者设置 unique+required（flags=3），金币仅 required（flags=2）。嵌套、map 和 repeated 在此作为业务载荷保存，不自动成为多值索引。

普通 proto3 标量的零值常被序列化器省略；数据库无法从缺失 wire 字段推断业务默认值。本例用 `optional`，显式赋值 0 或空字符串仍保留字段，能与 required 索引一致。required 表示 wire 字段存在，不表示数值非零或字符串非空；业务非空/数值范围规则由业务校验负责。

升级添加字段 8/9，保持旧字段号与类型。验证流程为 Python v2 写入 → C# v1 解析后修改 → Java v1 用 toBuilder 修改 → C#/Python v2 重新读取，确认新增字段仍在。旧代码应修改已解析的对象或从其克隆/建造器开始，不要只复制已知字段重建消息，也不要调用 DiscardUnknownFields。删掉的字段号/名字应 reserved；不要复用字段号或把 int64 与 sint64 等不同解释互换。

Java 的 uint64 由 signed long 保存同一组 64 位；ID 超过 Long.MAX_VALUE 时，生成业务键应使用 Long.toUnsignedString，不能把同一 ID 写成负数字符串。字符串索引按 UTF-8 字节比较；大小写、Unicode 规范化与业务非空规则由应用明确约定。

新增字段的 required 索引应在旧写入端升级、数据迁移完成后再启用。数据库约束不是业务 schema 自动迁移器。Protobuf map 顺序及未知字段重排可能使语义相同的消息产生不同字节；数据库按原字节保存与比较，整理不会重编码消息。

## 调用成本测量与复用

驱动器给三种语言使用同一份含 4096 字节二进制字段的载荷，分别测量：序列化、解析、复制字节读取、准备键配合复用目标缓冲读取、读取加解析、每批 32 条读取。先预热，再顺序交替语言执行多轮；报告吞吐中位数和抽样 P99。批量吞吐按记录数归一，P99 仍是一整批调用的时间。

可选原生对照为 `fqdb_store_binding_bench`，由启用 FQDB_BUILD_TOOLS 的源码构建产生；它提供相同载荷的独立字节副本、准备键复用缓冲及批量独立副本，批量副本在同一调用中全部保持存活。原生对照不链接业务 Protobuf 运行库，不测 DTO 序列化。

另测 Python `prepared.bind_buffer(bytearray).read()` 的固定参数复用，以及 Java `prepared.getIntoDirect(ByteBuffer)`。两者均按需启用，仍然复制字节；缓冲容量和生命周期要求见 [复用读取](../../docs/STORE_BINDING_BUFFERS.md)。

这些指标包含语言运行时、GC、官方 Protobuf 实现和封装成本，不是纯 FFI 开销，也不是其他数据库的公平吞吐上限。只读测试使用热缓存，启动、生成、编译和数据准备不在计时内。正式定位可设置 `--iterations 1000000 --runs 3`；结果和原生对照见 [业务模型验证记录](../../docs/PROTOBUF_BUSINESS_REVIEW_20261107.md)。

业务只在对象变化时序列化，未变时复用已保存的字节；mutable DTO 的缓存必须由业务可靠地标记失效。`GetInto` 复用目标数组可减少数组分配，仍然会复制字节；`GetMany` 可减少跨语言调用次数。底层私有完整二进制的覆盖与减法裁剪仍由数据库实现，不需要重新拼接所有可复用块。变长整数和长度前缀会改变字段位置，不要对生成的 Protobuf 消息硬编码 Patch 偏移。
