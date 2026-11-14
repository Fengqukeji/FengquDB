#!/bin/sh
# Reproducible Linux x64 bundle build inside the already installed gcc:11 image.
# C ABI only; each DSO hides its static C++ runtime and frees its own buffers.
set -eu
task_out=${1:-build/index-portable}
mkdir -p "$task_out"
g++ -std=c++17 -O3 -DNDEBUG -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections \
    -Iinclude -Isrc -DFQDB_BUILD -pthread -shared -static-libstdc++ -static-libgcc \
    -Wl,--gc-sections,--version-script=src/fengqudb.map,-soname,libfengqudb.so.2,-s \
    src/common.cpp src/logging.cpp src/store.cpp -o "$task_out/libfengqudb.so.2"
g++ -std=c++20 -O3 -DNDEBUG -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections \
    -Iinclude -Isrc/index -DFQIDX_BUILD -pthread -shared -static-libstdc++ -static-libgcc \
    -Wl,--gc-sections,--version-script=src/index/exports.map,-soname,libfengqudb_index.so.1,-s \
    -Wl,-rpath,'$ORIGIN' src/index/index.cpp -L"$task_out" -Wl,-l:libfengqudb.so.2 \
    -o "$task_out/libfengqudb_index.so.1"
g++ -std=c++20 -O2 -Iinclude -pthread -static-libstdc++ -static-libgcc \
    tests/index/extended_tests.cpp -L"$task_out" -Wl,-l:libfengqudb_index.so.1,-l:libfengqudb.so.2 \
    -Wl,-rpath,'$ORIGIN' -o "$task_out/fqdb_index_extended_tests"
