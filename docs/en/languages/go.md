# Go getting started

[简体中文](../../languages/go.md)

[API and examples](../api/go.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Reference the included module

Use your project's existing Go/cgo toolchain; the SDK includes the generator. Extract the SDK as `FengquDB` in the project root. Add this to your existing go.mod:

```go
require fengqudb v0.0.0
replace fengqudb => ./FengquDB
```

v0.0.0 is a placeholder for the local module reference. Directory layout:

```text
project/
  go.mod
  main.go
  models/student.go
  FengquDB/
  FengquDB/Tools/fqdb-codegen.exe            <- Windows tool
```

## 2. Run a minimal read and write

`main.go`:

```go
package main
import (
    "fmt"
    "os"
    "fengqudb"
)
func run() error {
    if err := os.MkdirAll("save", 0755); err != nil { return err }
    st, err := fengqudb.Open("save/main", nil)
    if err != nil { return err }; defer st.Close()
    if err := st.Put([]byte("name"), []byte("Alice")); err != nil { return err }
    value, err := st.Get([]byte("name")); if err != nil { return err }
    fmt.Println(string(value))
    return st.Sync()
}
func main() { if err := run(); err != nil { panic(err) } }
```

Run `FengquDB\run.bat .` on Windows or `sh FengquDB/run.sh .` on macOS/Linux from the project directory. The output `Alice` confirms success. The script sets the native library path for the process and uses the precompiled engine in the SDK.

## 3. Generate from your own struct

Create `models/student.go`. Put models in a separate, importable package:

```go
package models
// fqdb:entity prefix=student/ key=Id
type Student struct { Id int64; Name string; Score int32 }
```

Double-click the script described below to create `Generated/go/models`. Add these imports to main.go, replacing `your/module` with the module path from go.mod:

```go
"your/module/models"
fqmodels "your/module/Generated/go/models"
```

Add this after opening the Store in run:

```go
student := models.Student{Id:7, Name:"Alice", Score:95}
key, err := fqmodels.StudentFqKey(&student); if err != nil { return err }
value, err := fqmodels.StudentFqEncode(&student); if err != nil { return err }
if err := st.Put(key, value); err != nil { return err }
```

Continue using run.bat/run.sh; entity code updates automatically before startup. To publish, run `FengquDB\build.bat .` on Windows or `sh FengquDB/build.sh .` on macOS/Linux from the project root. The scripts prepare generated code, the executable, and required runtimes under bin. Use fixed-width field types such as int32/int64.

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Set a writable data directory in your application. Different subdirectories are independent databases:

```go
import "path/filepath"

root := "save" // Replace with the host's writable data directory
if err := os.MkdirAll(root, 0755); err != nil { return err }
players, err := fengqudb.Open(filepath.Join(root, "players"), nil)
if err != nil { return err }; defer players.Close()
world, err := fengqudb.Open(filepath.Join(root, "world"), nil)
if err != nil { return err }; defer world.Close()

options := fengqudb.DefaultWriteOptions()
options.Mode = fengqudb.WritePerformance
options.PeriodMs = 50
options.ThresholdBytes = 1024 * 1024
if err := players.ConfigureWrite(options); err != nil { return err }

options.Mode = fengqudb.WriteReliable
if err := players.ConfigureWrite(options); err != nil { return err }
// players.ResetWriteOptions() // Restore default performance mode, 50 ms / 1 MiB; check the returned error
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Each database stores its data and settings separately; settings survive reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle.

Deploy the executable and database runtime from bin.
