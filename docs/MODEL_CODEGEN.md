# 跨语言模型与原生代码生成

日常开发优先使用[源码实体生成](SOURCE_ENTITIES.md)：直接维护业务类，由构建或双击脚本生成原类 codec。本页介绍仍可选用的跨语言共享模型流程，适合需要一份定义生成各语言 DTO 的项目。

推荐入口是独立原生程序 `fqdb-codegen`。Windows 版静态链接 MSVC 运行库；Linux 版使用系统 C++ 运行库。运行生成器不需要 .NET、JDK、Python、Node、Go、Rust、protoc 或数据库动态库；目标项目只需要它自己原本使用的语言环境。模板嵌入工具，无须额外目录。

工具在开发机/构建机运行，生成的源文件再随业务编译；Android/iOS 使用开发机的生成器，不在移动设备执行工具。

## 最小模型与命令

直接复制 [学生模型](../examples/models/student.fqmodel.json)，一份 JSON 定义生成所有语言：

```json
{
  "version": 1,
  "namespace": "game.models",
  "entities": [{
    "name": "Student",
    "prefix": "student/",
    "fields": [
      {"name":"id", "type":"sint32", "number":1, "key":true},
      {"name":"name", "type":"string", "number":2},
      {"name":"score", "type":"sint32", "number":3}
    ]
  }]
}
```

```sh
fqdb-codegen generate --schema student.fqmodel.json --output Generated
fqdb-codegen generate --schema student.fqmodel.json --output Generated --lang csharp,java,python
fqdb-codegen generate --schema student.fqmodel.json --output Generated --check
```

默认生成 C、C++、C#、Java、Python、Go、Rust、Node.js/TypeScript。`javascript`、`typescript` 都是 `node` 的别名，生成 JavaScript 实现及 `.d.ts` 类型声明，无须额外转译。输出分别放在 `c/`、`cpp/`、`csharp/`、`java/`、`python/`、`go/`、`rust/`、`node/`。

手动执行每次都根据当前模型重新计算。无变化不改文件时间；`--check` 不写文件，退出 0 表示当前、2 表示缺失/过期、1 表示错误。错误写为 JSON，供外部程序读取。

每份模型使用自己的输出根目录。清单追踪工具生成文件；模型删除或重命名实体时，会清理所选语言中失效的工具文件。工具拒绝覆盖无生成标记的文件、拒绝输出经过符号链接或 Windows reparse point，并使用输出锁防止同时生成。单个文件原子替换；整套文件不是跨文件原子事务，构建必须等待命令成功。不要手改带生成标记的文件。

## 实际调用

C# 生成类直接实现现有类型化存取契约，不需要额外分析器：

```csharp
using FengquDB;
using game.models;

using var db = new Store(saveDirectory);
db.Put(new Student { id = 1, name = "张三", score = 95 });
Student? student = db.GetEntity<Student>(1);
db.Put("score", 95);                  // 只存整数，无须模型
int score = db.Get<int>("score");
db.Put("name", "张三");              // 只存字符串，无须模型
string name = db.Get<string>("name");
if (student != null) {
    student.score = 100;
    db.Put(student, forceReliable: true);
}
```

Python：把 `Generated/python` 和 FengquDB Python 绑定加入模块路径。

```python
from fq_models import Student
student = Student(id=1, name="张三", score=95)
db.put(student.fq_key(), student.fq_encode())
loaded = Student.fq_decode(db.get(student.fq_key()))
# 高频调用复用 bytearray：buffer.clear(); student.fq_encode_into(buffer)
```

Java：把 `Generated/java` 加入源码目录，生成的包名是模型 namespace。

```java
import game.models.Student;
Student student = new Student();
student.id = 1; student.name = "张三"; student.score = 95;
db.put(student.fqKey(), student.fqEncode());
Student loaded = Student.fqDecode(db.get(student.fqKey()));
// 复用输出数组：int count = student.fqEncodeInto(buffer, offset);
```

C++：包含 `cpp/fq_models.hpp`；同目录的 `fq_models.h` 是独立 C wire 实现。

```cpp
game::models::Student student;
student.id = 1; student.name = u8"张三"; student.score = 95;
db.put(student.fq_key(), student.fq_encode());
if (auto value = db.get(student.fq_key())) {
    auto loaded = game::models::Student::fq_decode(value->view());
}
// std::string buffer; student.fq_encode_into(buffer); // 复用容量
```

C：`game_models_Student` 使用借用字节视图，`*_fq_encode`/`*_fq_key` 写入调用方缓冲，`*_fq_decode` 返回成功/失败。调用 `*_fq_encode(&entity, NULL, SIZE_MAX, &size)` 可测量空间；长度超过 16 MiB 仍返回失败。解码视图及未知字段源引用输入，输入必须存活且保持不变；编码输出不能与实体引用的输入缓冲重叠。C++ 解码结果拥有自己的字符串和字节。原生源码按 UTF-8 编译，MSVC 使用 `/utf-8`。

Go：把 `go/fq_models.go` 放进目标包（包名取 namespace 最后一段）。字段首字母导出，如 `Id`、`Name`、`Score`；`student.FqKey()`、`student.FqEncode()` 返回 `([]byte,error)`；`StudentFqDecode(data)` 返回 `(Student,error)`。`FqEncodeInto(dst[:0])` 复用切片容量。

Rust：用 `#[path=".../fq_models.rs"] mod models;` 引入；`Student::fq_decode(bytes)`、`student.fq_encode()`、`student.fq_key()` 返回 `Result`。`fq_encode_into(&mut buffer)` 追加到调用方 Vec，先 `clear()` 即可复用容量。类型实现 `Default`，可以 `Student { id:1, name:"张三".into(), score:95, ..Default::default() }`。

Node/TypeScript：`const {Student}=require('./Generated/node/fq_models');`，`new Student()` 后设置字段，`db.put(student.fqKey(),student.fqEncode())`；`Student.fqDecode(bytes)` 返回实体。`fqEncodeInto(buffer)` 写入预留 Uint8Array 并返回实际长度；编码结果是输出缓冲的视图，复用缓冲前须结束对旧结果的使用。

其他语言也可以用一字段实体表达单值记录，或继续用现有原始字节 API；目前开箱即用的 `Put(key, int/string)` 重载属于 C# 绑定，不能把其他绑定的原始 bytes API 说成自动标量接口。

## 类型、格式和性能契约

| 模型类型 | C/C++ | C# | Java | Python | Go | Rust | Node/TS |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sint32 / sint64 | int32_t / int64_t | int / long | int / long | int | int32 / int64 | i32 / i64 | number / bigint |
| uint32 / uint64 | uint32_t / uint64_t | uint / ulong | long / long 位模式 | int | uint32 / uint64 | u32 / u64 | number / bigint |
| bool | uint8_t，限 0/1 | bool | boolean | bool | bool | bool | boolean |
| float / double | float / double | float / double | float / double | float | float32 / float64 | f32 / f64 | number |
| string | UTF-8 view / string | string | String | str | string | String | string |
| bytes | view / string | byte[] | byte[] | bytes | []byte | Vec<u8> | Uint8Array |

带符号整数用 ZigZag（sint），不是 protobuf `int32/int64` 的补码编码。Java uint64 使用 long 的无符号位模式，键用 `Long.toUnsignedString`。JS 64 位字段必须传 bigint，防止 Number 精度丢失。

所有已知字段都显式编码，包括零、false、空字符串；缺失字段解码为默认值，重复字段最后一个生效。未知字段保留并在再次编码时追加；不保证原始字段顺序。已知编号但 wire 类型不同的字段按未知字段保留。严格验证 UTF-8、varint/长度/整数溢出、布尔值和 group 嵌套上限 64；值上限 16 MiB、键上限 64 KiB。键统一为 UTF-8 前缀加整数十进制或原始字符串。

当前模型 v1 支持上表的扁平标量，暂不支持 optional/null、嵌套实体、数组、map、enum、oneof。传入不支持的类型或未知模型属性会报错，不会悄悄漏字段。每个实体有且只有一个整数或字符串键，前缀以 `/` 结尾且不同实体不能相互覆盖。namespace 使用点分隔的可移植标识符，实体首字母大写、字段首字母小写，跨语言关键字拒绝使用。

新增字段使用新编号；删掉的编号加入实体 `reserved` 数组；已发布字段不要改编号或类型。修改模型不是自动迁移旧存档。不要在不同模式中维护同一实体的两份模型；C# 类特性流程和跨语言模型流程分别适用于单语言已有类和共享模型。

编码/解码是编译期生成的字段访问，不使用运行时反射、JSON 中转或 protobuf 通用对象树。C/C++ 支持调用方缓冲和借用解码；C# 小写入使用栈、大写入使用池，并接入共享读取解码；其他语言提供输出缓冲复用。便利的“返回新数组”接口仍有相应语言的分配成本；不能把“原生 CLI”当成所有语言接口性能相同的保证。

## 构建接入

原生 CLI 可独立构建，不构建数据库：

```sh
cmake -S tools/codegen -B build/codegen -DCMAKE_BUILD_TYPE=Release
cmake --build build/codegen --config Release
cmake --install build/codegen --config Release --prefix artifacts/codegen
```

主工程 `FQDB_BUILD_TOOLS=ON` 同时构建并安装它。发布只需对应宿主系统的可执行文件；Linux 发行版仍应匹配宿主 glibc/libstdc++ 基线。

.NET 工程可导入 `bindings/csharp/build/FengquDB.Models.targets`，设置 `FqdbCodegenExecutable` 和 `FqdbModelSchema`。每次正常编译前重新检查模型，输出不变不改时间，自动把生成文件加入编译；不要求安装分析器。设计时 IntelliSense 不启动生成器，首次正常编译后可见类型。

```xml
<PropertyGroup>
  <FqdbCodegenExecutable>$(MSBuildProjectDirectory)/tools/fqdb-codegen.exe</FqdbCodegenExecutable>
  <FqdbModelSchema>$(MSBuildProjectDirectory)/student.fqmodel.json</FqdbModelSchema>
</PropertyGroup>
<Import Project="path/to/FengquDB.Models.targets" />
```

Unity 现有 `[FqEntity]` 类保持原有自动编译生成。使用共享模型时，在资源导入/构建步骤调用原生命令，把 `csharp/` 生成目录放在 Assets 内或包源码内；已有实体改类即生成的行为见 [C# 接入](CSHARP_ENTITIES.md)。原生 CLI 可由 CI 或外部工具随时强制调用。

CMake 使用 `cmake/FengquDBModels.cmake` 的 `fengqudb_generate_models(SCHEMA ... OUTPUT ... COMPILER ... TARGET generate_models LANGUAGES cpp)`，业务目标 `add_dependencies(app generate_models)`。交叉编译必须指定开发机的 COMPILER，不能运行 Android/iOS 目标工具。Java/Go/Rust/Node/Python 构建前也可直接执行同一命令。

`fqdb-csharp-codegen` 是可选的原 C# 项目/源码语义适配器，仍使用 Roslyn/.NET 10 解析真实工程引用。它不是跨语言 CLI 的依赖；保留用于已有 C# 特性类的手动备用生成，不要求其他语言用户安装 .NET。
