#!/bin/sh
# New Store writer/readers, staged roots and shared-cache race checks.
set -eu
ulimit -c 0
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$root/build/store-tsan"}
mkdir -p "$output"
run="$(date +%s)-$$"
cxx=${CXX:-g++}
# Detect edits during a long instrumented build instead of mixing revisions.
source_manifest="$output/source-$run.sha256"
find "$root/src" "$root/include" "$root/tests" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | sort -z | xargs -0 sha256sum > "$source_manifest"
verify_sources() {
    if ! sha256sum --status -c "$source_manifest"; then
        echo "SOURCE_CHANGED: finish source edits and start a consistent TSan build" >&2
        exit 2
    fi
}
for source in common logging store; do
    "$cxx" -std=c++17 -O1 -g1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie \
        -DFQDB_STATIC -DFQDB_TESTING -I"$root/include" -I"$root/src" -pthread \
        -c "$root/src/$source.cpp" -o "$output/$source.o"
done
for test in paged_io store store_maintenance store_expiry store_features store_operations store_limits store_query store_history store_watch store_io_budget; do
    "$cxx" -std=c++17 -O1 -g1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie \
        -DFQDB_STATIC -DFQDB_TESTING -I"$root/include" -I"$root/src" -pthread \
        "$root/tests/${test}_tests.cpp" "$output/common.o" "$output/logging.o" "$output/store.o" -o "$output/$test"
    verify_sources
    attempt=0
    while :; do
        attempt=$((attempt + 1))
        log="$output/$test-$run-attempt-$attempt.log"
        if TSAN_OPTIONS=halt_on_error=1 "$output/$test" > "$log" 2>&1; then
            verify_sources
            cat "$log"
            cp "$log" "$output/$test.log"
            break
        else
            code=$?
            if [ "$attempt" -ge 20 ] || grep -q 'FQDB_TSAN_STARTED' "$log"; then cat "$log"; exit "$code"; fi
            if [ "$code" -ne 139 ] && ! grep -q '^FATAL: ThreadSanitizer: unexpected memory mapping' "$log"; then cat "$log"; exit "$code"; fi
            echo "TSan pre-entry layout failure on $test attempt $attempt; retained $log"
        fi
    done
done
