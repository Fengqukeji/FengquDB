"""Assemble a relocatable stdlib-only deployment ZIP with one native runtime."""

import argparse
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
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
        output.write(library, "Native/" + runtime_name)
        output.write(root / "docs/LLMWIKI.md", "README.md")
        output.write(root / "docs/LLMWIKI_VALIDATION.md", "VALIDATION.md")
        for evidence in sorted((root / "docs/evidence/llmwiki").glob("*.json")):
            output.write(evidence, "evidence/llmwiki/" + evidence.name)
        output.writestr("run-wiki.py", "import os\nfrom pathlib import Path\nos.environ.setdefault('FENGQUDB_LIBRARY', str(Path(__file__).resolve().parent / 'Native' / '" + runtime_name + "'))\nfrom llmwiki.__main__ import main\nraise SystemExit(main())\n")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
