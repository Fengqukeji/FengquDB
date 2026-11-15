# GitHub 发布准备

本文记录 GitHub 首页和发布准备。源码提交与推送按用户授权执行；公开 GitHub 仓库、Pages 和 Release 尚未发布。

## 仓库信息建议

仓库名：`FengquDB`。主首页 `README.md` 为英文，`README.zh-CN.md` 为中文；相互切换，不把两种语言逐句交错。

About 英文描述（可直接粘贴）：

> A tiny, high-performance C++ embedded database for AI applications, local-first software and Unity. 1.08 MiB native core, binary storage and transactions.

中文简介：

> 1.08 MiB 的高性能 C++ 嵌入式数据库核心，为 AI 应用、本地优先软件和 Unity 提供二进制存储、事务与跨语言接入。

Website：`https://fengqukeji.github.io/FengquDB/`，正式启用 Pages 并验证成功后填写。发布 `docs/benchmarks` 目录，保留数据、证据和许可证。

SDK ZIP 由构建流程生成，正式发布时上传到对应 GitHub Release 的 Assets。`release` 分支如果启用，用于维护发布源码；它不等同于 Releases 附件。README 不放独立的 SDK 下载链接，用户从仓库 Releases 获取对应语言和开发平台的 ZIP。

已准备 `.github/workflows/pages.yml`：仅在手动触发时部署静态报告，不重新运行数据库测试。正式发布时，在目标仓库把 Pages 来源设置为 GitHub Actions，再运行 Publish benchmark report。当前尚未执行部署。

## Topics：建议这 20 个

优先覆盖数据库定位、可核实的能力和目标使用者。GitHub 最多 20 个 Topics，每个不超过 50 字符：

```text
embedded-database database key-value-store cpp high-performance
protobuf binary-serialization cross-platform transactions local-first
unity game-save dotnet csharp python java
ai-infrastructure ai-agents rag knowledge-base
```

`embedded-database`、`key-value-store`、`cpp`、`high-performance`、`protobuf`、`unity` 是核心词；`ai-infrastructure`、`ai-agents`、`rag`、`knowledge-base` 表达适用方向。首页明确 RAG/Wiki 是上层，不把核心宣传成自带向量模型或完整 RAG 产品。

不建议加入 `sql`、`distributed-database`、`replication`、`gpu`、`vector-database` 等核心包尚不具备或不能代表本次交付的标签。避免 `fastest-database` 作为事实标签；没有覆盖所有数据库与负载的证据。

规则见 [GitHub Topics 官方说明](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/classifying-your-repository-with-topics)。

Topics 能帮助 GitHub 站内发现，但不会自动保证 Google/Bing 排名。标题、About、可索引的中英文正文、可运行例子、完整测试方法、实际用户链接和后续技术文章共同起作用，不堆无关关键词。

## 预览与发布检查

- 源码按用户授权提交至 `develop`；首页交互审阅使用本地 HTML，GitHub 草稿用于渲染参考，不据此创建公开仓库或发布 Release。
- 首页图表由 `python scripts/prepare-github-home.py` 从 `docs/benchmarks/results.json` 生成，原测试不重跑。`comparisons.json` 保存 44 组中位数和逐库倍数。
- 图表静态文件随源码发布；GitHub README 无法直接运行交互 HTML，所以首页嵌入 PNG，交互报告用 Pages 链接。
- README 已移除 SDK 下载按钮及未启用的 Pages 入口，报告先链接仓库中的说明与原始证据。准备文档中的 Releases、Pages 地址是计划地址，正式发布前逐一验证；交互报告部署验收后再配置 Website 或添加线上入口。
- GitHub 在线预览使用私有构建仓库的编辑器，正文不点 Commit。为显示图表，允许把四张不含私密资料的性能图片上传到未提交的 Issue 草稿；预览副本使用附件地址，正式 README 保持仓库相对路径。
- 未提交 README 的相对语言链接会指向尚未存在的仓库文件并出现 404。当前语言切换审阅使用本地 `artifacts/github-home-preview-20261115/en.html` 与 `zh.html`，语言和章节链接均指向对应 HTML。GitHub 草稿仅验证渲染外观；正式发布仍使用两份独立 README 的相对链接。
- 本地预览使用 `python scripts/preview-docs.py --port 7935`：Markdown 统一渲染为声明 UTF-8 的 HTML，浏览器入口使用 `.html`，旧 `.md` 地址重定向到对应网页；接入与 API 文档的本地链接同步转换，不使用原始 Markdown 响应作为浏览器审阅页。
- 首页所述约 1 MiB 对应 Windows x64 LTO 验证包的原生核心：1,128,960 字节，约 1.08 MiB。完整 SDK、工具及其他平台库另计。性能图沿用原冻结测试版本，不能把精简包体积当作新一轮性能实测。
- 首页标题“目标：世界上最快的数据库”为目标表达；胜出结论只对应这批性能模式业务吞吐，保留持久化与调用架构差异。
- 两张原始收款码已放入 `docs/assets/sponsors/alipay.jpg` 与 `wechat.jpg`，首页以 240px 宽显示，保留原始像素。已离线检查协议与 EXIF，平台规则和身份公开范围见 [赞助核查](SPONSOR_QR_REVIEW.md)。当前仅为私有未提交预览。
- 发布前检查源码和许可证；构建产物使用 Release 附件，不将 build、artifacts、业务存档或凭据推入源码仓库。

首页体积宣传使用核心 DLL 对 MySQL Windows `mysqld.exe` 的 1/46 比例，精确数值与发行包来源见 [体积核查](../docs/SIZE_COMPARISON.md)。读取速度约为 MySQL 的 400 倍、单条新增写入速度约为 MySQL 的 40 倍，只对应单调用线程 128 B 简介档案的完整读取和单条新增吞吐，不能省略场景限定。

## 后续技术文章建议

1. “1 MiB 的嵌入式数据库如何完成档案 CRUD”：使用快速示例，说明真实集成步骤和持久化选择。
2. “缓存二进制减法如何减少整存重建”：公开机制、适用数据变化及退化路径，不只展示最好结果。
3. “同一业务的五库实测”：使用本报告与原始数据，解释嵌入式与服务调用、序列化及持久化设置的影响。

文章围绕开发者的问题组织，每篇回链到项目和可复现实验；不开设未经用户批准的外部账号或发布内容。
