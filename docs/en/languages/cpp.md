# C++ getting started

[简体中文](../../languages/cpp.md)

[API and examples](../api/cpp.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Place the files

Download this language's SDK ZIP and extract its `FengquDB` folder into your project root. For a first try, create this project:

```text
project/
  CMakeLists.txt
  main.cpp
  models/student.hpp
  FengquDB/                   <- The SDK, including its tools
```

`models/student.hpp`:

```cpp
#pragma once
#include <cstdint>
#include <string>
// fqdb:entity prefix=student/ key=id
struct Student { int64_t id=0; std::string name; int32_t score=0; };
```

## 2. Enable automatic generation and link the precompiled library

`CMakeLists.txt`:

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

The build uses the precompiled library and generation tool in the SDK.

## 3. Save, read, and check the output

`main.cpp`:

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

Run `cmake -S . -B build`, then `cmake --build build --config Release` in the project directory. fengqudb_link links the engine and copies its runtime beside the executable. Run game; `Alice 95` confirms success. Data is stored in save/main under the working directory. Deploy the executable and the database runtime beside it.

After changing the struct, a normal build updates `Generated/cpp`. To store just a string or your own binary data, call `db.put("name", "Alice")` directly. See the API page for other single-value examples.

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Pass a writable data root supplied by the host. Different subdirectories are independent databases:

```cpp
std::string root = "save"; // Replace with the host's UTF-8 data directory
auto players = fengqudb::Store::open_at(root, "players");
auto world = fengqudb::Store::open_at(root, "world");

auto options = players.write_options();
options.mode = fengqudb::StoreWriteMode::Performance;
options.period_ms = 50;
options.threshold_bytes = 1ULL * 1024 * 1024;
players.configure_write(options);

options.mode = fengqudb::StoreWriteMode::Reliable;
players.configure_write(options); // Require durable storage for every write to this database
// players.reset_write_options(); // Restore default performance mode, 50 ms / 1 MiB
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Settings take effect per database and are saved across reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle.

Deploy the application and the native database runtime for the target platform.
