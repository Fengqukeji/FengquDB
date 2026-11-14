"""Run the owning Python suites with machine-readable, zero-skip evidence."""

import argparse
import hashlib
import json
import os
import platform
import sys
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", choices=("wiki", "index", "all"), default="all")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    patterns = {"wiki": ["test_llmwiki*.py"], "index": ["test_fengqudb_index_python.py"],
                "all": ["test_fengqudb_index_python.py", "test_llmwiki*.py"]}[args.suite]
    suite = unittest.TestSuite()
    for pattern in patterns:
        suite.addTests(unittest.defaultTestLoader.discover(str(ROOT / "tests"), pattern))
    start = time.perf_counter()
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    def library(name):
        value = os.environ.get(name)
        if not value:
            return None
        path = Path(value).resolve(strict=True)
        return {"path": str(path), "bytes": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    report = {"suite": args.suite, "tests": result.testsRun, "failures": len(result.failures),
              "errors": len(result.errors), "skipped": len(result.skipped),
              "passed_without_skips": result.wasSuccessful() and not result.skipped,
              "elapsed_seconds": round(time.perf_counter() - start, 6), "platform": platform.platform(),
              "python": platform.python_version(), "core": library("FENGQUDB_LIBRARY"),
              "index": library("FENGQUDB_INDEX_LIBRARY")}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return 0 if report["passed_without_skips"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
