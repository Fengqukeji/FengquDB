"""Verify an extracted delivery with its bundled binding/runtime, outside the repo."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--native", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        package = root / "package"
        with zipfile.ZipFile(args.archive) as archive:
            # Only accept the artifact layout produced by our own packager.
            for name in archive.namelist():
                if name.startswith(("/", "\\")) or ".." in name.replace("\\", "/").split("/") or ":" in name:
                    raise ValueError("unsafe archive member")
            archive.extractall(package)
        source = root / "sample.md"
        source.write_text("打包验证：数据库支持持久化事务。 packageduniqueterm", encoding="utf-8")
        environment = {k: v for k, v in os.environ.items() if k not in ("FENGQUDB_LIBRARY", "FENGQUDB_INDEX_LIBRARY", "FQDB_LIBRARY", "PYTHONPATH")
                       and not k.startswith(("WIKI_MODEL", "WIKI_COMPILER_MODEL", "WIKI_ANSWERER_MODEL"))}
        environment["PYTHONIOENCODING"] = "utf-8"
        def call(*arguments):
            index_options = ["--index-provider", "native"] if args.native else []
            result = subprocess.run([sys.executable, "run-wiki.py", "--db", str(root / "data"), *index_options, *arguments],
                cwd=package, env=environment, capture_output=True, encoding="utf-8", check=True, timeout=30)
            return json.loads(result.stdout)
        ingested = call("ingest", str(source), "--uri", "package:sample")
        compiled = call("compile")
        answer = call("ask", "packageduniqueterm")
        if not ingested["changed"] or compiled["items"][0]["state"] != "done" or not answer["citation_validated"]:
            raise RuntimeError("package failed ingest/compile/ask")
        call("backup", str(root / "backup"))
        call("verify")
        call("compact")
        print(json.dumps({"package": str(args.archive.resolve()), "ingest": "ok", "compile": "ok",
                          "ask": "ok", "backup": "ok", "verify": "ok", "compact": "ok", "runtime": "bundled"}))


if __name__ == "__main__":
    main()
