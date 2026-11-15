# 只维护业务实体源码

开发者维护自己的类/结构体，原生工具推断受支持的字段类型，记录稳定编号，生成直接访问原实体的 codec。JSON 中间模型由工具写出，不需要手填，也不生成第二套业务 DTO。

## 两个使用入口

优先把生成步骤接入业务构建。所有支持语言同时保留外层的手动工具：

- Windows：把 `fqdb-codegen.exe` 和 `fqdb-generate.bat` 放在项目根目录，双击 bat。也可以把项目目录拖到 bat 上。
- macOS：把宿主版本 `fqdb-codegen` 和 `fqdb-generate.command` 放在项目根目录，给两者执行权限，双击 command。首次打开时可能需要按 macOS 的本地可执行程序规则允许运行。
- Linux/终端：`sh fqdb-generate.command /path/to/project`。

脚本调用 `fqdb-codegen project --root 项目目录`。已有构建流程运行过时，它会使用工具自动记录的本机配置，重用相同源目录、输出目录、语言和编号锁；不会因为手动兜底而另生成到错误目录。首次没有配置时扫描项目并输出到 `Generated`。命令执行失败会保留错误信息，窗口不会假装生成成功。

```sh
fqdb-codegen generate --source . --output Generated
fqdb-codegen generate --source . --source-language java --output build/generated/fengqudb
fqdb-codegen project --root . --check
```

`--source` 可指定一个文件或项目目录；目录模式跳过 build、bin、obj、target、node_modules、dist、venv、Generated 等产物。只处理显式标记的实体，不扫描所有业务类就自动存储。C++ 的 `.h` 文件请加 `--source-language cpp`；`.hpp` 自动按 C++ 识别。

手动选择 Models/src 等子目录时，建议在项目根目录运行并显式传 `--schema-lock FengquDB.schema.lock.json`，让编号锁和本机配置留在根目录，根目录双击脚本才能重放相同设置。Python 的 src 布局应把 src 作为输入根，详见 [Python 指南](languages/python.md)。

## 实体标记与类型

```java
// fqdb:entity prefix=student/ key=id
public class Student {
    public int id;
    public String name = "";
    public int score;
}
```

Python 用 `# fqdb:entity ...`，其他语言用 `// fqdb:entity ...`。省略 key 会找 `id/Id`，省略 prefix 会使用类名小写加 `/`；长期保存的数据建议显式固定 prefix。

| 语言 | 常用源码声明 | 自动推断 |
| --- | --- | --- |
| C | int32_t/int64_t、uint32_t/uint64_t、float/double、fqdb_text/fqdb_bytes | 固定宽度数字、UTF-8 文本、字节视图 |
| C++ | 固定宽度整数、bool、float/double、std::string、vector<uint8_t> | 相应 protobuf 标量 |
| C# | int/long/uint/ulong、bool、float/double、string、byte[] | public 可写字段/属性；识别 FqField 编号 |
| Java | int/long、boolean、float/double、String、byte[] | public/package 字段，或 private 字段的显式 public get/is + set |
| Python | int/str/bool/float/bytes 提示 | int 默认 sint64；float 默认 double |
| Go | int32/int64/uint32/uint64、bool、float32/float64、string、[]byte | 固定宽度标量，模型在可导入包中 |
| Rust | i32/i64/u32/u64、bool、f32/f64、String、Vec<u8> | pub 字段，原类需 Default |
| JS/TS | number/bigint/string/boolean/Uint8Array | number 为 double；bigint 为 sint64；number 键要求安全整数 |

Python 普通类和 JavaScript 类可从明确默认值推断。例如 `score=0`、`name=""`、`enabled=True/true`；Python 推荐 dataclass 加类型提示，JavaScript 推荐 TypeScript。`None/null`、未知类型别名、动态表达式、混合类型、集合、嵌套对象不会被猜成某种格式。错误包含文件、行号和字段名。

目前支持扁平可写实体。继承、泛型实体、复杂预处理、任意装饰器/注解生成字段、私有 getter-only 对象、构造函数要求额外上下文、TS 路径别名等复杂语义需要显式适配；最终由目标语言编译器检查生成代码与业务类是否匹配。生成器不会执行业务模块或处理器。

## 字段编号、忽略和升级

`FengquDB.schema.lock.json` 自动保存字段名、类型和编号，**应随业务源码提交版本控制**。不要把它放在会被 Clean 删除的临时目录，不要手改。新增字段得到新编号；调整源码顺序不改变旧编号；删除字段后保留编号，不会分给别的字段。

工具生成的 `Field_*`、`Fields`、`fields` 或 `FIELD_*` 常量可直接用于条件查询。中间模型在 `Generated/model/<语言>/*.fqmodel.json`，也能作为单份 schema 输入给共享模型流程。

```java
// fqdb:ignore
public Object runtimeCache; // 不持久化的业务成员要显式排除

// fqdb:field number=3
public int grade;          // 从旧 score 重命名时保留其原编号
```

重命名不能靠“看起来像”自动判断。若要继承旧字段的数据，在代码中明确使用原编号；否则新字段就是新字段。类型改变、主键或前缀改变会报错，需要新字段/新实体或数据迁移。普通方法、静态成员、Java transient 不参与持久化。

动态数字需要固定 wire 类型时可以在代码上写 `fqdb:field type=sint32` 等受支持的覆盖。它主要用于 Python int、JS number/bigint 的跨语言互通，工具检查兼容性，运行时检查范围，不会截断超范围数字。

`FengquDB.codegen.local.json` 是自动记录的**本机生成配置**，无需填写，通常加入 gitignore；批处理重放它。编号锁与本机配置用途不同，不能把编号锁也忽略掉。

已有生成结果而编号锁丢失时，工具会拒绝重新分配编号，需先从版本控制恢复。删除最后一个实体会移除相应语言失效的工具输出，但编号历史继续保留。`.fqdb-codegen.lock` 由操作系统管理并在进程结束时释放；POSIX 上保留的空文件不是“仍有人持锁”，也可加入 gitignore。

## 原实体和未知字段

新对象直接传 codec 编码，不构造另一份 DTO。解码返回一个记录，内含原实体及未知字段信息，例如 Java `record.value`、C# `record.Value`、Python `row.value`。读改写时通过记录的 Encode/fqEncode/fq_encode 编码，保留新版本添加、当前版本尚不理解的字段。

C++/Go/Rust 的记录将原实体内联保存。Java/C#/Python/JS 使用轻量记录对象，避免为了兼容未知字段去修改用户类或维护全局弱引用缓存。普通“返回新数组”的便利方法有分配，热点可使用 caller buffer/Vec/slice/Span 复用；生成路径不使用运行时反射或 JSON 中转。

复用的编码输出不能与实体的字节字段、原解码输入或未知字段缓冲重叠；编码期间不要并发修改实体。编码失败后输出可能已部分写入，不能把它当作有效载荷存库，应修正输入并重新编码。

## 接入构建

| 工程 | 接入文件/方式 | 自动发生的时机 |
| --- | --- | --- |
| C++ CMake | `cmake/FengquDBModels.cmake`，`fengqudb_generate_models(SOURCE ... OUTPUT ... COMPILER ... TARGET ...)`，业务目标添加依赖 | 编译业务目标之前 |
| C#/.NET | 导入 `bindings/csharp/build/FengquDB.Models.targets`，设置 FqdbEntitySources、FqdbCodegenExecutable | 正常 CoreCompile 之前；已有 Roslyn 流程继续可用 |
| Java Gradle | apply `integrations/gradle/fengqudb-models.gradle` | compileJava 之前 |
| Java Maven | 合并 `integrations/maven/fengqudb-models.xml` | generate-sources |
| Rust | 合并 `integrations/rust/build.rs`，include OUT_DIR 生成入口 | cargo build 的 build script |
| Node/TS | 复制 `integrations/node/fqdb-generate.cjs` 到项目根目录，合并 package.scripts.json，采用匹配的 allowJs/目录布局 | npm run build 的 prebuild |
| Python 打包 | 合并 `integrations/python/build_py.py` | setuptools 的 build_py；交互开发也可手动运行 |
| Go | 提供 `integrations/go/build.sh` / build.bat，或工程 Make/IDE prebuild | 使用所给构建入口时先生成再 go build |

Go 的直接 go build、直接 tsc、Python 普通解释运行没有这里的通用前置编译钩子，不能声称会自动执行。手动双击入口始终保留。C 的 Make/IDE 工程同样可在编译前执行一次原生命令。

C# 原有 `[FqEntity]` 分析器随编译更新，不依赖本原生源码扫描；使用原生兜底时需要在类前加源码标记。带 FqField 特性时按它们选择持久化成员，避免把未标记业务字段写进去。

## 失败和部署

类型/模式检查失败会在发布前停止；预留的字段编号可能已写入锁，以防重跑复用编号。本机路径配置可以在失败前记录下来，便于兜底重放。每个文件原子替换，整个文件集合不保证跨文件原子；I/O 错误可能只更新部分输出，修复后重跑，构建必须等待命令成功后再编译。

`--check` 不写文件，0 表示当前、2 表示过期/缺失、1 表示错误。未变化的内容不改时间。失效的工具输出按清单清理，不覆盖无生成标记文件。别手改带生成标记的文件。

生成器只放开发机/构建机，移动端与业务发布包不需要带它、脚本、文档或编号配置。Windows/Linux/macOS 分别使用各自主机可执行文件；Mac 的本地执行权限与具体平台原生库仍由宿主发布流程处理。
