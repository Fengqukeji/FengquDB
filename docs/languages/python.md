# Python 接入

[English](../en/languages/python.md)

[API、用途和调用例子](../api/python.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 放好 SDK 与自己的类

把 SDK 放进业务项目，文件夹命名为 `FengquDB`：

```text
项目/
  main.py
  student.py
  FengquDB/fengqudb.zip
  FengquDB/__init__.py
  FengquDB/Tools/fqdb-codegen.exe            ← Windows 工具
```

`student.py`：

```python
from dataclasses import dataclass
# fqdb:entity prefix=student/ key=id
@dataclass
class Student:
    id: int = 0
    name: str = ""
    score: int = 0
```

Python 用类型提示识别字段。普通类的 `score=0`、`name=""` 也能识别；`None` 或动态表达式字段请补 `int/str/float/bool/bytes` 等类型提示。

## 2. 在启动时生成并存取

`main.py`，可直接运行：

```python
from pathlib import Path
from FengquDB import Store, generate

project = Path(__file__).resolve().parent
generate(project)  # 开发入口执行一次，修改类后重启应用会更新
from student import Student
from fqdb_generated.student.fq_codecs import StudentFqCodec as Codec

root = project / "save"
root.mkdir(parents=True, exist_ok=True)
with Store(root / "main") as db:
    student = Student(7, "张三", 95)
    db.put(Codec.key(student), Codec.encode(student))
    saved = Codec.decode(db.get(Codec.key(student))).value
    print(saved.name, saved.score)
    db.sync()
```

在项目目录运行 `python main.py`，看到 `张三 95` 即成功。生成代码位于 `Generated/python`，generate 已设置导入路径。generate 在开发启动入口执行一次。如果采用 src 布局，改为 `generate(project, source="src")`。

只存单值时可直接 `db.put(b"name", "张三".encode("utf-8"))`，读取后 `.decode("utf-8")`；整数示例见 API 页。

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

路径由项目设置，使用可写的存档目录。沿用上面加载的 Store，不同子目录就是不同数据库：

```python
root = Path("save")  # 换成宿主提供的存档目录
root.mkdir(parents=True, exist_ok=True)
with Store(root / "players") as players, Store(root / "world") as world:
    players.configure_write(mode="performance", period_ms=50, threshold_bytes=1024 * 1024)
    players.put(b"name", "张三".encode("utf-8"))

    players.configure_write(mode="reliable")  # 这个库改为每笔可靠落盘
    # players.reset_write_options()  # 恢复默认性能模式、50 ms / 1 MiB
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。数据和配置按库独立保存，配置重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。

正式发布前先生成代码，带上实体文件、Generated/python 和 FengquDB 文件夹中的 __init__.py、fengqudb.zip、Native。设置环境变量 `FQDB_CODEGEN_SKIP=1`，generate 仍会设置导入路径。SDK 自动从自己的 Native 目录加载引擎。
