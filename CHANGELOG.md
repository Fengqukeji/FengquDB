# Changelog / 更新记录

## 1.0.1

- Reduce work on the caller thread when optional diagnostics are enabled; bound queued events and rate-limit repeated warnings. Diagnostics remain disabled by default.
- Recover and clean recognized owned backup and maintenance outputs after interruption, including source reopen; preserve unrelated files and directories.
- Tighten storage reclamation and maintenance ownership handling.
- Include both English and Simplified Chinese getting-started and API documents in every language SDK.
- Preserve C ABI 2 and the existing storage format. Default write confirmation remains performance mode, 50 ms / 1 MiB; reliable writes retain their existing guarantees.

- 降低启用可选诊断日志时的调用线程开销，限制队列占用并合并重复告警；日志默认仍关闭。
- 修复备份与维护中断后的已登记输出清理，源库重开后可回收自身遗留，同时保留无关文件和目录。
- 改善存储空间回收及维护输出的归属处理。
- 每种语言 SDK 均同时提供英文、简体中文接入文档和 API 文档。
- 保持 C ABI 2 与现有存储格式。默认写入策略仍为性能模式 50 ms / 1 MiB；可靠写入语义不变。
