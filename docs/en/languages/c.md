# C getting started

[简体中文](../../languages/c.md)

[API and examples](../api/c.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Place the SDK and link it

Download this language's SDK ZIP and extract its `FengquDB` folder into your project root:

```text
project/
  main.c
  FengquDB/
    include/                    <- Headers
    Native/fengqudb.dll          <- Windows runtime
    Native/fengqudb.lib          <- Windows import library
    fqdb-generate.bat
    Tools/fqdb-codegen.exe
```

Add this to your existing CMake project:

```cmake
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/FengquDB")
find_package(FengquDB CONFIG REQUIRED)
add_executable(game main.c)
fengqudb_link(game)
```

Build normally. The runtime is copied beside the executable automatically. With other build tools, add FengquDB/include to the include path, link the library in Native, and deploy the runtime beside the application.

## 2. Run a minimal read and write

Save the following as `main.c`, then compile and run it using your project's normal build process:

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

The output `Alice` confirms a successful connection. Data is stored in `save/main` under the working directory. In a real application, pass a writable data directory supplied by the host. Single values can be stored as bytes; choose a fixed integer encoding. See the API page for examples.

## 3. Generate serialization code from a struct

Create `student.h` in the project root:

```c
#pragma once
#include <fengqudb_entity_types.h>
// fqdb:entity prefix=student/ key=id
struct Student { int64_t id; fqdb_text name; int32_t score; };
```

Run the manual generation steps below to create `Generated/c/fq_codecs.h`. Include it at the top of main.c to call `models_StudentFqCodec_encode/decode`. See the API page for usage and buffer release.

To update automatically during builds, put the command below in your IDE's pre-build step with the project root as the working directory. On Windows, append `.exe` to the tool name:

```text
FengquDB/Tools/fqdb-codegen generate --source . --source-language c --output Generated --schema-lock FengquDB.schema.lock.json
```

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Pass a writable data root supplied by the host. `players` and `world` are independent databases, each with its own data and settings:

```c
const char *root = "save"; /* Replace with the host's writable data directory */
fqdb_store *players = NULL, *world = NULL;
fqdb_status status = fqdb_store_open_at(root, "players", NULL, &players);
if (status != FQDB_OK) return (int)status;
status = fqdb_store_open_at(root, "world", NULL, &world);
if (status != FQDB_OK) { fqdb_store_close(players); return (int)status; }

fqdb_store_write_options options = {sizeof(options), 1, FQDB_WRITE_PERFORMANCE, 50, 1048576};
status = fqdb_store_write_configure(players, &options);
if (status == FQDB_OK) {
    options.mode = FQDB_WRITE_RELIABLE;
    status = fqdb_store_write_configure(players, &options); /* Switch this database to reliable mode */
}
/* fqdb_store_write_configure(players, NULL); Restore default performance mode, 50 ms / 1 MiB */
fqdb_store_close(world);
fqdb_store_close(players);
if (status != FQDB_OK) return (int)status;
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Settings take effect per database and are saved across reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle.

Deploy your executable and the database runtime for the target platform.
