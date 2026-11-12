# 查询开发与基准环境：Linux 构建、ctest、SQLite 对照及 perf 剖析。
# 源码在运行时只读挂载到 /src，构建产物留在容器内 /build，避免与 Windows 构建目录冲突。
# perf 需要以 --privileged 运行容器。JDK 用于编译 JNI 适配库并运行 Java 绑定测试；
# 用完整 JDK 而不是 headless，因为 CMake FindJNI 默认要求 AWT 库。
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 libsqlite3-dev sqlite3 ca-certificates \
    linux-tools-generic openjdk-17-jdk && \
    ln -sf "$(ls -d /usr/lib/linux-tools/*/ | head -n 1)perf" /usr/local/bin/perf && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /build
