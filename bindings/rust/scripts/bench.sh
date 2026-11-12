#!/bin/sh
# 在 fqdb-lang-dev:1 容器内运行：编译 bench_c 与 Rust 基准，依次测量并输出 JSON 行。
# 用法（宿主）见 scripts/run-bench.ps1。
# 挂载: /lang = build/lang, /src = bindings/rust, /tools = tools/lang_conformance
set -e
export LD_LIBRARY_PATH=/lang/linux/lib
export FQDB_LIB_DIR=/lang/linux/lib
SECS=${1:-2}
ROUNDS=${2:-1}
HEX=$(python3 -c "import json;print([c for c in json.load(open('/lang/conformance.json'))['cases'] if c['id']=='topn_mage_level'][0]['request'])")
export FQDB_BENCH_SELECT_HEX="$HEX"

cc -O2 -I/lang/linux/include "${BENCH_C:-/tools/bench_c.c}" -L/lang/linux/lib -lfengqudb -o /tmp/bench_c
cd /src
cargo build --release --example bench 2>&1 | tail -3
RUST_BENCH=/tmp/cargo-target/release/examples/bench

i=1
while [ "$i" -le "$ROUNDS" ]; do
  rm -rf /tmp/fqdb-bench-c /tmp/fqdb-bench-rs
  echo "# round $i: C baseline"
  /tmp/bench_c /tmp/fqdb-bench-c "$SECS"
  echo "# round $i: Rust"
  "$RUST_BENCH" /tmp/fqdb-bench-rs "$SECS" --extra
  i=$((i + 1))
done
echo "# Rust heap allocations per operation (--allocs)"
rm -rf /tmp/fqdb-bench-rs
"$RUST_BENCH" /tmp/fqdb-bench-rs 1 --allocs --extra >/dev/null
