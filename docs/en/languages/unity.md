# Unity getting started

[简体中文](../../languages/unity.md)

[API and examples](../api/unity.md)

## 1. Add the SDK

Drag the `FengquDB` folder from the ZIP into `Assets/Plugins`, preserving `.meta` files:

```text
Assets/Plugins/FengquDB/
  Runtime/
  Native/
  FengquDB.Generator.dll
  Editor/Tools/
  fqdb-generate.bat
  fqdb-generate.command
  GettingStarted.md
  接入.md
  API.zh-CN.md
  API.md
```

When building a Player, Unity uses the included .meta settings to select the target platform's native library, and includes the managed and generated code used by your application. The generator, Editor/Tools, manual scripts, and documentation remain in the development project. The name Runtime alone does not determine what is packaged.

## 2. Create a shared entry point

Create `Assets/GameDb.cs`. It opens the database at startup and closes it at shutdown. All scenes and components share GameDb.Main:

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

The default data directory is `Application.persistentDataPath/FengquDB/main`.

## 3. Call the database

Create `Assets/FengquDbDemo.cs`:

```csharp
using UnityEngine;

public class FengquDbDemo : MonoBehaviour {
    void Start() {
        GameDb.Main.Put("score", 95);
        GameDb.Main.Put("name", "Alice");
        int score = GameDb.Main.Get<int>("score");
        string name = GameDb.Main.Get<string>("name");
        Debug.Log(name + " " + score);
    }
}
```

Create an empty GameObject, attach FengquDbDemo, and click Play. The Console prints `Alice 95`. Other components use GameDb.Main too; GameDb manages its lifetime.

## 4. Save your own objects

Create `Assets/Student.cs`:

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

Save and wait for Unity to finish compiling, then call this from your application code:

```csharp
GameDb.Main.Put(new Student { Id = 7, Name = "Alice", Score = 95 });
Student saved = GameDb.Main.GetEntity<Student>(7);
```

After editing an entity and saving the script, compilation updates its serialization code. Keep existing field numbers fixed and use new numbers for new fields.

## Manual generation (optional)

On Windows, double-click `Assets/Plugins/FengquDB/fqdb-generate.bat` in File Explorer; on macOS, double-click `fqdb-generate.command` in the same folder.

Success prints `Entity codecs are up to date.` Generated code is placed in `Assets/Generated/csharp`. Keep `FengquDB.schema.lock.json` in the project root with your application sources. To use the manually generated codec:

```csharp
var student = new Student { Id = 7, Name = "Alice", Score = 95 };
byte[] key = StudentFqCodec.Key(student);
GameDb.Main.Put(key, StudentFqCodec.Encode(student));
Student saved = StudentFqCodec.Decode(GameDb.Main.Get(key)).Value;
```

## Optional settings

The examples above use the default settings. Make the relevant changes below when needed.

### Change the data directory

Change root in GameDb.Open; application code continues to use GameDb.Main.

### Add an independent database

Add a world database entry point to GameDb:

```csharp
private static Lazy<Store> world = new Lazy<Store>(() => Open("world"));
public static Store World => world.Value;
```

After Close(); in Initialize, add:

```csharp
world = new Lazy<Store>(() => Open("world"));
```

In Close, add:

```csharp
if (world.IsValueCreated) world.Value.Dispose();
```

Access it through `GameDb.World`.

### Change the write mode, period, and threshold

New databases default to performance mode: writes return after applying changes in memory and flush at 50 ms / 1 MiB. Crashes or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Settings are saved per database and reused when it is reopened.

To switch to reliable mode:

```csharp
var options = GameDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Reliable;
GameDb.Main.ConfigureWrite(options);
```

To adjust performance-mode settings:

```csharp
var options = GameDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Performance;
options.PeriodMs = 50;
options.ThresholdBytes = 1024 * 1024;
GameDb.Main.ConfigureWrite(options);
```

Restore the defaults with `GameDb.Main.ResetWriteOptions();`.

If an application asmdef enables Override References, select FengquDB.Managed.dll under Precompiled References.
