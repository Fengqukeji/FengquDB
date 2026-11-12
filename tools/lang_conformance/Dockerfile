# 多语言绑定开发/验证镜像：Ubuntu 基础镜像（含 cmake/g++）+ Go + Rust + Node 工具链。
# 工具链二进制来自各自官方镜像；它们链接较旧的 glibc，可在较新的 Ubuntu 上运行。
FROM golang:1.23-bookworm AS go
FROM rust:1-bookworm AS rust
FROM node:24-bookworm AS node
FROM fqdb-final-bench-base:20261107
COPY --from=go /usr/local/go /usr/local/go
COPY --from=rust /usr/local/cargo /usr/local/cargo
COPY --from=rust /usr/local/rustup /usr/local/rustup
COPY --from=node /usr/local/bin/node /usr/local/bin/node
COPY --from=node /usr/local/lib/node_modules /usr/local/lib/node_modules
COPY --from=node /usr/local/include/node /usr/local/include/node
ENV PATH=/usr/local/go/bin:/usr/local/cargo/bin:$PATH \
    RUSTUP_HOME=/usr/local/rustup CARGO_HOME=/usr/local/cargo \
    GOFLAGS=-mod=mod GOPATH=/tmp/gopath GOCACHE=/tmp/gocache \
    CARGO_TARGET_DIR=/tmp/cargo-target
RUN ln -sf /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm && \
    ln -sf /usr/local/lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx
