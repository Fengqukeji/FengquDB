# Apple native libraries: getting started

[简体中文](../../languages/apple-native.md) · [C API and examples](../api/c.md)

For Unity, use the complete Unity SDK and follow the [Unity guide](unity.md). This native-library bundle is for applications linking the C/C++ core directly.

## 1. Choose the library

Extract `FengquDB-Apple-native-1.0.0.zip`:

| Target | Library |
| --- | --- |
| macOS Apple Silicon | `macos/arm64/libfengqudb.dylib` |
| macOS Intel | `macos/x86_64/libfengqudb.dylib` |
| iOS arm64 device | `iphoneos/arm64/libfengqudb.a` |
| iOS arm64 simulator | `iphonesimulator/arm64/libfengqudb.a` |

The `include` folder provides the C and C++ public headers. Keep `libfengqudb.2.dylib` beside the macOS runtime: it is the filename recorded in the library's install name. Device and simulator libraries are separate build targets.

## 2. Save and read a string

Save as `main.c` beside the extracted `FengquDB-Apple` folder:

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

On an Apple Silicon Mac, run from the project directory (use `x86_64` instead of `arm64` on Intel):

```sh
clang main.c -IFengquDB-Apple/include -LFengquDB-Apple/macos/arm64 \
  -lfengqudb -Wl,-rpath,@executable_path/FengquDB-Apple/macos/arm64 -o demo
./demo
```

The output is `Alice`; data is stored in `save/main`. Reuse one open database handle across your application modules and close it at shutdown.

In an iOS Xcode target, add `include` to Header Search Paths, define `FQDB_STATIC`, and link the matching device/simulator `.a` and `libc++.tbd`. Pass a writable application-container directory to `fqdb_store_open_at` instead of the relative `save` path.

## 3. Optional settings and entities

Different names under the same writable root, such as `main` and `world`, create independent databases. New databases default to performance mode (50 ms / 1 MiB): writes become readable in memory before they are durable, so a crash or power loss can lose unflushed changes. Pass the final reliability flag as 1, as above, for critical writes. Use `fqdb_store_write_configure` to save per-database settings; passing NULL restores defaults. The C API guide includes all parameters and examples.

For entity generation, run `Tools/arm64/fqdb-codegen` on Apple Silicon or `Tools/x86_64/fqdb-codegen` on Intel. From your application project root:

```sh
./FengquDB-Apple/Tools/arm64/fqdb-codegen generate \
  --source . --source-language c --output Generated --schema-lock FengquDB.schema.lock.json
```

See the [C API](../api/c.md) for generated codec calls, transactions, queries, backups, and resource lifetimes.
