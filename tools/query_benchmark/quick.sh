#!/bin/sh
# 快速 A/B：只构建查询基准，只跑指定负载，每个负载重复 N 次，输出 us_per_query。
# 用法：docker run --rm -v <repo>:/src:ro fqdb-query-dev:3 sh /src/tools/query_benchmark/quick.sh "负载1,负载2" [次数] [玩家数]
set -e
workloads=$1
rounds=${2:-3}
players=${3:-50000}
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF > /tmp/cfg.log 2>&1 || { tail -n 20 /tmp/cfg.log; exit 1; }
cmake --build /build -j 16 --target fqdb_store_select_bench > /tmp/build.log 2>&1 || { grep error -A3 /tmp/build.log | head -n 30; exit 1; }
for n in $(seq 1 "$rounds"); do
    rm -rf "/tmp/quick$n"
    FQDB_BENCH_ONLY="$workloads" /build/fqdb_store_select_bench "/tmp/quick$n" "$players" 8 |
        grep '"workload"' | sed -E 's/.*"workload":"([^"]*)".*"us_per_query":([0-9.]+).*/\1 \2/;s/.*"workload":"([^"]*)".*"qps":([0-9]+).*/\1 qps=\2/'
done
