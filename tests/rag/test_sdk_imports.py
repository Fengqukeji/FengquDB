"""Verify independent SDK namespaces and standalone imports in fresh processes."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--core", required=True)
parser.add_argument("--index", required=True)
parser.add_argument("--shared-python", required=True)
args, remaining = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True


WORK = r'''
import importlib
import sys
sys.dont_write_bytecode = True
prefix = sys.argv[4]
rag = importlib.import_module(prefix + "fengqudb_rag")
core = importlib.import_module(prefix + "fengqudb")
index = importlib.import_module(prefix + "fengqudb_index")
wire = importlib.import_module(prefix + "fengqudb_index_wire")
bindings = importlib.import_module(prefix + "fengqudb_rag._bindings")
assert bindings.Store is core.Store and bindings.StoreError is core.Error
assert bindings.ffi is index and bindings.vector is wire.vector
if prefix:
    facade = importlib.import_module(prefix.split(".bindings.python.")[0] + ".integrations.wordgame_rag")
    assert not any(name in sys.modules for name in ("fengqudb", "fengqudb_index", "fengqudb_index_wire", "fengqudb_rag", "wordgame_rag"))
else:
    facade = importlib.import_module("wordgame_rag")
assert facade.MemoryStore is rag.MemoryStore and facade.RagError is rag.RagError
embedding = rag.CallbackEmbedding(lambda texts, query=False: [[1.0, 0.0, 0.0] for _ in texts],
                                  rag.EmbeddingContract("sdk-import-test", 3))
with rag.MemoryStore(sys.argv[3], embedding=embedding, core_library=sys.argv[1], index_library=sys.argv[2]) as store:
    old = store.put("account", [rag.MemoryWrite(text="needle old", owner_id="npc", fact_key="home", known_at=1)])[0]
    new = store.put("account", [rag.MemoryWrite(text="needle new", owner_id="npc", fact_key="home", known_at=5)])[0]
    for mode in ("dense", "lexical", "hybrid"):
        rows = store.search("account", rag.MemorySearch(query="needle", owner_id="npc", as_of=4,
                                                       include_public=False, mode=mode))
        assert [row["id"] for row in rows] == [old["id"]]
    assert len(store.history("account", new["id"])["items"]) == 2
    store.delete("account", new["id"])
    assert store.stats("account")["records"] == 0
class AppError(RuntimeError):
    pass
store = facade.create_store(sys.argv[3] + "-facade", embed_with_provider=embedding.callback,
    identity="sdk-import-test", dimension=3, core_library=sys.argv[1], index_library=sys.argv[2], error_factory=AppError)
try:
    row = store.put("account", [dict(text="facade evidence")])[0]
    assert store.get("account", row["id"])["text"] == "facade evidence"
    try:
        store.get("other-account", row["id"])
    except AppError:
        pass
    else:
        raise AssertionError("facade did not translate isolated not-found error")
finally:
    store.close()
'''


class SdkImports(unittest.TestCase):
    def run_child(self, folder, paths, prefix):
        env = dict(os.environ, PYTHONPATH=os.pathsep.join(map(str, paths)), PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, "-c", WORK, str(Path(args.core).resolve()),
            str(Path(args.index).resolve()), str(folder / "data"), prefix],
            cwd=folder, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_combined_sdk_uses_siblings_without_global_aliases(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            bindings = folder / "FengquDB/bindings/python"
            bindings.mkdir(parents=True)
            shutil.copy2(ROOT / "bindings/python/fengqudb.py", bindings)
            for name in ("fengqudb_index.py", "fengqudb_index_wire.py"):
                shutil.copy2(Path(args.shared_python) / name, bindings)
            shutil.copytree(ROOT / "bindings/python/fengqudb_rag", bindings / "fengqudb_rag",
                            ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copytree(ROOT / "integrations/wordgame_rag", folder / "FengquDB/integrations/wordgame_rag",
                            ignore=shutil.ignore_patterns("__pycache__"))
            # These unrelated top-level modules must never be imported by the SDK.
            for name in ("fengqudb", "fengqudb_index", "fengqudb_index_wire", "fengqudb_rag", "wordgame_rag"):
                (folder / (name + ".py")).write_text("raise AssertionError('unrelated global module selected')\n", encoding="utf-8")
            self.run_child(folder, [folder], "FengquDB.bindings.python.")

    def test_standalone_source_imports_remain_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.run_child(Path(temporary), [ROOT / "bindings/python", Path(args.shared_python).resolve(), ROOT / "integrations"], "")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + remaining)
