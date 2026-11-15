# 来源与第三方声明

FengquDB 自有代码采用 [Apache-2.0](LICENSE)，允许商业使用、修改和再分发。分发时保留许可证和 [NOTICE](NOTICE)；修改过的文件应注明修改。第三方内容仍按其原许可证使用。

| 内容 | 用途与范围 | 来源与许可证 |
| --- | --- | --- |
| Protocol Buffers | 独立 wire 编码实现参考其规范及官方代码的边界行为；Google runtime、protoc 不编译或链接进数据库核心 | [Encoding](https://protobuf.dev/programming-guides/encoding/)、[v36.2](https://github.com/protocolbuffers/protobuf/tree/v36.2)，最初也参考 v33.0；保留 [Google BSD-3-Clause 声明](licenses/Protobuf-BSD-3-Clause.txt) |
| JSON for Modern C++ | 原生模型生成工具、可选诊断模块；不是 Store 存储格式 | [nlohmann/json](https://github.com/nlohmann/json)，MIT；源码许可证为 `third_party/json/LICENSE.MIT`，SDK 放在 `licenses/nlohmann-json.LICENSE.MIT` |
| GCC libstdc++ / libgcc | Linux 可移植构建可能静态链接的 C++ 运行时；普通动态构建使用系统运行时 | [GCC](https://gcc.gnu.org/)，[GPL-3.0](licenses/GPL-3.0.txt) 加 [Runtime Library Exception 3.1](licenses/GCC-Runtime-Library-Exception-3.1.txt)；符合例外条件的独立程序不因此改用 GPL |
| LLVM libc++ / libc++abi | Android NDK 使用 `c++_static` 时包含的运行时 | [LLVM](https://github.com/llvm/llvm-project)，[Apache-2.0 with LLVM exception](licenses/LLVM-libcxx-Apache-2.0-with-exception.txt)；保留当前 Unity NDK r23b 的完整 [NOTICE.toolchain](licenses/Android-NDK-r23b.NOTICE.toolchain) 和 [libc++abi 许可证](licenses/Android-NDK-r23b-libcxxabi.txt)。其他构建以所用 NDK 的 NOTICE 为准 |

Windows 和 Apple 的系统开发工具、SDK 及系统运行时不属于本项目 Apache 授权的内容。构建与发布方须遵守所用工具链的条款；Apple SDK 不随本项目分发。

## 基准工具

性能对比工具使用 yyjson（MIT）、MySQL 客户端、hiredis（BSD-3-Clause）与 SQLite（public domain），并启动 MySQL、Redis 服务。它们不是 FengquDB 核心库的依赖。yyjson 的来源、哈希和许可证保存在基准复现输入中。MySQL/Redis 等服务及其客户端保留各自许可证；公开报告不重新许可这些产品，也不把测试镜像作为数据库 SDK 分发。

本项目没有将 Google 的完整 Protobuf 库改名后作为数据库。Protobuf 只规定一部分二进制编码；索引、事务、缓存、恢复、持久化和查询由 FengquDB 自身实现。性能对比衡量完整业务调用，不能单独证明某一种编码优于所有其他方案。
