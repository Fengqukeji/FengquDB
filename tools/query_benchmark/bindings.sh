#!/bin/sh
# 在 fqdb-query-dev 容器内构建核心与 JNI，运行 Python、Java 查询绑定冒烟测试。
# 用法：docker run --rm -v <repo>:/src:ro fqdb-query-dev:3 sh /src/tools/query_benchmark/bindings.sh
set -e
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DFQDB_BUILD_TOOLS=OFF \
    -DFQDB_BUILD_JNI=ON > /tmp/cfg.log 2>&1 || { tail -n 20 /tmp/cfg.log; exit 1; }
cmake --build /build -j 16 > /tmp/build.log 2>&1 || { grep -E 'error' -A3 /tmp/build.log | head -n 30; exit 1; }
echo "warnings=$(grep -c 'warning:' /tmp/build.log || true)"
rm -rf /tmp/qb && mkdir -p /tmp/qb/classes
python3 /src/tests/store_select_bindings.py /tmp/qb/db /build/libfengqudb.so
javac -encoding UTF-8 -d /tmp/qb/classes /src/bindings/java/io/fengqudb/*.java /src/tests/java/SelectSmoke.java
java -Djava.library.path=/build -cp /tmp/qb/classes SelectSmoke /tmp/qb/db/main
