# Linux C# 实际运行补验

证据位于 `build/store-csharp-linux-20261107/`。使用官方 .NET 10.0.14 Linux x64 runtime，压缩包 SHA512 与官方发行元数据核对，安装在 `artifacts/tooling/` 的独立目录；未替换系统 runtime，也未安装 Linux SDK。

复用已编译的 portable net10.0 测试 DLL，直接由 Linux ELF dotnet host 执行。实际加载当前基线 Linux Release 核心 `libfengqudb.so.0.3.2`，SHA256 为 `482e8b94d3e6758e6697e4b22ac0a972d370c57934fe88ed404be48d380cec1b`。它与 Docker 核心使用不同构建配置，不能混用哈希。

- `smoke-native-report.json`：完整 Store 接口、事务、维护、查询、历史、watch、I/O 预算、中文根目录和备份恢复通过。
- `advanced-report.json`：Linux 夹具 seed 后，经 Java、C#、Python 接力，验证 `0x8000000000000003` 高位版本及 int64 两端。三条命令均成功；该原始报告整体为 false，因为后续证据脚本读取业务路径的参数索引错误，尚未执行业务检查便停止。
- `advanced-complete.json`：纠正证据脚本的参数索引，复制业务夹具到新的专属目录后，C# 官方生成 DTO v2 读取、typed helper 写入和 Unity 生命周期桌面桩通过。引用之前已成功的数值阶段，保留原始未完成报告，不修改原业务测量数据。

首次 smoke 在 WSL `/mnt/d` 共享文件系统上备份返回 UNSUPPORTED。原生 `renameat2(RENAME_NOREPLACE)` 对照探针在该目录返回 EINVAL，而在 Linux `/tmp` 成功；改用专属 `/tmp` 数据目录后 smoke 全部通过。没有为兼容共享盘而降低不覆盖目标的原子发布要求，目标位置约束见 [目录说明](STORE_LAYOUT.md)。

这些证据证明当前主基线的 Linux C# 调用和互通，不是隔离写入候选的验收，也不代表 Unity IL2CPP、Android/iOS 真机或 iOS Apple 工具链已通过。
