#!/bin/sh
# Targeted, unpublished paged-engine checks; never installs into a released SDK.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$root/build/paged-storage-sanitizers"}
cmake=${CMAKE:-"$root/artifacts/tooling/cmake-3.31.8-linux-x86_64/bin/cmake"}
if [ ! -x "$cmake" ]; then cmake=cmake; fi
mkdir -p "$output"
source_manifest="$output/source-$(date +%s)-$$.sha256"
find "$root/src" "$root/include" "$root/tests" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | sort -z | xargs -0 sha256sum > "$source_manifest"
case "$cmake" in
    */*) ctest="$(dirname -- "$cmake")/ctest" ;;
    *) ctest=ctest ;;
esac
"$cmake" -S "$root" -B "$output" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFQDB_SHARED=OFF -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF \
    -DFQDB_RELEASE_IPO=OFF -DBUILD_TESTING=ON \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
    '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g1' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -no-pie'
"$cmake" --build "$output" --target fqdb_paged_io_tests fqdb_paged_storage_tests fqdb_store_tests fqdb_store_sdk_tests fqdb_store_maintenance_tests fqdb_store_expiry_tests fqdb_store_features_tests fqdb_store_operations_tests fqdb_store_limits_tests fqdb_store_query_tests fqdb_store_history_tests fqdb_store_watch_tests fqdb_store_io_budget_tests --parallel 2
if ! sha256sum --status -c "$source_manifest"; then
    echo "SOURCE_CHANGED: finish source edits and start a consistent sanitizer build" >&2
    exit 2
fi
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1
"$ctest" --test-dir "$output" -R '^paged_(foundation|storage|store|store_sdk|maintenance|expiry|features|operations|limits|query|history|watch|io_budget)$' --output-on-failure
