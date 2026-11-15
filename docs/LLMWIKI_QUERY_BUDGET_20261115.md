# Wiki 原生长查询回归（2026-11-15）

Wiki 使用共享 NativeIndex 的查询预算修复，来源为 `codex/index-foundation` 的
`132f9b21263e81983a91a0aa35236d576f66894a`。部署应使用该修复后的配套 SDK；
Wiki 编译、问答、租约和向量接口无需修改。

## 同步边界

当前 `codex/llmwiki` 并不持有 `src/index/index.cpp`、`tests/index/`、
`cmake/FengquIndex.cmake` 或 `scripts/package-foundations.py`；已用 `git ls-tree`
核实。因此没有 cherry-pick 该提交，也没有在 Wiki 分支新增独立原生引擎。
底层源码及 C++ 回归继续由 Index 分支维护，本分支增加公共 Wiki API 的回归。
共享 bindings、原生库、SDK manifest 和既有 lease 修复均未修改。

## 实测依赖

- SDK：`D:/study/FengquDB-index/artifacts/foundation-20261115-query-budget`。
- release_id：`8368499292891011dfc4c53269c9dc0b8a533019b8fd4f68c595536d4f675c67`，直接读取包 manifest。
- 包内 `llmwiki/wiki.py` 与本分支生产文件字节一致，SHA-256 为
  `60ac832171d975c073084c4c2720952687f27f6ab42ebbbf00f2c3d0d514de58`。
- 包内 `bindings/python/fengqudb.py`、`fengqudb_index.py`、`fengqudb_index_wire.py`
  与本分支对应文件字节一致。

| 平台 | Core SHA-256 | Index SHA-256 |
| --- | --- | --- |
| Windows | `b35269a424f653fbccda077f588f0ddb98afc2077a3ead39f3103461ab0d59b4` | `0b00e394c1bf40112876b28ea95c191d4ea2e0f3838a72b05e908a2fa6c0f441` |
| Linux | `b8f07ad194abbec18d02a5e73494455701410ed5f93544ab5bdea35041100d3b` | `ac1c515a662f21c6f84535dbeabe103fc08f8f094470a4f40a73f60f0b8ee81d` |

两平台 Core 哈希均与 `foundation-20261114-r2` 相同。本轮仅运行 Windows，
Linux 上述信息是文件哈希核对，不代表本轮执行过 Linux 测试。

## 回归与结果

`tests/test_llmwiki_native.py` 新增两个真实库集成测试，覆盖：

- 33/64/65/66/128/129/140 词 AND 查询，含 prefix-last；缺少第 66 词或最后一词的来源不会被误收。
- ANY 查询保留第 64 位之后的命中，140 词 `Wiki.ask` 保留正确来源引用。
- 自然中文长问句在默认预算下完成，400 个重复词按去重后的单词预算完成。
- 显式词项和内存预算不足分别返回 `term_limit`、`memory_limit`，不会将不完整结果标记为已证明无匹配。

旧 r2 配套库运行新增的两项测试方法均失败，原生错误为
`FengquDB 9: size/version limit`；日志中的 15 个 errors 包含子测试，不能当作 15 项测试。
旧库在中文查询处提前失败，没有执行到重复词与预算检查。

新版配套库执行：

```powershell
$env:FENGQUDB_LIBRARY='D:\study\FengquDB-index\artifacts\foundation-20261115-query-budget\python\FengquDB\native\win-x64\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY='D:\study\FengquDB-index\artifacts\foundation-20261115-query-budget\python\FengquDB\native\win-x64\fengqudb_index.dll'
python -m unittest discover -s tests -p test_llmwiki_native.py -v
```

结果：**10/10 通过，无跳过，1.094 秒**。包括既有可见性、版本、回滚、发布、
删除恢复、向量及维护相关测试。本轮没有重跑整个 Wiki 套件，也没有调用外部模型。
中文问答使用离线引用 provider，证明查询与引用路径正常，不是模型语义质量评测。

原始日志保留在被忽略的 `build/wiki-query-budget-20261115/old-native.log` 与
`new-native-windows.log`。本轮没有发布或修改 SDK 包，也没有进行业务接入。
