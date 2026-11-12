"""unittest entry for the Python query binding regressions.

Runs tests/store_select_bindings.py and tests/store_query_builder.py against the native
library named by FQDB_LIBRARY, or the newest fengqudb shared library under build/.
Usage: python -m unittest discover -s tests -p test_store_query_python.py
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import store_query_builder  # noqa: E402
import store_select_bindings  # noqa: E402


def _library():
    explicit = os.environ.get("FQDB_LIBRARY")
    if explicit:
        return Path(explicit)
    names = ("fengqudb.dll", "libfengqudb.so", "libfengqudb.dylib")
    found = [p for p in (HERE.parent / "build").rglob("*") if p.name in names and p.is_file()]
    if not found:
        raise unittest.SkipTest("no native fengqudb library; set FQDB_LIBRARY")
    return max(found, key=lambda p: p.stat().st_mtime)


class PythonQueryBindings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = str(_library())

    def test_select_bindings(self):
        with tempfile.TemporaryDirectory() as root:
            store_select_bindings.run(Path(root) / "select", self.library)

    def test_typed_builder(self):
        with tempfile.TemporaryDirectory() as root:
            store_query_builder.run(Path(root) / "builder", self.library)


if __name__ == "__main__":
    unittest.main()
