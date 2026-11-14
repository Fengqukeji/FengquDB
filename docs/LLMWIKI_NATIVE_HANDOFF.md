# 公共 Index Python 绑定最终交接

> 本文件保留此前交付记录。当前 0.2 / schema 2 能力见 [底座契约](LLMWIKI_FOUNDATION.md)，本轮未提交修改的实际测试见 [当前验证](LLMWIKI_FOUNDATION_VALIDATION.md)。


来源工作树：`D:/study/FengquDB-llmwiki`，分支 `codex/llmwiki`。

状态：2026-11-14，源码交付提交 `ccea3e0`。主会话已确认将下列三个共享文件整合到 `D:/study/FengquDB-index` / `codex/index-foundation`。Wiki 领域实现继续保留在本分支；无需重复复制整包。

只复制以下三个文件即可获得独立标准库包装与契约测试，不依赖 Wiki 包：

1. `bindings/python/fengqudb_index.py`
2. `bindings/python/fengqudb_index_wire.py`
3. `tests/test_fengqudb_index_python.py`

`bindings/python/fengqudb.py` 是从公共桥接拥有方同步的相同文件，当前与 Index/RAG 的 SHA-256 一致；主树已整合时不需要再次覆盖。其余 `llmwiki/`、`tests/test_llmwiki_native.py` 及 Wiki 部署/说明留在 Wiki 分支，不要求复制整包。没有修改其他工作树、共享 Core C++、header/proto 或根构建文件。

包装按正式 ABI 1 / wire v1 实现 upsert/patch/erase、search/get/stats/list、类型化 Scalar/Predicate、精确向量字节、取消和版本 guard。dense/lexical 参数互斥及 min_similarity [-1,1] 已在编码端检查；不声明 HNSW、严格动态历史统计或字节摘录能力。

生命周期与兼容细节：使用公开 lease，核对实际 Core bridge 函数地址和 Store token；保持 lease 到输出释放结束；不缓存 Store 裸地址。Core 的 allocator/status 通过公开 C 函数地址建立本包装私有 prototype，不改写借用函数表的 ctypes 参数类型，支持 Store 绑定以不同模块名导入。编码/桥接预检失败也通过公开 cancel() 取消 stage 所属事务。

## 独立测试命令

在任一已复制上述文件且具备公开 Core bridge 的仓库根目录运行：

```powershell
$env:FENGQUDB_LIBRARY = 'D:\study\FengquDB-index\build\index-win\Release\fengqudb.dll'
$env:FENGQUDB_INDEX_LIBRARY = 'D:\study\FengquDB-index\build\index-win\Release\fengqudb_index.dll'
python -m unittest discover -s tests -p test_fengqudb_index_python.py -v
```

```sh
export FENGQUDB_LIBRARY=/src/build/index-linux/libfengqudb.so
export FENGQUDB_INDEX_LIBRARY=/src/build/index-linux/libfengqudb_index.so
export LD_LIBRARY_PATH=/src/build/index-linux
python3 -m unittest discover -s tests -p test_fengqudb_index_python.py -v
```

本会话实际 Windows / Docker Linux 各 **14/14，无跳过**。其中 4 项是真实共享库验证：外部事务查询与失败整体取消、dense 强过滤及 metadata patch/guard/list、不同 Core 映像拒绝、公共绑定模块别名不破坏原缓冲类型。6 项 transport fixture、4 项 wire fixture 单独测试错误路径，不冒充真实库用例。

Wiki 另有 **34/34，无跳过**（两平台）通过，含完整真实 NativeIndexProvider 摄入/编译、重复 dirty、缓存再发布、快照/撤销、删除退役及删后重建。命令和细节见 [LLMWIKI_NATIVE.md](LLMWIKI_NATIVE.md)。无真实模型调用或训练。

## 领域版本反馈

v1 domain_revision/source_revision 是 uint64，而 Wiki 对外 revision 是 SHA-256。Wiki 适配层已实现同事务的稳定 token 表，完整哈希保存在元数据和 locator 中；没有截断或冒用哈希数值。目前不要求修改 proto tag。projection_version、publication_generation 与 lifecycle_generation 分别保存，域原件不交给索引副本当权威数据。

## 整合后的验证与边界

以下为主会话在整合后报告的结果，本 Wiki 会话没有重新执行，不能与前述本会话实测混写：

- Windows/Linux：CTest `native_index`、`native_index_cpp`、`python_index`、`store_bridge` 四组全部通过；Python 独立包装 14/14。
- 桥接结果为 13 项通过、1 项旧库用例跳过；跳过不计为通过。
- Linux：ASan+UBSan 两组原生测试通过。该结果归主会话，不意味着 Wiki 领域代码在本会话跑过 sanitizer。
- 已增加原生查询谓词 UTF-8 校验；ABI 与 wire tag 未改变，本轮无需改动 Python 编码或重跑已有领域回归。

公共 C++20 便捷层及使用说明由 Index 分支维护，见 [INDEX_CPP.md](D:/study/FengquDB-index/docs/INDEX_CPP.md) 和 [INDEX_PYTHON.md](D:/study/FengquDB-index/docs/INDEX_PYTHON.md)。公共 header/proto/Core、原生检索算法和安装包由主会话集中整合；Wiki 保留摄入、编译、依赖、引用、领域版本及恢复代码。本轮收尾仅更新文档，不改这三个已共享的 Python 文件。

现有独立命令继续作为复现入口，Wiki 领域命令见 [原生接入](LLMWIKI_NATIVE.md)。Windows/Linux 的局部正确性与桥接验证不等于全平台或完整产品验收；没有新增模型/训练、HNSW、严格动态历史统计、自动索引迁移、UI 或新版 Wiki 发布包。
