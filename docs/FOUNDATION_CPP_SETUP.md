# C++ 接入

把 ZIP 内的 `FengquDB` 文件夹复制到项目的依赖目录。包内有公共头文件、Windows/Linux x64 二进制和 CMake 导入配置。

```cmake
find_package(FengquDB CONFIG REQUIRED PATHS "${CMAKE_CURRENT_SOURCE_DIR}/FengquDB/cmake" NO_DEFAULT_PATH)
target_link_libraries(your_app PRIVATE FengquDB::Index)
target_compile_options(your_app PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
if(WIN32)
  add_custom_command(TARGET your_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FengquDB::Core> $<TARGET_FILE:FengquDB::Index> $<TARGET_FILE_DIR:your_app>)
endif()
```

Linux 部署时保留 `native/linux-x64` 两个 `.so` 文件；应用的运行库搜索路径应包含该目录。Index 自身使用 `$ORIGIN` 找同目录 Core。

在应用存储服务或单例中长期保留 `fengqudb::Store`，按操作创建事务或快照。普通文本搜索不需要模型，最小保存与搜索例子见 API。

可选项：Store 构造参数设置存档目录，分别打开 `saves/main`、`saves/config` 可以使用多个库。同一个物理库由一个进程持有。默认性能模式在内存生效后确认，按 50 ms / 1 MiB 合并写盘；关键事务调用 `commit(true)` 等待可靠落盘。整个库需要可靠模式时调用 `db.configure_write({fengqudb::StoreWriteMode::Reliable, 50, 1048576})`。

本包提供 C++ 索引接口；RAG 的记忆治理和 Wiki 的模型编排通过 Python 包接入。原生索引本身用 C++20 构建。
