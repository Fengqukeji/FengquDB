ARG BASE_IMAGE=fqdb-final-bench-base:20261107
FROM ${BASE_IMAGE}
COPY include /source/include
COPY tools /source/tools
COPY reference /source/artifacts/reference/yyjson-0.12.0
COPY native /opt/public/lib
RUN gcc -O3 -DNDEBUG -c /source/artifacts/reference/yyjson-0.12.0/yyjson.c -o /tmp/yyjson.o && \
    g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pthread \
    -I/source/include -I/source/artifacts/reference/yyjson-0.12.0 -I/opt/bench/include \
    /source/tools/business_benchmark/benchmark.cpp /source/tools/business_benchmark/fq.cpp \
    /source/tools/business_benchmark/sqlite.cpp /source/tools/business_benchmark/mysql.cpp \
    /source/tools/business_benchmark/redis.cpp /tmp/yyjson.o /opt/public/lib/libfengqudb.so.2 \
    /opt/bench/lib/libsqlite3_benchmark.so $(pkg-config --cflags --libs mysqlclient hiredis) \
    -Wl,-rpath,/opt/public/lib:/opt/bench/lib -o /opt/public/business_bench && \
    strip /opt/public/business_bench
ENTRYPOINT ["python3", "/source/tools/public_benchmark/container.py"]
