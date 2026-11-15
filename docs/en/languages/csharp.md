# C# getting started

[简体中文](../../languages/csharp.md)

[API and examples](../api/csharp.md)

## 1. Add the SDK

Copy the `FengquDB` folder from the ZIP beside your application's `.csproj`. Add this before `</Project>` in the `.csproj`:

```xml
<Import Project="FengquDB/FengquDB.targets" />
```

## 2. Create a shared entry point

Use one long-lived Store for the same database across application modules, exposed through a static class or singleton service. Create `AppDb.cs`:

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

The default data path is `MyApp/FengquDB/main` under the local application-data directory. The Store is created on first access. Call AppDb.Close when the application shuts down.

## 3. Store integers and strings

At your application entry point:

```csharp
using System;

try {
    AppDb.Main.Put("score", 95);
    AppDb.Main.Put("name", "Alice");
    int score = AppDb.Main.Get<int>("score");
    string name = AppDb.Main.Get<string>("name");
    Console.WriteLine(name + " " + score);
} finally {
    AppDb.Close();
}
```

Run the application; the output is `Alice 95`. Other application methods can use `AppDb.Main` directly.

## 4. Save your own objects

Create `Student.cs`:

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

After compiling, call this from your application code:

```csharp
AppDb.Main.Put(new Student { Id = 7, Name = "Alice", Score = 95 });
Student saved = AppDb.Main.GetEntity<Student>(7);
```

After changing an entity, rebuild to update the serialization code. Keep existing field numbers fixed and use new numbers for new fields.

## Manual generation (optional)

On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `fqdb-generate.command` in the same folder; on Linux, run `sh FengquDB/fqdb-generate.command`.

Success prints `Entity codecs are up to date.` Generated code is placed in `Generated/csharp`. Keep the field-number lock `FengquDB.schema.lock.json` with your application sources. Use the separate codec produced by the manual tool like this:


```csharp
var student = new Student { Id = 7, Name = "Alice", Score = 95 };
byte[] key = StudentFqCodec.Key(student);
AppDb.Main.Put(key, StudentFqCodec.Encode(student));
Student saved = StudentFqCodec.Decode(AppDb.Main.Get(key)).Value;
```

## Optional settings

The examples above use the default settings. Make the relevant changes below when needed.

### Change the data directory

Change root in AppDb.Open; application code continues to use AppDb.Main.

### Add an independent database

Add a world database entry point to AppDb:

```csharp
private static readonly Lazy<Store> world = new Lazy<Store>(() => Open("world"));
public static Store World => world.Value;
```

Access it through `AppDb.World`. Add this release statement to AppDb.Close:

```csharp
if (world.IsValueCreated) world.Value.Dispose();
```

### Change the write mode, period, and threshold

New databases default to performance mode: writes return after applying changes in memory and flush at 50 ms / 1 MiB. Crashes or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Settings are saved per database and reused when it is reopened.

To switch to reliable mode:

```csharp
var options = AppDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Reliable;
AppDb.Main.ConfigureWrite(options);
```

To adjust performance-mode settings:

```csharp
var options = AppDb.Main.WriteOptions();
options.Mode = FengquDB.StoreWriteMode.Performance;
options.PeriodMs = 50;
options.ThresholdBytes = 1024 * 1024;
AppDb.Main.ConfigureWrite(options);
```

Restore the defaults with `AppDb.Main.ResetWriteOptions();`.
