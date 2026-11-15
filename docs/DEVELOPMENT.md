# 开发、验证与交付

当前源码的入口是 [项目 README](../README.md) 和 [当前文档](README.md)。历史 SDK 和 build 下某个旧 DLL 不表示最新源码。使用独立构建目录，记录源码清单、编译配置、库 SHA256 和实际验证命令；每份测量只代表绑定的源码/库。

## 版本控制与并行修改

日常开发与代码提交使用 `develop` 分支。`main` 仅保留初始化空提交和空文件树，未经用户要求不将开发内容合并到 `main`。只有用户明确要求提交或推送时才执行对应操作。

自有源码、测试、构建配置和当前契约应纳入版本控制，建立可回滚基线。多人/多个代理使用独立工作树或分支，并在合并时统一验证；仅把构建输出分开不能隔离同时改同一源码的冲突。build、下载参考源码、工具缓存和旧二进制不应混入源码基线，历史证据另行保存。

本地冻结 zip 和文件哈希可还原对应阶段，但不能提供提交历史、分支合并或作者归属。建立 Git 仓库不能从当前快照重建此前每次修改。

## 编译单元约定

公共头文件和可复用内部头文件应包含自身必需的声明。`commit_signal.hpp` 有独立编译检查，不能依赖 storage.hpp 先包含 Windows API。

`src/paged/group.hpp`、`periodic_write.hpp` 是只由 store.cpp 拥有的实现片段，定义非 inline Core 成员。它们要求 FQDB_CORE_IMPLEMENTATION，不能作为普通头文件加入第二个编译单元。当前布局没有重复定义；将来拆分翻译单元应先划定私有接口，测量构建时间、运行性能和包体再迁移。

`fengqudb_test` 带 FQDB_TESTING 的故障注入和观察点，重新编译核心是为了得到不同的测试行为，不能直接复用发布对象冒充故障测试。

`Core::current()` 的 shared_ptr 原子操作负责视图寿命，不承诺无锁。C++20 的 atomic<shared_ptr> 同样不保证无锁；裸指针替换必须先建立安全回收协议。已有 read_stamp 快路径用于减少重复获取，进一步更改以定向实测为依据。

## 消毒器

普通构建默认 FQDB_SANITIZER=none，发布 ABI 和默认优化选项不变。Linux GCC/Clang 支持 address（ASan+UBSan）或 thread（TSan），不能混在同一构建中；TSan 使用静态核心。示例：

```sh
cmake -S . -B build/san-address -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFQDB_SANITIZER=address -DFQDB_SHARED=OFF \
  -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF
cmake --build build/san-address --parallel 2 --target \
  fqdb_header_self_containment fqdb_write_policy_tests fqdb_committed_cache_tests \
  fqdb_atomic_owner_tests fqdb_store_sdk_tests fqdb_store_select_tests fqdb_store_query_tests
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/san-address --output-on-failure \
  -R '^paged_(write_policy|committed_cache|atomic_owner|store_sdk|select|query)$'
```

TSan 使用独立目录、`-DFQDB_SANITIZER=thread` 和 `TSAN_OPTIONS=halt_on_error=1`，其余目标和过滤相同。Windows、Linux、Android 的构建及消毒器验证在本地执行，保留 JUnit 和 CTest 日志。GitHub 只保留手动触发的 macOS/iOS 构建。若运行器因 TSan shadow 映射而在测试启动前失败，应保留诊断并修复环境，不能作为测试通过或静默跳过。

## 异常与持久化

C ABI 必须阻止 C++ 异常跨语言边界。current_error 保留数据库状态、bad_alloc 和 length_error，未知异常才映射 INTERNAL_ERROR。清理后重抛、可选缓存失败回退、已开始 WAL 后故障封锁等 catch 有不同契约，不能按出现次数批量删除。异常诊断应保留阶段和状态，并避免在内存不足路径分配大量字符串或把业务键值写入日志。

flush_periodic 排空捕获序号之前的执行队列，等待的是 periodic_completed；它自身不提供“某序号强制刷盘”的协议。sync、配置切换、停止线程及可靠请求的后续步骤负责持久化，取消标志由请求检查点观察。

## 证据与清理

按实际影响范围验证，涉及提交、取消和持久化应覆盖相关故障/恢复路径。测试数量以当前配置的 `ctest -N` 为准；本轮开启工具和诊断共有33组，跨语言、生成器和资源故障脚本单独计数。本次全面验证见 [develop 稳定性验收](DEVELOP_STABILITY_20261114.md)。

build/artifacts 占用是开发空间，不能当成运行库包体。清理前区分正在使用的构建树、唯一源码快照、失败证据、数据库夹具、下载缓存和可重建中间文件；先保留必要清单及归档，再删除确认为本次所有的临时项。历史报告链接应继续可追溯。
