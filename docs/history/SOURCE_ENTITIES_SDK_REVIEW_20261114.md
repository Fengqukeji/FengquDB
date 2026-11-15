# 源码实体与精简 SDK 交付检查

范围：原生源码实体生成、构建钩子、八语言接入文档及客户端打包。本轮未修改 src 下的数据库引擎算法，没有重新运行数据库吞吐对比。

已修复的接入和审查问题：

- 输出锁采用操作系统文件锁，持锁进程退出或崩溃后可恢复；避免残留目录锁阻塞后续生成。
- 已有输出但编号锁缺失时拒绝重新编号；删除最后一个实体或整种语言实体时清理对应工具输出，保留编号历史和用户文件。
- 修复 Windows 批处理末尾反斜杠的参数引用问题；项目配置重放与构建选择同一输出位置。
- CMake 选择模型子目录时默认把编号锁留在业务工程根目录，根目录手动脚本可重放同一配置。
- Python src 布局可明确选择导入根；setuptools 钩子在首次生成后登记生成包，避免其未进入构建输出。
- C# SDK 示例使用随包分析器，不引用未分发的生成器工程。
- 修复源码末尾未闭合块注释的识别，移除 Rust 发射器中被立即丢弃的字符串构造。
- ZIP 在完成写入后重新打开校验；Unix 执行位独立于打包宿主写入，精简包排除 Rust 基准和开发依赖。
- API 索引修正 C# 类型声明、C++ 默认参数、模板和作用域枚举的展示。

验证证据（build 目录为本机产物，不进入客户端 SDK）：

| 验证 | 结果与记录 |
| --- | --- |
| Windows 六语言源码生成与原实体往返 | `build/source-codegen-delivery/report.json`；Python、Node、Java、C#、C、C++ |
| Linux 源码生成与原实体往返 | `build/source-codegen-linux-delivery/report.json`；Go、Rust、Python、Node、C、C++ |
| 最后源码修复后的定向回归 | `build/source-final-regressions-win/report.json`、`build/source-final-regressions-linux/report.json` |
| 编号锁丢失、删除最后实体及整语言清理 | Windows 与 Linux 的源码定向回归，预期错误与成功均核对退出码 |
| 输出锁与持锁进程终止恢复 | Windows/Linux 的 `tests/codegen/lock_verify.py` |
| 共享模型模式的相关回归 | `build/shared-codegen-delivery/report.json`；Python/Node、输出保护与清单迁移 |
| npm prebuild + TypeScript 编译和运行 | `build/node-typescript-delivery`；使用已有 TypeScript 工具，无新安装 |
| Python 首次包构建和隔离导入 | `build/python-first-package-build/report.json`；setuptools、src 布局、手动重放 |
| Windows SDK 独立接入 | `build/client-final-win/report.json`；C++20、Python、C#实体与单值、Java JNI |
| Linux SDK 独立接入 | `build/client-final-linux/report.json`；C++20、Python、模型子目录手动重放 |
| 其他 Linux SDK 绑定 | Go 和 Rust 可离线编译；Node 原生模块与 TypeScript 包装编译并实际存取 |
| 最终包核对 | 每个包的 manifest.json 记录文件大小与 SHA256；校验 ZIP 内容和当前文档本地链接 |

生成器采用原生词法和声明子集提取，支持文档列出的扁平标量；不等同于完整语言语义编译器。各绑定当前未封装的引擎功能已在语言文档中明确列出。

macOS 宿主工具和脚本已配置 CI，但本轮没有 macOS 实机执行。Android/iOS Player 与设备运行不属于本轮已验证结果。生成器只参与开发构建，不进入移动端或业务运行包。
