"""Isolated combined-folder import, with no global module aliases or loader environment."""

import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = os.environ.get("FENGQUDB_LIBRARY")
INDEX = os.environ.get("FENGQUDB_INDEX_LIBRARY")


class CombinedPackageTests(unittest.TestCase):
    def test_copy_one_folder_import_and_cli_without_environment(self):
        if not CORE or not INDEX:
            raise RuntimeError("combined package verification requires the paired Core and Index")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            package = root / "FengquDB"
            (package / "bindings/python").mkdir(parents=True)
            for path in (package, package / "bindings", package / "bindings/python"):
                (path / "__init__.py").write_text("", encoding="utf-8")
            for name in ("fengqudb.py", "fengqudb_index.py", "fengqudb_index_wire.py"):
                shutil.copyfile(ROOT / "bindings/python" / name, package / "bindings/python" / name)
            shutil.copytree(ROOT / "llmwiki", package / "llmwiki", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
            architecture = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}[platform.machine().lower()]
            system = "win" if os.name == "nt" else "linux"
            native = package / "native" / (system + "-" + architecture)
            native.mkdir(parents=True)
            names = ("fengqudb.dll", "fengqudb_index.dll") if os.name == "nt" else ("libfengqudb.so.2", "libfengqudb_index.so.1")
            for source, name in zip((CORE, INDEX), names):
                shutil.copyfile(Path(source).resolve(strict=True), native / name)
            environment = {k: v for k, v in os.environ.items()
                if k not in ("PYTHONPATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "FENGQUDB_LIBRARY", "FENGQUDB_INDEX_LIBRARY", "FQDB_LIBRARY")
                and not k.startswith(("WIKI_MODEL", "WIKI_COMPILER_MODEL", "WIKI_ANSWERER_MODEL"))}
            code = '''import json,sys
from FengquDB.llmwiki import Wiki, NativeIndexProvider
from FengquDB.bindings.python.fengqudb import native_library_path
assert all(name not in sys.modules for name in ("bindings", "fengqudb", "fengqudb_index", "llmwiki"))
with Wiki("data") as wiki:
    assert isinstance(wiki.index,NativeIndexProvider)
    wiki.ingest("package:one","combinedpackageuniqueterm 中文")
    wiki.run_jobs(limit=10)
    assert wiki.ask("combinedpackageuniqueterm")["citation_validated"]
    print(json.dumps({"schema":wiki.status()["schema"],"core":native_library_path(),"index":native_library_path("fengqudb_index")}))
'''
            result = subprocess.run([sys.executable, "-c", code], cwd=root, env=environment,
                                    capture_output=True, text=True, check=True, timeout=30)
            self.assertEqual(2, json.loads(result.stdout)["schema"])
            cli = subprocess.run([sys.executable, "-m", "FengquDB.llmwiki", "--db", "data",
                                  "search", "combinedpackageuniqueterm"], cwd=root, env=environment,
                                 capture_output=True, text=True, check=True, timeout=30)
            self.assertTrue(json.loads(cli.stdout)["results"])
