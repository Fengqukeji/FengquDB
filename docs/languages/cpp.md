# C++ 接入

[English](../en/languages/cpp.md)

[API、用途和调用例子](../api/cpp.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 放好文件

下载本语言独立 ZIP，把 SDK 解压为业务项目根目录的 `FengquDB`。首次接入可以新建如下工程：

```text
项目/
  CMakeLists.txt
  main.cpp
  models/student.hpp
  FengquDB/                   ← 原 SDK，工具也留在这里
```

`models/student.hpp`：

```cpp
#pragma once
#include <cstdint>
#include <string>
// fqdb:entity prefix=student/ key=id
struct Student { int64_t id=0; std::string name; int32_t score=0; };
```

## 2. 设置自动生成并链接预编译库

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.15)
project(Demo LANGUAGES CXX)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/FengquDB")
find_package(FengquDB CONFIG REQUIRED)
add_executable(game main.cpp)
fengqudb_link(game)
fengqudb_generate_models(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/models"
  OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/Generated" TARGET models)
add_dependencies(game models)
```

编译使用包内预编译库和生成工具。

## 3. 保存、读取并看到结果

`main.cpp`：

```cpp
#include <fengqudb_store.hpp>
#include <iostream>
#include "Generated/cpp/fq_codecs.hpp"

int main() {
    auto db = fengqudb::Store::open_at("save", "main");
    using Codec = fqdb_generated::models::StudentFqCodec;
    Student student{7, "Alice", 95};
    db.put(Codec::key(student), Codec::encode(student));
    auto bytes = db.get(Codec::key(student));
    auto saved = Codec::decode(bytes->view());
    std::cout << saved.value.name << " " << saved.value.score << "\n";
    db.sync();
}
```

在项目目录执行 `cmake -S . -B build`，然后 `cmake --build build --config Release`。fengqudb_link 会链接引擎，并把运行库放到可执行文件旁。运行 game，出现 `Alice 95` 即成功。存档在运行目录下的 save/main，发布时复制可执行文件和旁边的数据库运行库。

以后修改结构体并正常编译，会更新 `Generated/cpp`。只存字符串或自己的二进制时直接 `db.put("name", "Alice")`，可直接存取；其他单值例子见 API 页。

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

宿主传入可写的存档根目录，不同子目录就是不同数据库：

```cpp
std::string root = "save"; // 换成宿主提供的 UTF-8 存档目录
auto players = fengqudb::Store::open_at(root, "players");
auto world = fengqudb::Store::open_at(root, "world");

auto options = players.write_options();
options.mode = fengqudb::StoreWriteMode::Performance;
options.period_ms = 50;
options.threshold_bytes = 1ULL * 1024 * 1024;
players.configure_write(options);

options.mode = fengqudb::StoreWriteMode::Reliable;
players.configure_write(options); // 这个库改为每笔可靠落盘
// players.reset_write_options(); // 恢复默认性能模式、50 ms / 1 MiB
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。配置按库立即生效并保存，重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。

发布只带应用和目标平台原生库；发布内容为运行库与业务程序。
