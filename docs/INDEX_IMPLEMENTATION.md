# FengquIndex 实现状态与验证

2026-11-14，工作树 `D:/study/FengquDB-index`，分支 `codex/index-foundation`。索引、RAG、Wiki 底座已经完成本轮实现、自审和 Windows/Linux 相关验收；业务项目的最终替换另由接入会话执行。

## 已交付

| 能力 | 实现 |
| --- | --- |
| 原生索引 | C++20 独立模块，C ABI、C++ facade、Python 标准库封装；复用现有 Core，不依赖外部检索引擎 |
| 同事务一致性 | 领域原文、索引投影、版本、墓碑和维护检查点同一事务提交；查询固定快照 |
| 增量与减法 | 复用未变词项；metadata patch 裁剪原二进制区段，不重建全文或向量 |
| 全文 | 字段加权 BM25、中文单字/字对、ASCII/全角折叠、标识符拆词；短语、邻近、末词前缀、排除词、字段限制 |
| 结果解释 | UTF-8 byte offsets、有界摘录、词频/长度/IDF/贡献分解 |
| 权限及时间过滤 | 类型化比较、范围、IN、存在、AND/OR/NOT；statistics_policy=1 按过滤后可见文档计算完整 BM25 统计 |
| 分块倒排 | 最多128行的压缩块、字段字典与ID前缀压缩；最新delta覆盖；单有效词的安全块上界跳过 |
| 有界维护 | 词项内部也可断点续做，保留截断块尾部；检查点持久化，重开继续 |
| 精确向量 | 归一化FP32余弦；标量/x64 SSE2/ARM64 NEON路径，小端整块读写，大端有显式转换 |
| 近似向量 | 自研持久HNSW，INT8导航、权威FP32重排；显式建图才承担维护成本，默认精确搜索 |
| 图重建与回收 | 影子代次、构建期间双写、发布前校验、旧代次有界回收；旧快照仍可读 |
| 质量与预算 | 排名性质、完成性、覆盖、命中证据分开；候选/扫描/词项/内存/超时/取消可限额 |
| 生命周期 | CAS、幂等重放、墓碑、防旧代次复活；Core身份和公开租约避免混用库映像 |

RAG 提供账户、owner/public、scope、timeline/as_of、事实有效期、纠错历史、分批清理与防复活、dense/lexical/hybrid、授权重排及引用上下文。Wiki 提供来源摄入、主题页、增量编译、实体/声明/冲突裁决、人工修订保护、持久任务、引用与答案缓存。两者生产检索都使用共享 C++ Index，Python参考评分只保留在Wiki测试中。

## 验收证据

- Windows MSVC Release / Linux GCC Release：native_index、native_index_cpp、native_index_extended 三组各全部通过。基础测试17,425条断言；扩展测试914项检查。
- 扩展覆盖分块整理前后分数与排名、17项倒排预算内续做、delta删除、旧快照、中文位置、短语邻近、前缀、过滤BM25独立对照、影子重建恢复、构建中增删改、旧图回收、二进制ID、SIMD奇数尾部。
- GCC11 / glibc2.31 便携构建的914项扩展检查通过；C++运行库静态链接，Index以$ORIGIN找Core。发布包不使用开发镜像glibc2.43生成的二进制。
- 最终Python共享包装22项、Wiki49项，负责人报告Windows/Linux各71/71，无跳过；摘要在Wiki工作树build/index-python-wrapper-handoff.json。
- 最终RAG Memory38/source26/SDK2，负责人报告Windows/Linux各66/66，无跳过；原生库及包装摘要在RAG工作树build/rag-frozen-verification.json。
- 本会话已验证组合Python SDK从临时空目录运行Core/Index/维护/RAG/Wiki，自动定位同包二进制；清除显式库路径，不依赖源码模块，不创建顶层全局别名。Windows/Linux均通过。
- 二进制C++ SDK以独立find_package consumer编译运行，Windows/Linux均返回中文检索结果。公共桥接和Python索引CTest也已通过。

ASan/UBSan的最终状态与发布包摘要统一记录在[本轮交付验收](FOUNDATION_ACCEPTANCE.md)。Android/iOS/ARM64设备和TSan本轮未执行；CI配置覆盖不等于目标平台已经验收。

## 有界性能样本

同快照2,400条记录，10次预热、交替顺序。低频词匹配24条，取Top8；热门词比较同一查询在整理前后的快照，分数与ID完全一致。

| 平台 | 低频倒排 / 无索引扫描中位数 | 热门词未整理 / 分块中位数 | 热门词读取记录数 |
| --- | --- | --- | --- |
| Windows | 37.90 / 419.60 μs（11.07倍） | 1905.70 / 186.80 μs（10.20倍） | 2400 → 128 |
| Linux Docker | 19.88 / 154.40 μs（7.77倍） | 1195.62 / 114.02 μs（10.49倍） | 2400 → 128 |

源码 tests/index/lookup_bench.cpp。只证明此数据分布的工作量减少，不代表相对其他检索引擎的全面胜出。

4,096条24维、16个固定查询，HNSW Recall@8=100%。此低维小集合中Windows exact/ANN约2074/1922μs，Linux约1327/3409μs；ANN并非总比精确快，默认仍用精确模式。未测真实大语料召回、百万向量、稳定P99或真实LLM答案质量。

## 发行和后续接入

独立包位于 artifacts/foundation/FengquDB-Python.zip 与 FengquDB-CPP.zip；每包复制一个FengquDB目录。每种语言只有接入.md和API.md两份用户文档。

旧数据迁移已取消；仅重建PTpma索引/经验、WordGame RAG对应存储，不能删除源码、模型或游戏业务存档。同一物理库由单进程持有，应用IPC由宿主实现。模型蒸馏未启动；50MiB模型的能力损失没有实测百分比。

复测入口：

```sh
cmake -S . -B build/index -DFQDB_BUILD_INDEX=ON -DFQDB_BUILD_INDEX_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/index --config Release --target fqdb_index_tests fqdb_index_cpp_tests fqdb_index_extended_tests fqdb_index_lookup_bench
ctest --test-dir build/index -C Release -R '^(native_index|native_index_cpp|native_index_extended|python_index|store_bridge)$' --output-on-failure
python tests/index/foundation_sdk_smoke.py --sdk artifacts/foundation/python
```
