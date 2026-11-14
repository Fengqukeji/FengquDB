FROM python:3.12-slim-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF -DBUILD_TESTING=OFF \
    && cmake --build /build --target fengqudb -j 2

FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home wiki && mkdir /data && chown wiki:wiki /data
WORKDIR /app
COPY --from=builder /src/llmwiki /app/llmwiki
COPY --from=builder /src/bindings/python/fengqudb.py /app/fengqudb.py
COPY --from=builder /build/libfengqudb.so /app/Native/libfengqudb.so
ENV FENGQUDB_LIBRARY=/app/Native/libfengqudb.so PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
USER wiki
EXPOSE 8098
VOLUME /data
HEALTHCHECK --interval=30s --timeout=3s CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8098/health', timeout=2)"
ENTRYPOINT ["python", "-m", "llmwiki", "--db", "/data/wiki"]
CMD ["serve", "--host", "0.0.0.0"]
