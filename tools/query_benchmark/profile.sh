#!/bin/sh
# 在 fqdb-query-dev 容器内构建带帧指针的查询基准，并逐个负载用 perf 采样热点。
# 用法：docker run --rm --privileged -v <repo>:/src:ro fqdb-query-dev:2 sh /src/tools/query_benchmark/profile.sh [负载...]
set -e
workloads=${*:-"topn_same_params topn_varying_params index_join"}
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" > /tmp/cfg.log 2>&1 || { tail -n 20 /tmp/cfg.log; exit 1; }
cmake --build /build -j 16 --target fqdb_store_select_bench > /tmp/build.log 2>&1 || { grep error -A3 /tmp/build.log | head -n 30; exit 1; }
for w in $workloads; do
    echo "=== $w"
    FQDB_BENCH_ONLY="fengqudb:$w" perf record -q -F 2999 -g -o "/tmp/$w.data" \
        /build/fqdb_store_select_bench "/tmp/db_$w" 50000 8 2>/dev/null | grep workload || true
    echo "--- self"
    perf report -i "/tmp/$w.data" --no-children --stdio --percent-limit 1.2 -g none --sort symbol 2>/dev/null |
        grep -v '^#' | grep -v '^$' | cut -c1-150 | head -n 28
    echo "--- inclusive"
    perf report -i "/tmp/$w.data" --children --stdio --percent-limit 4 -g none --sort symbol 2>/dev/null |
        grep -v '^#' | grep -v '^$' | grep fengqudb | cut -c1-150 | head -n 40
done
