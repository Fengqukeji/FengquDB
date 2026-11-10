#!/bin/sh
# Release .so and an independently built consumer of the installed C/C++ SDK.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$root/build/store-linux-shared"}
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
cmake=${CMAKE:-"$root/artifacts/tooling/cmake-3.31.8-linux-x86_64/bin/cmake"}
if [ ! -x "$cmake" ]; then cmake=cmake; fi
case "$cmake" in */*) ctest="$(dirname -- "$cmake")/ctest" ;; *) ctest=ctest ;; esac
"$cmake" -S "$root" -B "$output" -DCMAKE_BUILD_TYPE=Release -DFQDB_SHARED=ON \
    -DFQDB_BUILD_TOOLS=ON -DFQDB_BUILD_DIAGNOSTICS=OFF -DFQDB_BUILD_JNI=OFF -DBUILD_TESTING=ON
"$cmake" --build "$output" --target fqdb_store_sdk_tests fqdb_abi_smoke fqdb_cpp_tests fqdb_store_cli --parallel 2
"$ctest" --test-dir "$output" -R '^(paged_store_sdk|shared_c_abi|cpp_api)$' --output-on-failure
"$cmake" --install "$output" --strip --prefix "$output/sdk"
mkdir -p "$output/consumer-source"
cat > "$output/consumer-source/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(StoreSdkConsumer LANGUAGES C CXX)
find_package(FengquDB CONFIG REQUIRED)
add_executable(consumer "${FQDB_SRC_ROOT}/tests/store_sdk_tests.cpp" "${FQDB_SRC_ROOT}/tests/store_c_header.c")
target_link_libraries(consumer PRIVATE FengquDB::Cpp)
EOF
"$cmake" -S "$output/consumer-source" -B "$output/consumer" \
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$output/sdk" "-DFQDB_SRC_ROOT=$root"
"$cmake" --build "$output/consumer" --parallel 2
"$output/consumer/consumer"
