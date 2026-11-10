#!/bin/sh
# Validate Store, the wire codec, ownership and diagnostics under ASan/UBSan.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$root/build/linux-sanitizers"}
cmake -S "$root" -B "$output" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFQDB_SHARED=OFF -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=ON \
    -DFQDB_RELEASE_IPO=OFF -DBUILD_TESTING=ON \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
    '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g1' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -no-pie'
cmake --build "$output" --parallel 2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$output" --output-on-failure
