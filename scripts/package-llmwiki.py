"""Assemble a relocatable stdlib-only deployment ZIP with one native runtime."""

import argparse
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--index-library", required=True, type=Path, help="paired shared NativeIndex runtime")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    library = args.library.resolve(strict=True)
    runtime_name = ("fengqudb.dll" if library.suffix == ".dll" else
                    "libfengqudb.dylib" if library.suffix == ".dylib" else "libfengqudb.so")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "x", compression=zipfile.ZIP_DEFLATED) as output:
        for path in sorted((root / "llmwiki").glob("*.py")):
            output.write(path, "llmwiki/" + path.name)
        output.write(root / "bindings/python/fengqudb.py", "fengqudb.py")
        output.write(root / "bindings/python/fengqudb_index.py", "fengqudb_index.py")
        output.write(root / "bindings/python/fengqudb_index_wire.py", "fengqudb_index_wire.py")
        output.write(library, "Native/" + runtime_name)
        if args.index_library:
            index_library = args.index_library.resolve(strict=True)
            index_name = "fengqudb_index.dll" if library.suffix == ".dll" else (
                "libfengqudb_index.dylib" if library.suffix == ".dylib" else "libfengqudb_index.so")
            output.write(index_library, "Native/" + index_name)
        output.write(root / "docs/LLMWIKI.md", "README.md")
        output.write(root / "docs/LLMWIKI_INDEX_PROVIDER.md", "LLMWIKI_INDEX_PROVIDER.md")
        output.write(root / "docs/LLMWIKI_NATIVE.md", "LLMWIKI_NATIVE.md")
        output.write(root / "docs/LLMWIKI_VALIDATION.md", "VALIDATION.md")
        for name in ("LLMWIKI_FOUNDATION.md", "LLMWIKI_FOUNDATION_VALIDATION.md"):
            output.write(root / "docs" / name, name)
        for example in sorted((root / "examples/llmwiki").glob("*")):
            if example.is_file():
                output.write(example, "examples/llmwiki/" + example.name)
        for evidence in sorted((root / "docs/evidence/llmwiki").glob("*.json")):
            output.write(evidence, "evidence/llmwiki/" + evidence.name)
        launcher = ("import os\nfrom pathlib import Path\nroot=Path(__file__).resolve().parent\n"
            "os.environ.setdefault('FENGQUDB_LIBRARY', str(root / 'Native' / '" + runtime_name + "'))\n")
        if args.index_library:
            launcher += ("os.environ.setdefault('FENGQUDB_INDEX_LIBRARY', str(root / 'Native' / '" + index_name + "'))\n"
                         "from ctypes import CDLL\ncore_runtime=CDLL(os.environ['FENGQUDB_LIBRARY'])\n")
        output.writestr("run-wiki.py", launcher + "from llmwiki.__main__ import main\nraise SystemExit(main())\n")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
