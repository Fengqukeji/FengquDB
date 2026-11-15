# Apple 原生库接入

[English](../en/languages/apple-native.md) · [C API 与例子](../api/c.md)

Unity 用户使用完整 Unity SDK，按 [Unity 接入](unity.md) 操作。本原生库包供直接链接 C/C++ 核心的应用使用。

## 1. 选择原生库

解压 `FengquDB-Apple-native-1.0.0.zip`：

| 目标 | 原生库 |
| --- | --- |
| macOS Apple Silicon | `macos/arm64/libfengqudb.dylib` |
| macOS Intel | `macos/x86_64/libfengqudb.dylib` |
| iOS arm64 真机 | `iphoneos/arm64/libfengqudb.a` |
| iOS arm64 模拟器 | `iphonesimulator/arm64/libfengqudb.a` |

`include` 提供 C、C++ 公共头文件。macOS 的 `libfengqudb.2.dylib` 是库安装名对应的文件，应与运行库一起保留。真机与模拟器库分别用于对应的构建目标。

## 2. 保存并读取字符串

在解压后的 `FengquDB-Apple` 文件夹旁保存 `main.c`：

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
    status = fqdb_store_put(db, bytes("name"), bytes("Alice"), 0, &info, 1);
    if (status == FQDB_OK) status = fqdb_store_get(db, bytes("name"), &value);
    if (status == FQDB_OK) printf("%.*s\n", (int)value.size, (const char *)value.data);
    fqdb_buffer_free(&value);
    fqdb_store_close(db);
    return (int)status;
}
```

在 Apple Silicon Mac 的项目目录执行，Intel Mac 将 `arm64` 改为 `x86_64`：

```sh
clang main.c -IFengquDB-Apple/include -LFengquDB-Apple/macos/arm64 \
  -lfengqudb -Wl,-rpath,@executable_path/FengquDB-Apple/macos/arm64 -o demo
./demo
```

输出 `Alice`，存档位于 `save/main`。各业务模块复用同一数据库句柄，应用退出时关闭。

iOS Xcode 工程将 `include` 加入 Header Search Paths，定义 `FQDB_STATIC`，链接对应真机或模拟器的 `.a` 与 `libc++.tbd`。向 `fqdb_store_open_at` 传入应用容器中的可写目录，替换相对路径 `save`。

## 3. 可选设置与实体生成

同一可写根目录下使用 `main`、`world` 等不同名称，就是独立数据库。新库默认性能模式（50 ms / 1 MiB），内存生效后可读取，崩溃或断电可能丢失尚未落盘的数据。关键写入按上例将最后的可靠标志传 1。`fqdb_store_write_configure` 按库设置并保存配置，传 NULL 恢复默认值。完整参数与例子见 C API 文档。

实体生成工具：Apple Silicon 使用 `Tools/arm64/fqdb-codegen`，Intel 使用 `Tools/x86_64/fqdb-codegen`。在业务项目根目录运行：

```sh
./FengquDB-Apple/Tools/arm64/fqdb-codegen generate \
  --source . --source-language c --output Generated --schema-lock FengquDB.schema.lock.json
```

生成编解码调用、事务、查询、备份及资源释放方式见 [C API](../api/c.md)。
