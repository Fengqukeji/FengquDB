#!/bin/sh
# 在 fqdb-lang-dev:1 容器内运行：测试 + clippy。挂载同 bench.sh。
set -e
export LD_LIBRARY_PATH=/lang/linux/lib
export FQDB_LIB_DIR=/lang/linux/lib
export FQDB_CONFORMANCE=/lang/conformance.json
cd /src
cargo test --release "$@"
# 镜像默认没有 clippy 组件（容器是一次性的，需要联网安装）。
cargo clippy --version >/dev/null 2>&1 || rustup component add clippy
cargo clippy --release --all-targets -- -D warnings
