# C# 接入

[English](../en/languages/csharp.md)

[API、用途和调用例子](../api/csharp.md)

## 1. 放入 SDK

把 ZIP 中的 `FengquDB` 文件夹复制到业务 `.csproj` 同级。在 `.csproj` 的 `</Project>` 前加入：

```xml
<Import Project="FengquDB/FengquDB.targets" />
```

## 2. 建立统一入口

同一数据库由一个长期存活的 Store 服务各业务模块，入口可使用静态类或单例服务。新建 `AppDb.cs`：

```csharp
using System;
using System.IO;
using FengquDB;

public static class AppDb {
    private static readonly Lazy<Store> main = new Lazy<Store>(() => Open("main"));
    public static Store Main => main.Value;

    private static Store Open(string name) {
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MyApp", "FengquDB");
        Directory.CreateDirectory(root);
        return new Store(Path.Combine(root, name));
    }

    public static void Close() {
        if (main.IsValueCreated) main.Value.Dispose();
    }
}
```

默认存档位于本机应用数据目录的 `MyApp/FengquDB/main`，Store 在首次调用时创建。宿主退出时统一调用 AppDb.Close。

## 3. 存取整数和字符串

在程序入口写入：

```csharp
using System;

try {
    AppDb.Main.Put("score", 95);
    AppDb.Main.Put("name", "张三");
    int score = AppDb.Main.Get<int>("score");
    string name = AppDb.Main.Get<string>("name");
    Console.WriteLine(name + " " + score);
} finally {
    AppDb.Close();
}
```

运行项目，输出 `张三 95`。其他业务方法直接使用 `AppDb.Main`。

## 4. 保存自己的对象

新建 `Student.cs`：

```csharp
using FengquDB.Entities;

// fqdb:entity prefix=student/ key=Id
[FqEntity("student/")]
public partial class Student {
    [FqKey, FqField(1)] public int Id { get; set; }
    [FqField(2)] public string Name { get; set; } = "";
    [FqField(3)] public int Score { get; set; }
}
```

编译后，在业务方法中调用：

```csharp
AppDb.Main.Put(new Student { Id = 7, Name = "张三", Score = 95 });
Student saved = AppDb.Main.GetEntity<Student>(7);
```

实体修改后重新编译，序列化代码随之更新。已有字段编号保持固定，新字段使用新编号。

## 手动生成（按需）

Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击同目录 `fqdb-generate.command`；Linux 执行 `sh FengquDB/fqdb-generate.command`。

成功显示 `Entity codecs are up to date.`，代码生成到项目 `Generated/csharp`。字段编号锁 `FengquDB.schema.lock.json` 随业务代码保存。手动工具生成的独立 codec 用法：


```csharp
var student = new Student { Id = 7, Name = "张三", Score = 95 };
byte[] key = StudentFqCodec.Key(student);
AppDb.Main.Put(key, StudentFqCodec.Encode(student));
Student saved = StudentFqCodec.Decode(AppDb.Main.Get(key)).Value;
```

## 可选设置

前面的接入示例已包含默认配置。需要调整时，按下面对应项修改。

### 更换存档目录

修改 AppDb.Open 中的 root，所有业务调用继续使用 AppDb.Main。

### 增加独立数据库

在 AppDb 类中添加世界库入口：

```csharp
private static readonly Lazy<Store> world = new Lazy<Store>(() => Open("world"));
public static Store World => world.Value;
```

业务通过 `AppDb.World` 访问。在 AppDb.Close 中加入释放语句：

```csharp
if (world.IsValueCreated) world.Value.Dispose();
```

### 修改写入模式、周期与阈值

新库默认使用性能模式：内存生效后返回，按 50 ms / 1 MiB 刷盘；崩溃或断电可能丢失尚未落盘的数据。可靠模式等落盘成功后返回。配置按库保存，重新打开时沿用已保存值。

需要切到可靠模式时执行：

```csharp
var options = AppDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Reliable;
AppDb.Main.ConfigureWrite(options);
```

调整性能模式参数：

```csharp
var options = AppDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Performance;
options.PeriodMs = 50;
options.ThresholdBytes = 1024 * 1024;
AppDb.Main.ConfigureWrite(options);
```

恢复默认配置：`AppDb.Main.ResetWriteOptions();`。
