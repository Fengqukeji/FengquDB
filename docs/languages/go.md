# Go 接入

[English](../en/languages/go.md)

[API、用途和调用例子](../api/go.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 引用随包模块

使用项目已有的 Go/cgo 工具链；生成工具已随包提供。SDK 解压为项目根目录的 `FengquDB`。在已有 go.mod 中加入：

```go
require fengqudb v0.0.0
replace fengqudb => ./FengquDB
```

v0.0.0 是本地模块引用占位值。目录如下：

```text
项目/
  go.mod
  main.go
  models/student.go
  FengquDB/
  FengquDB/Tools/fqdb-codegen.exe            ← Windows 工具
```

## 2. 先跑通最小存取

`main.go`：

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

在项目目录运行 `FengquDB\run.bat .`（Windows）或 `sh FengquDB/run.sh .`（macOS/Linux），看到 Alice 即成功。脚本自动设置当前进程的原生库位置，使用包内的预编译引擎。

## 3. 从自己的结构体生成

`models/student.go`，模型使用独立的可导入 package：

```go
package models
// fqdb:entity prefix=student/ key=Id
type Student struct { Id int64; Name string; Score int32 }
```

按下节双击生成，得到 `Generated/go/models`。在 main.go 的 import 中加入以下两行，将 `你的module` 替换成 go.mod 中 module 的实际内容：

```go
"你的module/models"
fqmodels "你的module/Generated/go/models"
```

把下面代码放入 run 的 Store 打开后：

```go
student := models.Student{Id:7, Name:"Alice", Score:95}
key, err := fqmodels.StudentFqKey(&student); if err != nil { return err }
value, err := fqmodels.StudentFqEncode(&student); if err != nil { return err }
if err := st.Put(key, value); err != nil { return err }
```

继续使用 run.bat/run.sh，启动前会自动更新实体代码。发布时从项目根目录运行 `FengquDB\build.bat .`（Windows）或 `sh FengquDB/build.sh .`（macOS/Linux），生成代码、可执行文件及所需运行库会一起准备好，发布内容在 bin 目录。字段使用 int32/int64 等固定宽度类型。

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

根目录由项目设置，使用可写的存档目录。不同子目录就是不同数据库：

```go
import "path/filepath"

root := "save" // 换成宿主提供的存档目录
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
// players.ResetWriteOptions() // 恢复默认性能模式、50 ms / 1 MiB；检查返回的 error
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。数据和配置按库独立保存，配置重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。

发布 bin 目录中的可执行文件和数据库运行库。
