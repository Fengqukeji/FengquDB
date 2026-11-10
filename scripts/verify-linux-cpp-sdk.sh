#!/bin/sh
# Native CMake + installed/relocated shared/static C++ consumer validation.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake_bin=${1:-$(command -v cmake)}
ctest_bin=$(dirname -- "$cmake_bin")/ctest
build_root="$root/build/linux-cpp-sdk-store"
sdk="$root/artifacts/sdk-store/linux-x64"
"$cmake_bin" -S "$root" -B "$build_root/shared" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib -DFQDB_BUILD_JNI=OFF
"$cmake_bin" --build "$build_root/shared" -j 4
"$ctest_bin" --test-dir "$build_root/shared" --output-on-failure
"$cmake_bin" --install "$build_root/shared" --strip --prefix "$sdk"
"$cmake_bin" -S "$root" -B "$build_root/static" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib -DFQDB_SHARED=OFF -DFQDB_BUILD_JNI=OFF
"$cmake_bin" --build "$build_root/static" -j 4
"$ctest_bin" --test-dir "$build_root/static" -R 'paged_store_sdk|shared_c_abi|wire' --output-on-failure
"$cmake_bin" --install "$build_root/static" --prefix "$sdk/static"
relocated="$build_root/relocated-$$"
mkdir -p "$relocated"
cp -a "$sdk/." "$relocated/"
for linkage in shared static; do
    prefix="$relocated"
    if [ "$linkage" = static ]; then prefix="$relocated/static"; fi
    consumer="$build_root/consumer-$linkage-$$"
    "$cmake_bin" -S "$root/tests/cmake-consumer" -B "$consumer" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix"
    "$cmake_bin" --build "$consumer" -j 4
    "$consumer/cpp_consumer" "$consumer/example.fdb"
    "$consumer/diagnostics_consumer" "$consumer/diagnostics.fdb"
done
