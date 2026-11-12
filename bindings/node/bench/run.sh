#!/bin/sh
# 在 fqdb-lang-dev:1 容器内运行: 编译并运行 C 基线，再运行 Node 基准（同一台机器/同一库）。
# 需要挂载: /lang (build/lang)、/tools (tools/lang_conformance)、/src (bindings/node)，
# 环境变量 LD_LIBRARY_PATH=/lang/linux/lib、FQDB_CONFORMANCE=/lang/conformance.json。
set -e
SECONDS_PER_OP=${1:-2}
cc -O2 -I/lang/linux/include /tools/bench_c.c -L/lang/linux/lib -lfengqudb -o /tmp/bench_c
FQDB_BENCH_SELECT_HEX=$(node -e "console.log(JSON.parse(require('fs').readFileSync(process.env.FQDB_CONFORMANCE,'utf8')).cases.find(c=>c.id==='topn_mage_level').request)")
export FQDB_BENCH_SELECT_HEX
rm -rf /tmp/bench-c-db /tmp/bench-node-db
echo "== C baseline"
/tmp/bench_c /tmp/bench-c-db "$SECONDS_PER_OP"
echo "== Node"
node /src/dist/bench/bench.js /tmp/bench-node-db "$SECONDS_PER_OP"
