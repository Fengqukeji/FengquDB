# 实际资源故障验证

`fqdb_store_resource_probe` 只使用生产版 Store 的公开 C/C++ 接口，不链接测试内核，不启用 FQDB_TESTING 故障注入。`scripts/verify-store-resources.py` 在独立 Docker 容器中执行探针，保存宿主观察到的确认日志、容器限制与退出原因、恢复校验结果和源码/二进制哈希。

## 覆盖范围

| 场景 | 实际限制与验证 |
| --- | --- |
| 追加时文件系统写满 | 16 MiB tmpfs 写至内核返回 ENOSPC；保留 2 MiB 预分配文件，失败后释放以提供恢复空间。重开后核对已确认记录及多键事务，继续同步提交并校验 |
| 整理时文件系统写满 | 将剩余空间填至 64 KiB，迫使未发布候选写入失败；检查 CURRENT 未变、快照有效、候选清理和原 writer 可用，释放空间后重试整理及完整备份 |
| 可恢复分配失败 | RLIMIT_AS 限制实际虚拟地址空间，实际分配压力耗尽后通过 C ABI 暂存大值，要求 NO_MEMORY、失败事务不能提交、其他已提交数据不变，释放压力后可开启新事务 |
| 内核 OOM 强杀 | 96 MiB cgroup 内存上限、禁用容器额外交换空间，业务提交之间持续触碰并保留内存页，要求 Docker 确认 OOMKilled 和退出码 137，再从同一卷重开 |
| 宿主外部 SIGKILL | 默认三轮，宿主在观察到提交开始事件后向指定容器发送 KILL，要求退出码 137 且 OOMKilled=false，再从同一卷重开 |

每笔业务事务同步写入 Protobuf 载荷、镜像记录及递增 head，共三条记录。确认仅在默认 SYNC 提交返回成功后发出。宿主收集全部已观察确认；恢复必须至少包含该完整前缀，每条载荷与镜像精确匹配，不能留下事务的一部分。允许完整但尚未向宿主确认的尾部事务。恢复后再提交、检查点并关闭重开，验证可写性和恢复幂等性。

tmpfs 产生真实文件系统空间错误，但没有模拟物理磁盘断电。SIGKILL/OOM 是进程故障，不会切断宿主内核和设备缓存。此验证不替代真实硬件、文件系统掉电或长期生产验收，也不构成 2 核/4 GiB 最大吞吐结论。

## 构建与执行

在 Linux 构建可选探针；正常库构建默认不包含它，也不安装到 SDK：

```sh
cmake -S . -B build/resource-linux -DCMAKE_BUILD_TYPE=Release \
  -DFQDB_BUILD_RESOURCE_PROBE=ON -DFQDB_BUILD_TOOLS=OFF \
  -DFQDB_BUILD_DIAGNOSTICS=OFF -DBUILD_TESTING=OFF
cmake --build build/resource-linux --target fqdb_store_resource_probe --parallel 2
python3 scripts/verify-store-resources.py \
  --probe build/resource-linux/fqdb_store_resource_probe \
  --library build/resource-linux/libfengqudb.so.0.3.2 \
  --output build/resource-run
```

需要可访问的 Linux amd64 Docker 引擎和 `ubuntu:26.04` 镜像；运行记录固定实际镜像 ID。容器关闭网络、只读根文件系统、移除 capabilities，只开放独立 `/data` 写入。各场景分配 2 核；普通与分配失败场景使用 512 MiB，强杀/恢复为 256 MiB，OOM 场景为 96 MiB，并记录实际生效限制。

Windows 宿主可通过 WSL 构建 Linux 产物，再用 Windows Python 和 Docker CLI 运行脚本，传入 Windows 可访问的产物绝对路径。`--case` 可选择单个场景，`--kill-rounds` 设置 1～5 轮，默认三轮；每次输出目录必须不存在。

运行只创建带本轮唯一标签的容器/卷，清理前再次核对所有权，成功后删除已停止的自有容器及卷。失败保留相关资源和日志，`report.json` 记录名称及清理状态；超时记为失败并停止该容器，不自动重启重试。探针拒绝非 `/data` 路径及非恢复场景下已有的数据库根目录。

## 默认日志保留压力

`scripts/verify-diagnostics-retention.py` 通过公开 C ABI 打开实际日志后端，再用真实 Store 操作生成日志。测试临时记录全部成功提交，但保持默认 8 MiB 文件轮转、64 MiB 总量和 7 天保留策略，不调用 configure 降低上限，也不调用 clear。

```sh
python3 scripts/verify-diagnostics-retention.py \
  --library build/resource-linux/libfengqudb.so.0.3.2 \
  --diagnostics build/resource-linux/libfengqudb_diagnostics.so \
  --output build/diagnostics-retention
```

该场景须构建可选诊断模块，即配置 `FQDB_BUILD_DIAGNOSTICS=ON`。Windows 可传开发版 DLL 的绝对路径。脚本生成足量事件直到首个实际日志文件被总量策略回收，检查全部存量 JSONL、占用和单文件上限、队列计数、业务数据隐私、无关文件保留及只读诊断查询；重复 PutIfChanged 不改变值，要求业务 WAL 不随日志压力增长。另放入时间戳为 8 天前的自有旧格式文件验证过期规则，不能将其表述为运行了 8 天。

本批实际结果、构建与资源清理证据见 [资源故障与日志压力记录](RESOURCE_REVIEW_20261107.md)。
