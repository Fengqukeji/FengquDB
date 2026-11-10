FROM fqdb-final-bench-base:20261107
COPY CMakeLists.txt /src/CMakeLists.txt
COPY cmake /src/cmake
COPY src /src/src
COPY include /src/include
COPY tools/final_benchmark /src/tools/final_benchmark
RUN python3 -c 'import pathlib,hashlib,json; p=pathlib.Path("/src"); files=[f for f in p.rglob("*") if f.is_file()]; pathlib.Path("/opt/bench/source-manifest.json").write_text(json.dumps({f.relative_to(p).as_posix():hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(files)},indent=2))'
RUN cmake -S /src/tools/final_benchmark -B /build -DCMAKE_BUILD_TYPE=Release -DFQDB_SOURCE=/src && \
    cmake --build /build --parallel 2 && \
    cp /build/fqdb_final_bench /opt/bench/bin/ && \
    cp -P /build/core/libfengqudb.so* /opt/bench/lib/ && \
    strip /opt/bench/bin/fqdb_final_bench /opt/bench/lib/libfengqudb.so.0.3.2
COPY tools/final_benchmark/runner.py /opt/bench/runner.py
ENTRYPOINT ["python3", "/opt/bench/runner.py"]
