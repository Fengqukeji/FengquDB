# Binary size comparison / 二进制体积对比

Checked on 2026-11-15. All MiB figures use 1 MiB = 1,048,576 bytes.

| Item / 项目 | Exact bytes / 字节 | MiB |
| --- | ---: | ---: |
| FengquDB Windows x64 LTO validation / 验证包 `fengqudb.dll` | 1,128,960 | 1.0767 |
| FengquDB original benchmark build / 原测试版本 Windows x64 DLL | 1,206,272 | 1.1504 |
| MySQL Community Server 8.4.13 Windows x64 `bin/mysqld.exe` | 51,644,544 | 49.2521 |
| MySQL Community Server 8.4.13 complete Windows x64 ZIP | 281,154,912 | 268.1302 |

The homepage compares **one uncompressed core DLL with one uncompressed server executable**, excluding other tools, dependencies and data on both sides. The MySQL executable is 45.7452 times the size, so “approximately 1/46” describes the file-size ratio. The programs have different feature scopes: the MySQL server includes SQL and service capabilities that FengquDB's embedded core does not provide. This is a deployment-footprint comparison, not equal feature coverage or a claim that the entire FengquDB SDK is 1.08 MiB.

首页比较的是**未压缩的单个核心 DLL 与单个服务端主程序**，双方其他工具、依赖和数据均不计。MySQL 主程序为本库 DLL 的 45.7452 倍，因此可写“体积约为 1/46”。MySQL 还提供本库核心没有的完整 SQL 和服务端能力，功能范围不同；这说明交付体积差异，不表示功能完全等价，也不表示完整 SDK 只有 1.08 MiB。

The homepage size is from the Windows x64 LTO validation build; the public throughput charts retain the earlier frozen benchmark build. [LTO size and validation evidence](evidence/windows-lto-size-20261115.json).

首页体积来自 Windows x64 LTO 验证包；公开吞吐图仍使用此前冻结实测版本。[LTO 体积及验证记录](evidence/windows-lto-size-20261115.json)。

## Source and method / 来源与方法

- [Official MySQL Windows downloads / 官方下载页](https://dev.mysql.com/downloads/mysql/8.4.html?os=3): ZIP `mysql-8.4.13-winx64.zip`, listed as 268.1M; debug/test ZIP excluded.
- [Official ZIP / 官方 ZIP](https://cdn.mysql.com/Downloads/MySQL-8.4/mysql-8.4.13-winx64.zip).
- Used two bounded HTTP range requests to read the ZIP's end record and central directory: **103,312 metadata bytes** in total. The central directory provides the uncompressed size of `mysql-8.4.13-winx64/bin/mysqld.exe`. The archive was not downloaded in full, installed or executed.
- 仅通过两个有界 HTTP Range 请求读取 ZIP 尾记录和中央目录，总计约 101 KiB 元数据；从中央目录取得主程序的未压缩大小。未完整下载、安装或运行 MySQL。
- [Machine-readable evidence / 机器可读记录](evidence/mysql-windows-size-20261115.json) includes the archive byte count, ranges and central-directory hash.

The package version used for size verification is 8.4.13. The business benchmark used MySQL 8.4.11; package-size verification and throughput measurement are separate evidence.

体积核查使用 8.4.13 官方发行包；性能测试使用 8.4.11。二者是独立证据，不能把体积核查当作重新做过性能测试。
