# 发布包体积核查

首轮核查对象为 `artifacts/unity-sdk-20261115-complete/FengquDB-unity-win-x64.zip`，以及现有独立语言 SDK。随后使用相同冻结源码补做 Windows LTO 对照，并另行产出 `artifacts/unity-sdk-20261115-win-lto/`。原包不变，没有重跑公开五库性能矩阵或宣称 LTO 提速。

## 结论

**有精简空间，已量化的主要收益在 macOS 符号表；核心库没有混入整套测试、基准程序或其他语言生成器。**

| 对象 | 原始字节 | 副本裁剪后字节 | 变化 |
| --- | ---: | ---: | ---: |
| macOS ARM64 动态库 | 1,241,024 | 936,544 | 减少 24.53% |
| macOS x64 动态库 | 1,319,704 | 1,016,948 | 减少 22.94% |
| Linux x64 动态库 | 1,252,272 | 1,252,272 | 无变化 |
| Android ARM64 动态库 | 1,218,176 | 1,218,176 | 无变化 |
| Windows x64 核心 DLL | 1,206,272 | LTO：1,128,960 | 减少 6.41%，已定向验证并单独打包 |
| iOS ARM64 静态库 | 1,722,480 | 未得到有效候选 | 现有 LLVM 12 不支持其中的新加载命令 |

macOS 实验使用现有 NDK 的 `llvm-strip -x`，只删除本地符号。裁剪前后 `llvm-nm -g --defined-only` 的输出一致，但这不等于 Apple 平台运行验收。正式产物需要在 macOS 使用配套工具裁剪、重新签名，并验证加载、接口链接和运行。原始库及 ZIP 均保持不变。

## SDK 下载体积与成品体积

完整 Unity Windows 开发包有 36 个文件，ZIP 为 4,004,045 字节（3.82 MiB），解压为 10,097,587 字节（9.63 MiB）。其中包含六个平台原生库、两种托管适配、开发期生成器和工具、两份用户文档及许可证。

这些平台库通过 `.meta` 筛选，生成工具位于 `Editor` 或禁用为运行插件。过去的 Windows Mono Player A/B 确认只带目标平台原生库和托管运行库，工具、生成器、文档及其他平台库未进入 Player；当时增量为 1.18 MiB。该 A/B 使用的是更早的核心 DLL，不能当作最新完整包或 Android/iOS 的实测体积。首轮完整包的 Windows 原生库与托管库文件相加为 1,303,040 字节（1.24 MiB）；LTO 新包相加为 1,225,728 字节（1.17 MiB），也不等于重新构建得到的 Player 增量。

## 确实存在的冗余或待优化项

1. **macOS 本地符号**：两个动态库合计可以裁去 607,236 字节；这是符号元数据，不是删除数据库功能。动态加载所需的导出/导入信息仍须保留。
2. **首轮 Windows 核心没有使用 LTO**：原 DLL 来源为 Index 工作树的 `build/index-win/Release/fengqudb.dll`，其缓存为 `FQDB_RELEASE_IPO=OFF`；Apple 脚本也显式关闭 IPO。补充对照使用相同冻结源码和 MSVC，重新构建的无 LTO 基线与原包的代码段逐字节一致；开启 LTO 后缩小 77,312 字节。新包不改变数据库功能、异常处理、运行时链接方式或持久化选项。
3. **独立 Linux 语言包重复存放同一库**：检查的 C++、C# 包均同时包含内容相同的 `libfengqudb.so` 和 `libfengqudb.so.2`，每份为 1,417,856 字节。两种文件名服务于直接加载和 SONAME 依赖，直接删一个会破坏部分接入；可通过统一加载规则或可靠的别名机制消除实体重复。这是较早的语言包，不是上表完整 Unity 包中的 Linux 库。
4. **重复许可证**：Unity 包中 `nlohmann-json.MIT` 与 `nlohmann-json.LICENSE.MIT` 内容相同，重复 1,076 字节，压缩后约 637 字节。可统一名称并修正引用，但收益很小。JSON 仍由开发期原生模型工具使用；删除重复文件不等于可以删除所需的 MIT 声明。

## 未发现的混入

- 发布 ZIP 不包含 PDB、OBJ、测试程序、基准程序、完整 Roslyn 或 Google Protobuf runtime。
- Windows 核心仅导出 115 个 `fqdb_` 接口，导入依赖为 Windows 系统的 `KERNEL32.dll` 与 `bcrypt.dll`；没有动态链接 MySQL、SQLite、Redis、JSON 或 Protobuf 库。
- 核心目标只编译 `common.cpp`、`logging.cpp`、`store.cpp`。测试用 `FQDB_TESTING` 仅属于独立测试目标；JNI、Node、诊断适配器和生成器是其他目标，不会因为同一次构建就合并进核心 DLL。
- RAG、Wiki 与专业检索底座不是这个核心 DLL 的混入模块；`fqdb_store_index_*` 是数据库本身的字段索引能力。

Windows DLL 的文件段以代码为主：`.text` 为 977,920 字节（约 81.1%）。剩余还包含常量、导出表、静态数据、异常展开和重定位信息；这些不能按“看起来不是业务代码”直接删除。静态 C++ 运行时也有体积成本，换成动态运行时须同时计算新增分发依赖。

## Windows LTO 补充验证

- 新核心为 1,128,960 字节（1.0767 MiB），115 个导出函数及其序号与原包一致；C ABI、C++ SDK、编码、不可变读取和重开读取检查通过。
- 对同一个基准程序交替加载无 LTO / LTO DLL，各跑三次。12 项中位数比值范围约为 0.946–1.035；普通按键命中为 1.019，新增为 0.965，256 项 Hash 扫描为 0.946。短测表明整体接近，不能据此保证所有负载没有回退或声称提速。
- 生产 ABI 的四种组合（性能/可靠模式 × 普通/分段存储）共完成 1,024 笔并发写事务，同时检查快照、备份、同步、重开与数据校验。
- 修正旧基准清理名单遗漏 `.blk` 文件的问题；修正前的基准退出失败不计入通过样本，诊断记录保留在本地审计目录。
- 三个宿主版本的 Unity ZIP 均包含相同 LTO Windows DLL，其他平台库保留已验证版本。没有重新运行 Unity Player 或移动设备验收。
- 新包位于 `artifacts/unity-sdk-20261115-win-lto/`。后续固定 Apple 云构建输入时，可向 `scripts/prepare-apple-cloud.py` 传入 `--windows-library` 选择已验证的 Windows 库，避免继续拾取旧构建目录。

后续优先完成 Apple 符号裁剪与平台验证，再整理独立语言包的重复库文件。保留全部数据库功能及恢复/校验路径，不以关闭校验、异常处理或压缩壳换取表面体积下降。

原始实验记录见 [package-size-audit-20261115.json](evidence/package-size-audit-20261115.json)。本地候选副本位于 `artifacts/package-size-audit-20261115/`，未替换交付库。

Windows LTO 的原始短测、接口与稳定性记录见 [windows-lto-size-20261115.json](evidence/windows-lto-size-20261115.json)。公开首页的五库性能结果仍绑定原冻结测试输入，不是这次短测。
