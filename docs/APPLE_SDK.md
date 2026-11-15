# Apple 库与 Unity SDK

2026-11-15 已使用 GitHub 标准 macOS runner 构建实际 Apple 库，无需在本机安装 macOS。私有构建仓库使用账号包含的额度，付费预算保持零。

[成功运行 #1](https://github.com/Fengqukeji/FengquDB-build/actions/runs/43672128035)：任务用时 1 分 31 秒，环境为 macOS 15.8.2、Xcode 16.4、Apple Clang 17。

## 本地产物

Unity SDK 位于 `artifacts/unity-sdk-20261115-complete/`。选择开发电脑对应的包：

| 开发电脑 | 文件 |
| --- | --- |
| Windows x64 | FengquDB-unity-win-x64.zip |
| Mac Apple Silicon | FengquDB-unity-macos-arm64.zip |
| Mac Intel | FengquDB-unity-macos-x64.zip |

每个包包含全部发布平台原生库、托管库、自动生成器、手动生成工具和接入文档。把包中的 `FengquDB` 文件夹放到 `Assets/Plugins` 并保留 `.meta`。macOS 包的区别是开发期序列化工具的 CPU 架构。

补充的 Windows LTO 精简包位于 `artifacts/unity-sdk-20261115-win-lto/`，宿主文件名与上表相同。Windows 核心由 1.15 MiB 缩小到 1.08 MiB，其他平台库不变；已完成原生接口、重开和并发备份等定向检查，未重做完整性能矩阵或 Unity Player 验收。验证口径见 [包体核查](PACKAGE_SIZE_AUDIT_20261115.md)。

单独的 Apple 库位于 `artifacts/apple-sdk-20261115/`：

- `macos/arm64/libfengqudb.dylib`
- `macos/x86_64/libfengqudb.dylib`
- `iphoneos/arm64/libfengqudb.a`
- `iphonesimulator/arm64/libfengqudb.a`
- `managed-ios/FengquDB.Managed.dll`

## 已验证范围

云端编译了以上四种目标和两个 macOS 生成工具，macOS 原生宿主实际执行保存、关闭和重开读取；iOS 真机与模拟器各完成 C ABI 可执行文件链接。三个 Unity 包通过文件清单、CRC、文档链接与哈希检查。

本机再次核对 GitHub 产物 SHA-256、每个 SDK 文件的大小和哈希、Mach-O CPU/平台、静态库对象的目标平台和动态库系统依赖。未执行 Unity macOS/iOS 构建或 iOS 真机运行验收。

固定源代码包、workflow、逐文件输入清单、校验结果和成功截图保存在 `artifacts/apple-cloud-20261115/`。输入使用 Index 工作树已验证的核心源码，以及当前工作树的 C# 绑定、生成器和组包脚本；具体输入哈希在 `snapshot/snapshot.json`，下载产物的输入清单在 `artifacts/apple-sdk-20261115/snapshot.json`。

GitHub 下载产物 SHA-256：`2b7efcab6e1d91947cd9ff538f3a0fd3984d1ccec930af7a1c08a96714a588bb`。

## 再次构建

`scripts/prepare-apple-cloud.py` 生成固定源代码 ZIP 和与其校验值绑定的手动 workflow。它仅收集必要源码和指定预编译输入。将 ZIP 和 workflow 更新到私有构建仓库后运行 Apple SDK 任务；保留标准 runner 和零付费预算。常规源码仓库 CI 的 Apple 任务仍使用 `scripts/build-apple-sdk.sh`。
