#!/bin/sh
# 归因基准：同一份数据上测 FengquDB / SQLite / 内存数组的基础操作单行成本。
# 用法：docker run --rm -v <repo>:/src:ro fqdb-query-dev:3 sh /src/tools/query_benchmark/attribution.sh [行数] [值填充字节...]
set -e
rows=${1:-50000}
shift || true
pads=${*:-"32 256"}
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF > /tmp/cfg.log 2>&1 || { tail -n 20 /tmp/cfg.log; exit 1; }
cmake --build /build -j 16 --target fqdb_store_attribution_bench > /tmp/build.log 2>&1 || { grep error -A3 /tmp/build.log | head -n 30; exit 1; }
for pad in $pads; do
    rm -rf "/tmp/attr$pad"
    /build/fqdb_store_attribution_bench "/tmp/attr$pad" "$rows" "$pad"
    echo
done
