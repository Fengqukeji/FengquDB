# Unity 接入

[English](../en/languages/unity.md)

[API、用途和调用例子](../api/unity.md)

## 1. 放入 SDK

把 ZIP 中的 `FengquDB` 文件夹拖到 `Assets/Plugins`，保留 `.meta`：

```text
Assets/Plugins/FengquDB/
  Runtime/
  Native/
  FengquDB.Generator.dll
  Editor/Tools/
  fqdb-generate.bat
  fqdb-generate.command
  GettingStarted.md
  API.md
  接入.md
  API.zh-CN.md
```

发布时 Unity 根据随包 .meta 选择当前平台的原生库，并编入业务用到的托管代码和生成代码。生成器、Editor/Tools、手动脚本与文档留在开发工程中。Runtime 这个目录名本身不决定打包范围。

## 2. 建立统一入口

新建 `Assets/GameDb.cs`。它负责在游戏启动时打开数据库、退出时关闭，所有场景和组件共享 GameDb.Main：

```csharp
using System;
using System.IO;
using FengquDB;
using UnityEngine;

public static class GameDb {
    private static Lazy<Store> main = new Lazy<Store>(() => Open("main"));
    public static Store Main => main.Value;

    private static Store Open(string name) {
        string root = Path.Combine(Application.persistentDataPath, "FengquDB");
        Directory.CreateDirectory(root);
        return new Store(Path.Combine(root, name));
    }

    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
    private static void Initialize() {
        Close();
        main = new Lazy<Store>(() => Open("main"));
        _ = Main;
        Application.quitting -= Close;
        Application.quitting += Close;
    }

    public static void Close() {
        if (main.IsValueCreated) main.Value.Dispose();
    }

#if UNITY_EDITOR
    [UnityEditor.InitializeOnLoadMethod]
    private static void WatchReload() {
        UnityEditor.AssemblyReloadEvents.beforeAssemblyReload -= Close;
        UnityEditor.AssemblyReloadEvents.beforeAssemblyReload += Close;
    }
#endif
}
```

默认存档目录为 `Application.persistentDataPath/FengquDB/main`。

## 3. 调用数据库

新建 `Assets/FengquDbDemo.cs`：

```csharp
using UnityEngine;

public class FengquDbDemo : MonoBehaviour {
    void Start() {
        GameDb.Main.Put("score", 95);
        GameDb.Main.Put("name", "张三");
        int score = GameDb.Main.Get<int>("score");
        string name = GameDb.Main.Get<string>("name");
        Debug.Log(name + " " + score);
    }
}
```

创建空物体并挂上 FengquDbDemo，点击 Play，Console 显示 `张三 95`。其他组件同样通过 GameDb.Main 读写，生命周期由 GameDb 统一管理。

## 4. 保存自己的对象

新建 `Assets/Student.cs`：

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

保存并等待 Unity 编译完成，在业务方法中调用：

```csharp
GameDb.Main.Put(new Student { Id = 7, Name = "张三", Score = 95 });
Student saved = GameDb.Main.GetEntity<Student>(7);
```

以后修改实体并保存脚本，序列化代码随编译更新。已有字段编号保持固定，新字段使用新编号。

## 手动生成（按需）

Windows 在资源管理器中双击 `Assets/Plugins/FengquDB/fqdb-generate.bat`；macOS 双击同目录 `fqdb-generate.command`。

成功显示 `Entity codecs are up to date.`，代码生成到 `Assets/Generated/csharp`。根目录的 `FengquDB.schema.lock.json` 随业务代码保存。使用手动生成的 codec：

```csharp
var student = new Student { Id = 7, Name = "张三", Score = 95 };
byte[] key = StudentFqCodec.Key(student);
GameDb.Main.Put(key, StudentFqCodec.Encode(student));
Student saved = StudentFqCodec.Decode(GameDb.Main.Get(key)).Value;
```

## 可选设置

前面的接入示例已包含默认配置。需要调整时，按下面对应项修改。

### 更换存档目录

修改 GameDb.Open 中的 root，所有业务调用继续使用 GameDb.Main。

### 增加独立数据库

在 GameDb 类中添加世界库入口：

```csharp
private static Lazy<Store> world = new Lazy<Store>(() => Open("world"));
public static Store World => world.Value;
```

在 Initialize 的 Close(); 之后加入：

```csharp
world = new Lazy<Store>(() => Open("world"));
```

在 Close 中加入：

```csharp
if (world.IsValueCreated) world.Value.Dispose();
```

业务通过 `GameDb.World` 访问。

### 修改写入模式、周期与阈值

新库默认使用性能模式：内存生效后返回，按 50 ms / 1 MiB 刷盘；崩溃或断电可能丢失尚未落盘的数据。可靠模式等落盘成功后返回。配置按库保存，重新打开时沿用已保存值。

需要切到可靠模式时执行：

```csharp
var options = GameDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Reliable;
GameDb.Main.ConfigureWrite(options);
```

调整性能模式参数：

```csharp
var options = GameDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Performance;
options.PeriodMs = 50;
options.ThresholdBytes = 1024 * 1024;
GameDb.Main.ConfigureWrite(options);
```

恢复默认配置：`GameDb.Main.ResetWriteOptions();`。

业务 asmdef 开启 Override References 时，在 Precompiled References 中勾选 FengquDB.Managed.dll。
