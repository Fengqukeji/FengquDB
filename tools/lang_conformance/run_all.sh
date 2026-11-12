#!/bin/sh
# 一键验证 Go / Rust / Node 三个绑定：全新构建 Linux 核心库 -> 生成一致性向量 -> 分别运行各绑定测试。
# 在 fqdb-lang-dev:1 容器内运行（只读挂载仓库到 /repo）：
#   docker run --rm --entrypoint sh -v "$REPO:/repo:ro" fqdb-lang-dev:1 /repo/tools/lang_conformance/run_all.sh
# 构建产物全部在容器 /tmp 下，不写回仓库。
set -e
mkdir -p /w && cd /repo && tar --exclude=./build --exclude=./.git --exclude=./.agents \
  --exclude=./bindings/node/node_modules --exclude=./bindings/node/build --exclude=./bindings/node/dist \
  -cf - . | tar -xf - -C /w
cd /w
echo "== build core"
cmake -S . -B /b -DCMAKE_BUILD_TYPE=Release -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF \
  -DFQDB_BUILD_JNI=OFF -DBUILD_TESTING=OFF >/tmp/cfg.log 2>&1 || { tail -20 /tmp/cfg.log; exit 1; }
cmake --build /b --target fengqudb --parallel 2 >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log; exit 1; }
mkdir -p /lang/linux/lib /lang/linux/include
cp -P /b/libfengqudb.so* /lang/linux/lib/ && cp include/*.h /lang/linux/include/
export LD_LIBRARY_PATH=/lang/linux/lib FQDB_CONFORMANCE=/lang/conformance.json
echo "== conformance vectors (Python binding on the same core)"
python3 tools/lang_conformance/gen_vectors.py /lang/linux/lib/libfengqudb.so "$FQDB_CONFORMANCE"

echo "== Go"
(cd bindings/go &&
 GOCACHE=/tmp/gocache GOPATH=/tmp/gopath GOFLAGS=-buildvcs=false \
 CGO_CFLAGS=-I/lang/linux/include CGO_LDFLAGS='-L/lang/linux/lib -lfengqudb' \
 sh -c 'test -z "$(gofmt -l .)" && go vet ./... && go test -count=1 -v ./... 2>&1 | grep -E "^(--- FAIL|FAIL|ok|PASS)"')

echo "== Rust"
(cd bindings/rust && FQDB_LIB_DIR=/lang/linux/lib \
 cargo test --release 2>&1 | grep -E "^test result|FAILED|panicked")

echo "== Node"
# 默认按 package.json 声明的范围安装；TS_SPEC=latest 可验证对最新 TypeScript 的兼容性。
TS_SPEC=${TS_SPEC:-^5.6.0}
(cd bindings/node &&
 npm install --prefix /tmp/tsdev --no-audit --no-fund "typescript@$TS_SPEC" "@types/node@^24.0.0" >/dev/null 2>&1 &&
 cmake -S . -B build -DFQDB_INCLUDE_DIR=/lang/linux/include -DFQDB_LIB_DIR=/lang/linux/lib \
   -DNODE_INCLUDE_DIR=/usr/local/include/node -DCMAKE_BUILD_TYPE=Release >/tmp/ncfg.log 2>&1 &&
 cmake --build build >/tmp/nbuild.log 2>&1 &&
 /tmp/tsdev/node_modules/.bin/tsc -p tsconfig.json --typeRoots /tmp/tsdev/node_modules/@types &&
 node --test --test-reporter=tap 'dist/test/*.test.js' 2>&1 | grep -E "^# (tests|pass|fail)|^not ok")
echo "== done"
