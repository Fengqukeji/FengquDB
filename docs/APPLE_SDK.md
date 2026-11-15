# Apple 库与 Unity SDK

1.0.0 的真实 macOS、iOS 库使用 GitHub macOS runner 和 Xcode，从本仓库原生核心源码构建。

## 取得 SDK

正式 ZIP 在本仓库 Releases 附件中。Unity 按开发电脑选择：

| 开发电脑 | 文件 |
| --- | --- |
| Windows x64 | FengquDB-unity-win-x64.zip |
| Mac Apple Silicon | FengquDB-unity-macos-arm64.zip |
| Mac Intel | FengquDB-unity-macos-x64.zip |

每个 Unity 包均包含 Windows x64、Linux x64、Android arm64、iOS arm64、macOS arm64/x64 原生库，以及托管库、自动生成器、手动生成工具和两份用户文档。包之间的区别是开发期生成工具的宿主架构。

把 `FengquDB` 文件夹复制到 `Assets/Plugins` 并保留 `.meta`。接入流程见 [Unity 接入](languages/unity.md)，调用方法见 [Unity API](api/unity.md)。

`FengquDB-Apple-native-1.0.0.zip` 单独提供：

- `macos/arm64`、`macos/x86_64`：动态库及其安装名对应的 `libfengqudb.2.dylib`。
- `iphoneos/arm64`：iOS 真机静态库。
- `iphonesimulator/arm64`：iOS 模拟器静态库。
- `Tools/arm64`、`Tools/x86_64`：macOS 原生生成工具。
- `managed-ios`：使用 `__Internal` 调用静态插件的托管程序集。

## 验证范围

[1.0.0 平台验证运行](https://github.com/Fengqukeji/FengquDB/actions/runs/43706853433) 的 Apple 任务已完成两种 macOS 架构和 iOS 真机/模拟器 arm64 的编译链接，并在 macOS 原生宿主实际执行写入、关闭、重开读取。Windows、Linux 和 Android 在本地构建；桌面 SDK 使用 [本地组包脚本](../scripts/build-client-sdk.ps1)，独立接入检查由 `tests/codegen/package_verify.py` 执行。附件提供 SHA-256。

Unity 包检查文件清单、文档、原生目标架构和 `.meta` 平台选择配置，并验证从 `Assets/Plugins` 运行手动生成工具。尚未完成 Unity macOS/iOS Player 和移动真机运行验收。

## 再次构建

生产方在 macOS/Xcode 环境运行 `sh scripts/build-apple-sdk.sh`。GitHub 只保留手动触发的 Apple 构建，使用同一脚本；`scripts/assemble-unity-sdk.py` 在本地合并各平台真实构建产物。用户通过独立 ZIP 接入，脚本与编译环境属于生产方工具链。

早期私有构建输入与体积核查属于发布前历史，见 [Windows 包体核查](PACKAGE_SIZE_AUDIT_20261115.md)。历史性能测量没有因 1.0.0 发布而重新测量。
