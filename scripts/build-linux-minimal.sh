#!/bin/sh
# Direct GCC Store build when CMake is unavailable.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$root/build/linux-minimal"}
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
cxx=${CXX:-g++}
cc=${CC:-gcc}
for source in common logging store; do
    "$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden -DFQDB_BUILD -I"$root/include" -pthread -c "$root/src/$source.cpp" -o "$output/$source.o"
done
"$cxx" -shared -pthread "$output/common.o" "$output/logging.o" "$output/store.o" -Wl,-soname,libfengqudb.so.1 -Wl,--version-script="$root/src/fengqudb.map" -o "$output/libfengqudb.so.1"
ln -sf libfengqudb.so.1 "$output/libfengqudb.so"
ar rcs "$output/libfengqudb.a" "$output/common.o" "$output/logging.o" "$output/store.o"
"$cc" -std=c11 -I"$root/include" "$root/tests/abi_smoke.c" -L"$output" -lfengqudb '-Wl,-rpath,$ORIGIN' -o "$output/fqdb_abi_smoke"
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/include" -pthread "$root/tests/store_sdk_tests.cpp" "$root/tests/store_c_header.c" -L"$output" -lfengqudb '-Wl,-rpath,$ORIGIN' -o "$output/fqdb_store_sdk_tests"
"$output/fqdb_abi_smoke"
"$output/fqdb_store_sdk_tests"
"$cxx" --version | head -n 1
ls -l "$output/libfengqudb.so.1" "$output/libfengqudb.a"
