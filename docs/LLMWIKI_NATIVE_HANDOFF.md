# 可供公共 Index 分支复制的 Python 文件

来源工作树：`D:/study/FengquDB-llmwiki`，分支 `codex/llmwiki`。

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
