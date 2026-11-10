# Store 目录规范复核（2026-11-07）

本次围绕开发版 Store 的可配置根目录、浅层布局、短文件名及 Unity 持久化目录进行复核，原有已发布单文件 Database 的格式和发布包保持兼容边界。接入说明见 [Store 目录与 Unity 存档](STORE_LAYOUT.md)。

## 目录契约

- C/C++、C#、Java、Python 均提供按实例设置 `root` 与 `name` 的入口，实际目录为 `<root>/<name>`，没有进程级可变根目录。
- 新生成的数据文件及内部临时文件最长 21 个 ASCII 字符，库内文件平铺。集合名、业务键和用户 ID 不参与文件命名。
- Windows 完整内部路径限制为小于 240 个 UTF-16 单元，POSIX 为小于 960 字节；打开目录预留内部文件名空间。旧长名代际继续可读，成功整理后逐步改为短名。
- 普通存储、恢复和整理文件留在库目录内。显式备份、恢复、迁移在调用者选择的目标父目录内创建短临时目录，再原子发布；日志也须显式指定独立目录。
- 推荐 Unity 以 `Path.Combine(Application.persistentDataPath, "db")` 为根目录，库名使用 `main`、`slot1` 等短名；日志与备份由宿主统一放在持久化目录下的 `log`、`bak`。

## 本次修复

原检查只限制完整路径长度，POSIX 下仍可能接受单个超过 255 字节的目录名。此时 `open_at` 的递归创建可能先留下部分父目录，再因组件过长失败；显式备份也可能在发布目标时才发现问题。

现于路径构造阶段逐组件检查最多 255 个原生单元，超限返回 LIMIT，先于任何目录创建；同时以减法比较预留空间，避免长度相加溢出。补充测试覆盖 ASCII 超长组件、UTF-8 中文组件按字节超限、拒绝后目录无新增、中文根目录重开和多实例根目录隔离。

接入文档同时明确 `root/name` 的实际拼接规则，统一日志示例使用宿主持久化根目录，并修正 Unity 源码接入应包含全部 C# 绑定文件的说明。

## 验证证据

本批证据位于 `build/store-layout-20261107/`：

- Windows 相关 5 项 CTest 通过：maintenance、operations、history、io_budget、diagnostics，见 `windows-ctest.log`。
- Linux ASan/UBSan 相关 3 项通过：maintenance、operations、io_budget，启用泄漏检测及 UBSan 失败退出。构建后源文件 SHA256 清单检查通过，见 `linux-asan.log`、`asan/source.sha256`。WSL 挂载盘构建有毫秒级时间戳偏差警告，最终三个目标均构建并执行成功。
- C#/Java/Python 的 9 步构建、运行及跨语言共库互通通过；各语言包含中文 `open_at` 根目录写入及关闭后重开，见 `bindings/report.json`。
- Android ARM64 原生共享库编译通过，见 `android-build.log`。iOS Apple 工具链构建及 Android/iOS Unity IL2CPP 真机运行尚未验收。
- 既有发布 DLL 的 SHA256 仍为 `3411f07db33140f7b7fc1a479d36c51b862460295680194462489a39d07ee9c9`，见 `artifact-hashes.json`。

验证范围按路径与存档风险选择，未将本批结果扩大解释为整个长期可靠性目标完成。

## 日志目录边界补查

后续审查发现可选诊断适配器只检查完整路径，尚未逐段检查 255 原生单元限制。Linux 下传入不存在父目录再接 256 字节目录名，新边界用例在旧实现失败，并即时观察到已创建的父目录；复现信息见 `diagnostics-repro-before.json` 与 `diagnostics-linux-before.log`。WSL 临时目录本身不作为长期保留的证据。

现对日志原始绝对路径和符号链接规范后的路径统一检查完整预算及每段长度，改为减法比较预留 56 个单元，先于日志后端创建目录返回 LIMIT。有效中文路径仍正常写入；公开接口及文件格式不变。

补查验证覆盖 256 字节 ASCII 单段、258 字节中文单段、拒绝后父目录不存在、中文日志目录和全部文件平铺短名。Windows Release 的 diagnostics 1/1 与 Linux ASan/UBSan（含泄漏检查）的 diagnostics 1/1 通过，分别见 `diagnostics-windows-final.log`、`diagnostics-linux-final.log`。Linux 构建沿用隔离消毒器目录并启用可选诊断目标；挂载盘时间戳偏差及 WSL 代理提示保留在日志中，没有影响编译和执行结果。

核心 DLL 字节与已发布 DLL SHA256 均保持上述版本；本次仅重建可选诊断库，未重新全量测试核心。最终文件哈希记录在 `diagnostics-final-hashes.json`。读取性能扫描在补查修改前已冻结源码完成；后续比较须重建工作镜像，不能把旧镜像称作当前完整源码。
