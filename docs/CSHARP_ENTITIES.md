# C# 实体与单值存取

使用精简 SDK 时，运行库 DLL 位于 `lib/managed`、分析器 DLL 位于 `lib/analyzers`，也可选择 `bindings/csharp` 的运行库源码。下面针对 `bindings/csharp/Generator`、`tools/FengquDB.CodeGen` 的构建命令属于开发仓库流程；SDK 用户直接使用预编译分析器，或使用外层原生脚本生成。按语言的最短接入路径见 [C# 指南](languages/csharp.md)。

实体字段在 C# 中标记稳定的 protobuf 字段号，编译时自动生成编码、解码、键和查询列。无需编写 `.proto` 或手动运行 protoc。运行库保持 netstandard2.1，生成器为 netstandard2.0，使用 Roslyn 4.1 API。

## 最小学生示例

```csharp
using FengquDB;
using FengquDB.Entities;

[FqEntity("student/")]
public partial class Student {
    [FqKey, FqField(1)] public int Id { get; set; }
    [FqField(2)] public string Name { get; set; } = "";
    [FqField(3)] public int Score { get; set; }
}

// 放在业务方法中。Unity 使用 Application.persistentDataPath 下的目录。
using var db = new Store(databaseDirectory);
db.Put(new Student { Id = 1, Name = "张三", Score = 95 });
Student? student = db.GetEntity<Student>(1); // 键不存在时为 null。
db.Sync();
```

每个实体需要一个公开无参构造函数，恰好一个同时带 FqKey、FqField 的整数或字符串键。实体使用独立、稳定的前缀。数字键采用不受区域设置影响的十进制 UTF-8，例如 `student/1`。字段号、物理编码和前缀在数据发布后保持稳定，删除字段后保留其旧编号。

## 只存整数、字符串或其他单值

```csharp
db.Put("score", 95);
int score = db.Get<int>("score");
db.Put("name", "张三");
string name = db.Get<string>("name");
db.Put("coins", 123456789L);
db.Put("enabled", true);
db.Put("ratio", 0.5);
if (!db.TryGet<int>("missing", out int value)) {
    // 缺失与已经存入的 0 可以区分。
}
```

Store 上的类型化 Put 返回此次写入的版本号，错误会抛出异常。Get<T> 缺失时抛出 KeyNotFoundException，TryGet 返回 false。支持 byte、sbyte、short、ushort、int、uint、long、ulong、bool、float、double、string、byte[]。null 标量值会被拒绝，空字符串和空字节数组可以保存。

单值统一编码为 protobuf 消息的字段1。读取时使用相同类型。有符号整数使用 ZigZag，对应 sint32/sint64；无符号整数使用 varint。float/double 为标准小端 fixed32/fixed64。字符串为严格 UTF-8，字节数组为 bytes。原有 byte[] 键与 byte[] 值的 Put 仍接收完整原始载荷。

键的原生上限为64 KiB，值的上限为16 MiB，值长度包括 protobuf 标签和长度前缀。类型化写入在租用大缓冲前检查这些限制。

## 事务、可靠写入与查询

```csharp
using (var tx = db.Begin()) {
    tx.Put("score", 96);
    tx.Put(new Student { Id = 1, Name = "张三", Score = 96 });
    int current = tx.Get<int>("score");
    tx.Commit(forceReliable: true);
}
db.Put("paid-order", 123L, forceReliable: true);
var scoreColumn = Student.Cols.Score;
var nameColumn = Student.Cols.Name;
var rows = Q.From("student/").Where(scoreColumn >= 90L)
    .Select(nameColumn, scoreColumn).OrderBy(scoreColumn.Desc()).Limit(20).Run(db);
```

默认性能模式在内存写入完成后确认，周期和阈值仍为50 ms / 1 MiB。关键单笔使用 forceReliable，事务在 Commit 指定可靠性。应用可在适当的生命周期调用 Sync。

生成列与编码使用相同字段号和物理类型。小整数的查询列使用 long/ulong，float 查询列使用 double。byte[] 暂不生成字符串查询列。

## 高频读取

```csharp
using var scoreReader = db.PrepareValue<int>("score");
int score = scoreReader.Read(); // 固定键只编码一次。
using var studentReader = db.PrepareEntity<Student>(1);
Student student = studentReader.Read();
using var fields = db.PrepareReader(Student.MakeKey(1));
int onlyScore = fields.Read(rows => Student.ReadScore(rows[0].Value));
```

普通 typed Get 直接从原生共享不可变字节解码。pin 预算不足时使用原有复制读取的原生缓冲，仍避免额外的托管载荷数组。实体写入路径也由编译器生成，直接调用具体类型的编码方法。小写入使用栈缓冲，并通过 UTF-8 长度安全上界减少重复扫描，大写入使用 ArrayPool，在原生调用完成后归还。每次调用的缓冲独立，支持并发使用数据库。

整数等值的热读取及只读数字字段可以零托管分配。完整实体解码需要创建实体和所需字符串或字节数组。生成器不会缓存并返回可变业务对象。写入期间避免其他线程修改同一个实体实例。

## 普通 .NET 项目接入

业务项目引用运行库和生成器，生成器以 Analyzer 方式引用。

```xml
<ItemGroup>
  <ProjectReference Include="path/to/bindings/csharp/FengquDB.csproj" />
  <ProjectReference Include="path/to/bindings/csharp/Generator/FengquDB.Generator.csproj"
                    OutputItemType="Analyzer" ReferenceOutputAssembly="false" />
</ItemGroup>
```

完整可运行项目在 `examples/csharp/Students.csproj`。使用 dotnet run 启动前，使对应平台的 fengqudb 原生库位于程序目录或系统动态库搜索路径中。

## Unity 接入

1. SDK 用户使用 `lib/analyzers/FengquDB.Generator.dll`；源码仓库用户先构建 FengquDB.csproj 和 Generator/FengquDB.Generator.csproj 的 Release 版本。
2. 将 bindings/csharp 顶层的 `.cs` 文件、FengquDB.asmdef 和 csc.rsp 放入 Unity 的独立 Runtime 目录，另按目标平台放置原生库。Generator 子目录的源码留在数据库工程中。
3. 将 FengquDB.Generator.dll 放入实体脚本所在程序集的目录，在插件 Inspector 中关闭 Any Platform 和 Editor 的运行时加载，并添加区分大小写的 `RoslynAnalyzer` 资源标签。
4. 在该程序集内编写带特性的 partial 实体类。Unity 编译脚本时生成对应方法。

生成器的 Microsoft.CodeAnalysis 依赖由编辑器编译器提供，无需把 Roslyn 的 DLL 一并导入 Assets。编辑器需要能够加载随包的源代码生成器；无法自动生成时使用随包脚本。代码使用常规泛型和静态生成方法，目标包含 IL2CPP/AOT，具体 Player 和设备运行需分别验证。

运行库源码交给 Unity 编译时，iOS 的 UNITY_IOS 条件会自动选择 `__Internal`，链接宿主内的静态原生库。Windows/Linux/Android 使用对应动态库。若采用预编译托管 DLL，需要针对 iOS 带 UNITY_IOS 常量单独构建并设置平台导入范围。

## 自动更新与强制生成 CLI

Unity 保存实体脚本后，在相关程序集实际重新编译时运行生成器。关闭 Auto Refresh、暂停脚本编译或等待退出 Play Mode 时，生成会随编译延后，可执行 Assets / Refresh 触发导入。实体和生成器需要处在正确的程序集作用范围。

.NET 项目引用 Analyzer 后，每次 C# 编译都会运行生成流程。普通 dotnet build 和 IDE Build 会在实体源码或其依赖发生变化时自动更新结果。未变化的工程或实体可以复用增量结果，生成代码默认保存在编译器内部。修改其他 partial 文件中的持久化字段也会更新序列化代码。

共享模型请优先使用不依赖 .NET 的[原生跨语言生成器](MODEL_CODEGEN.md)。以下工具是读取已有 C# 特性类的可选语义适配器。

外部程序可以调用 `fqdb-csharp-codegen`。工具每次都从当前源码重新运行同一个生产生成器，输出一个确定的 FengquDB.Entities.g.cs 文件，通过单文件原子替换发布。内容相同时保留文件时间，避免 Unity 无意义地重新编译。

```powershell
# 在仓库中构建命令行工具，需要 .NET 10 SDK。
dotnet build tools/FengquDB.CodeGen/FengquDB.CodeGen.csproj -c Release

# 按业务工程的引用、编译条件和目标框架强制计算。
dotnet tools/FengquDB.CodeGen/bin/Release/net10.0/fqdb-csharp-codegen.dll generate `
  --project examples/csharp/Students.csproj --configuration Release --output build/student-generated

# Unity 或其他外部工具可指定实体源码文件、目录及所需程序集引用。
fqdb-csharp-codegen generate --source Assets/Models --output Assets/Models/Generated

# 仅检查是否需要更新，适合 CI。
fqdb-csharp-codegen generate --source Assets/Models --output Assets/Models/Generated --check
```

`--source`、`--reference` 和 `--define` 可以重复传入。源码模式引用 Unity 类型时，使用 --reference 提供对应 Unity 程序集。每次处理一个 C# 程序集，将生成文件放到实体所在的 asmdef 范围内。工程模式可用 --framework 选择多目标工程的一项，默认执行 restore，已经恢复依赖时可传 --no-restore。

CLI 文件默认受 `FQDB_MANUAL_ENTITIES` 条件保护。在常规自动模式下，可以保留此文件作为备用。要用它参与编译，在 .NET 的 DefineConstants 或 Unity 对应目标平台的 Scripting Define Symbols 中添加 `FQDB_MANUAL_ENTITIES`。此时分析器停止重复生成，继续检查文件中的模式指纹。字段、类型或前缀改变后，旧文件会触发 FQDB002，重新执行 CLI 即可。分析器未加载的情况下，可以在发布前调用 --check 校验文件是否过期。

CLI 对当前编译临时设置 `FQDB_FORCE_ENTITY_GENERATION`，因此启用手动模式的工程也可以再次生成。此标记由工具管理。生成器不支持的实体、编译错误及外来同名输出文件均使命令失败，原有有效输出保持完整。

标准输出为 JSON，包含实体数、输出路径、内容哈希和 changed。退出码0表示成功，1表示错误，--check 返回2表示文件缺失或过期。外部工具等待退出码0后即可编译或刷新 Unity。

工具也可以打包成 .NET tool，供外部系统安装和调用。

```powershell
dotnet pack tools/FengquDB.CodeGen/FengquDB.CodeGen.csproj -c Release -o build/codegen-packages
dotnet tool install FengquDB.CodeGen --tool-path ./codegen-tools --add-source ./build/codegen-packages --version 0.3.2
```

CLI 和生成器均在构建阶段运行，数据库运行时的编码、缓存及原生接口路径沿用生成代码。

## 格式与支持范围

实体支持上述标量字段、可空数值及可空引用。嵌套对象、列表、map、枚举、decimal、Guid、继承的持久化字段、record、泛型实体、嵌套类、只读字段和 init 属性会产生编译诊断，避免静默遗漏。运行时普通成员未标记 FqField 时不参与持久化。

非空标量的零值和空字符串也明确编码，以保留核心字段查询的存在性。可空字段为 null 时省略，解码缺失字段时使用相应的语言默认值，非可空 string/byte[] 分别为空字符串/空数组。重复标量标签取最后一个。未知字段保留并随下一次实体保存重新写出，已知字段热路径不分配未知字段缓冲。

自动生成 EncodedSize、EncodeTo、Decode、MakeKey、Read字段名、View 和 Cols，业务成员避免使用这些名称及 `__fq` 前缀。实体类型已知时可以直接调用 EncodeTo/Decode 与其他 protobuf 语言互操作。

## 2026-11-13 验证

Windows / .NET 10 已验证标量极值、protobuf 固定向量、实体往返、属性校验、可空值、未知字段保留、非法输入、事务隔离与回滚、TTL、强制可靠写、重开恢复、并发池缓冲及 pin 预算回退。生成器诊断测试覆盖重复或非法字段号、不支持类型、只读成员、缺键、record、泛型及命名冲突。原有 C# 共享读取测试通过，正式学生示例可正常构建运行。

PTFramework 的 Unity 2022.3.62f3c1 编辑器实际加载生成器，并使用运行库源码程序集完成实体、单值、事务、生成查询列和重开读取。IL2CPP Player、Android/iOS 真机运行尚未在本轮验证。

后续已实测 Unity 在其他 partial 文件增加字段后重新生成对应方法。关闭分析器标签后，CLI 文件与程序集内的手动编译标记仍可完成编码和解码。.NET 测试覆盖普通增量 build、自动与备用文件共存、过期模式诊断、独立手动编译及 CLI 的源码和工程两种输入。打包安装后的命令行入口已验证工程模式生成。

17字节学生记录的同机最小热测如下。读取和编码取5组交替测量中位数，三种写入方式在同一测量窗口内按201组短块轮换顺序，默认性能模式保持50 ms / 1 MiB。

| 操作 | 基线 | 类型化接口或生成代码 |
| --- | --- | --- |
| 固定键整数读取并解码 | 手写83.1 ns | 86.7 ns |
| 写入复用缓冲的 protobuf 编码 | 手写72.0 ns | 74.0 ns |
| 单条 Put | 预编码字节和键1.864 µs | 实体编码、键生成与写入2.215 µs |
| 同样包含编码、键生成和写入的实体保存 | 手写2.230 µs | 生成代码2.215 µs |

热标量读取、只读数字字段和小值类型化写入均通过零托管分配断言。完整实体读取需要创建实体及所含引用值。以上数字是短测结果，序列化与键生成相对预编码裸写仍有实际开销，完成相同工作的生成路径与手写路径接近。
