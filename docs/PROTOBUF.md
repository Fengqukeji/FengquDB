# 独立 wire 编码器的范围与来源

`include/fengqudb_wire.hpp` 是 FengquDB 内独立编写的 C++17 wire primitives；不复制或编译 Google 的运行库实现，不依赖 protoc、反射、生成代码、Abseil 或完整 libprotobuf。它是有限编码层，不是完整 Protobuf SDK 的重写。

参考公开 [Encoding 规范](https://protobuf.dev/programming-guides/encoding/) 与 Google 官方源码中的接口和边界行为：

* [v36.2 wire_format_lite.cc](https://github.com/protocolbuffers/protobuf/blob/v36.2/src/google/protobuf/wire_format_lite.cc)：wire type、未知字段与 group 跳过。
* [v36.2 wire_format_lite.h](https://github.com/protocolbuffers/protobuf/blob/v36.2/src/google/protobuf/wire_format_lite.h)：ZigZag 与字段语义。
* [v36.2 io/coded_stream.h](https://github.com/protocolbuffers/protobuf/blob/v36.2/src/google/protobuf/io/coded_stream.h)：varint / 固定宽度读写接口及快慢路径边界。

官方 v36.2 源码和测试工具仅下载到 `artifacts/reference`，不进入库的编译、链接或 SDK 安装。另曾下载 v33.0 作为最初参考，未引入生产源文件。Google 源码档案附带其原有 LICENSE；重新分发官方档案/工具时须保留许可证，独立实现不能用来删除实际衍生代码的许可证义务。

固定参考档案的 SHA256：

| 文件 | SHA256 |
| --- | --- |
| protobuf-v36.2.zip | 9f920d506147af23d9500278661e31f670ec541c79a8f46041ae1f833400e086 |
| protoc-36.2-win64.zip | f0c128dc0d8492eceece83bb459a4c0e316764b929ffbf1aa416357fd644edd3 |

实现支持 unsigned varint、ZigZag、fixed32/64、IEEE float/double、长度字段、标签、未知字段跳过（包含最大深度 64 的旧式 group）。packed repeated 与嵌套消息由调用方使用子 Reader/Writer 组合。Reader 默认消息上限 64 MiB，拒绝非法标签、长度越界、截断及 uint64 溢出。Reader 借用输入，调用者必须维持输入生命周期。

不提供 `.proto` 编译器、反射/descriptor、JSON/TextFormat、自动对象映射、UTF-8 校验、消息合并与未知字段对象保留；应用可以使用官方语言库。应用字节在数据库中原样存取，不经过重新编码。

业务 DTO 的官方生成流程、C#/Java/Python 互通、Unity 接入、optional 默认值与索引规则见 [业务模型示例](../examples/protobuf/README.md)。两版 schema 的未知字段升级及实际调用成本见 [验证与测量记录](PROTOBUF_BUSINESS_REVIEW_20261107.md)；这些依赖仅属于业务应用。

`tools/verify_protobuf.py` 使用官方 protoc 双向验证极值整数、ZigZag、定长整数、浮点、含零/高位字节、packed repeated、嵌套消息与未知字段。有限互操作测试不能代表通过官方完整 conformance 测试。
