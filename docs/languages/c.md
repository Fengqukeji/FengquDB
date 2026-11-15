# C 接入

[API、用途和调用例子](../api/c.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 放好 SDK 并链接

下载本语言独立 ZIP，把 SDK 解压为业务项目根目录的 `FengquDB`：

```text
项目/
  main.c
  FengquDB/
    include/                    ← 头文件
    Native/fengqudb.dll          ← Windows 运行库
    Native/fengqudb.lib          ← Windows 链接库
    fqdb-generate.bat
    Tools/fqdb-codegen.exe
```

在已有 CMake 工程中加入：

```cmake
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/FengquDB")
find_package(FengquDB CONFIG REQUIRED)
add_executable(game main.c)
fengqudb_link(game)
```

正常构建即可，运行库会自动放到可执行文件旁。使用其他构建工具时，包含 FengquDB/include，链接 Native 中的库，并将运行库部署到程序旁。

## 2. 运行最小存取

将以下内容保存为 `main.c`，按自己工程的正常方式编译运行：

```c
#include <fengqudb_store.h>
#include <stdio.h>
#include <string.h>

static fqdb_slice bytes(const char *s) {
    fqdb_slice v = {s, (uint64_t)strlen(s)}; return v;
}
int main(void) {
    fqdb_store *db = NULL;
    fqdb_buffer value = {0};
    fqdb_store_atomic_info info = {0};
    fqdb_status status = fqdb_store_open_at("save", "main", NULL, &db);
    if (status != FQDB_OK) return (int)status;
    status = fqdb_store_put(db, bytes("name"), bytes("Alice"), 0, &info, 0);
    if (status == FQDB_OK) status = fqdb_store_get(db, bytes("name"), &value);
    if (status == FQDB_OK) printf("%.*s\n", (int)value.size, (const char *)value.data);
    fqdb_buffer_free(&value);
    fqdb_store_close(db);
    return (int)status;
}
```

看到 `Alice` 即接入成功，存档在运行目录的 `save/main`。正式项目传入自己的可写存档目录。只存单值时可直接存字节；整数采用业务固定编码，详细例子在 API 页。

## 3. 从结构体生成序列化代码

在项目根目录新建 `student.h`：

```c
#pragma once
#include <fengqudb_entity_types.h>
// fqdb:entity prefix=student/ key=id
struct Student { int64_t id; fqdb_text name; int32_t score; };
```

运行下面的手动生成步骤。成功后出现 `Generated/c/fq_codecs.h`。在 main.c 顶部包含它，就能调用 `models_StudentFqCodec_encode/decode`，用法和缓冲释放方式在 API 页。

需要随构建自动更新时，把下面命令放进 IDE 的构建前事件，工作目录设置为项目根目录；Windows 给工具名加 `.exe`：

```text
FengquDB/Tools/fqdb-codegen generate --source . --source-language c --output Generated --schema-lock FengquDB.schema.lock.json
```

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

根目录由宿主设置，传入可写的存档目录。`players`、`world` 是两个独立数据库，数据和配置各自保存：

```c
const char *root = "save"; /* 换成宿主提供的存档目录 */
fqdb_store *players = NULL, *world = NULL;
fqdb_status status = fqdb_store_open_at(root, "players", NULL, &players);
if (status != FQDB_OK) return (int)status;
status = fqdb_store_open_at(root, "world", NULL, &world);
if (status != FQDB_OK) { fqdb_store_close(players); return (int)status; }

fqdb_store_write_options options = {sizeof(options), 1, FQDB_WRITE_PERFORMANCE, 50, 1048576};
status = fqdb_store_write_configure(players, &options);
if (status == FQDB_OK) {
    options.mode = FQDB_WRITE_RELIABLE;
    status = fqdb_store_write_configure(players, &options); /* 切换为可靠模式 */
}
/* fqdb_store_write_configure(players, NULL); 恢复默认性能模式、50 ms / 1 MiB */
fqdb_store_close(world);
fqdb_store_close(players);
if (status != FQDB_OK) return (int)status;
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。配置按库立即生效并保存，重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。

发布内容为 exe 和本平台数据库运行库。
