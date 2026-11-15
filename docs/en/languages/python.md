# Python getting started

[简体中文](../../languages/python.md)

[API and examples](../api/python.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Place the SDK and your class

Place the SDK in your application project under a folder named `FengquDB`:

```text
project/
  main.py
  student.py
  FengquDB/fengqudb.zip
  FengquDB/__init__.py
  FengquDB/Tools/fqdb-codegen.exe            <- Windows tool
```

`student.py`:

```python
from dataclasses import dataclass
# fqdb:entity prefix=student/ key=id
@dataclass
class Student:
    id: int = 0
    name: str = ""
    score: int = 0
```

Python fields are identified by type hints. Simple class defaults such as `score=0` and `name=""` also work. Add type hints such as `int/str/float/bool/bytes` for fields with `None` or dynamic expressions.

## 2. Generate at startup, then save and read

Create `main.py` and run it directly:

```python
from pathlib import Path
from FengquDB import Store, generate

project = Path(__file__).resolve().parent
generate(project)  # Once at development startup; restart after changing an entity
from student import Student
from fqdb_generated.student.fq_codecs import StudentFqCodec as Codec

root = project / "save"
root.mkdir(parents=True, exist_ok=True)
with Store(root / "main") as db:
    student = Student(7, "Alice", 95)
    db.put(Codec.key(student), Codec.encode(student))
    saved = Codec.decode(db.get(Codec.key(student))).value
    print(saved.name, saved.score)
    db.sync()
```

Run `python main.py` from the project directory; `Alice 95` confirms success. Generated code is under `Generated/python`, and generate sets the import path. Run generate once at the development entry point. For a src layout, use `generate(project, source="src")`.

For a single value, call `db.put(b"name", "Alice".encode("utf-8"))` directly and decode the returned bytes with `.decode("utf-8")`. See the API page for integers.

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Set a writable data directory in your application. Using the Store imported above, different subdirectories are independent databases:

```python
root = Path("save")  # Replace with the host's writable data directory
root.mkdir(parents=True, exist_ok=True)
with Store(root / "players") as players, Store(root / "world") as world:
    players.configure_write(mode="performance", period_ms=50, threshold_bytes=1024 * 1024)
    players.put(b"name", "Alice".encode("utf-8"))

    players.configure_write(mode="reliable")  # Require durable storage for every write to this database
    # players.reset_write_options()  # Restore default performance mode, 50 ms / 1 MiB
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Each database stores its data and settings separately; settings survive reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle.

Before publishing, generate the code and include your entity files, Generated/python, and the SDK's __init__.py, fengqudb.zip, and Native directory. Set `FQDB_CODEGEN_SKIP=1`; generate still sets the import path. The SDK loads the engine from its own Native directory.
