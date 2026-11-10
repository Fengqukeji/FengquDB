"""Verify optional Python and Java output reuse against production libraries."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--java-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csharp", action="store_true", help="Also build/run shared C# reader checks")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    library = args.library.resolve(strict=True)
    java = args.java_bin.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    classes = output / "classes"
    classes.mkdir()
    suffix = ".exe" if os.name == "nt" else ""
    jni = library.parent / ("fengqudb_jni.dll" if os.name == "nt" else "libfengqudb_jni.so")
    files = [Path(__file__).resolve(), root / "bindings/python/fengqudb.py",
              root / "bindings/java/io/fengqudb/Store.java",
             root / "bindings/java/store_jni.cpp", root / "tests/java/StoreBufferSmoke.java",
             root / "tests/python_store_buffer.py", root / "tests/store_shared_smoke.py",
             root / "tests/java/StoreSharedSmoke.java"]
    if args.csharp:
        files += [root / "tests/csharp/StoreSharedSmoke.cs", root / "tests/csharp/StoreSharedSmoke.csproj"]
        files += list((root / "bindings/csharp").glob("*.cs")) + [root / "bindings/csharp/FengquDB.csproj"]
    def manifest():
        return {p.relative_to(root).as_posix(): digest(p) for p in files}
    report = {"ok": False, "source_manifest": manifest(), "library_sha256": digest(library),
              "jni_sha256": digest(jni), "commands": []}
    def run(label, command):
        result = subprocess.run(list(map(str, command)), cwd=root, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=90)
        (output / (label + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        row = {"name": label, "command": list(map(str, command)), "exit_code": result.returncode}
        print(json.dumps(row), flush=True)
        if result.returncode:
            raise RuntimeError(label + " failed: " + result.stdout + result.stderr)
        return row
    try:
        report["commands"].append(run("javac", [java / ("javac" + suffix), "-encoding", "UTF-8", "-d", classes,

                                               root / "bindings/java/io/fengqudb/Store.java",
                                               root / "tests/java/StoreBufferSmoke.java", root / "tests/java/StoreSharedSmoke.java"]))
        commands = [("python", [sys.executable, root / "tests/python_store_buffer.py", output / "python", library]),
                    ("python-fallback", [sys.executable, root / "tests/python_store_buffer.py", output / "python-fallback", library, "--fallback"]),
                    ("java", [java / ("java" + suffix), "-Xcheck:jni", "-Djava.library.path=" + str(library.parent),
                              "-cp", classes, "StoreBufferSmoke", output / "java"])]
        commands += [("python-shared", [sys.executable, root / "tests/store_shared_smoke.py", output / "python-shared", library]),
                     ("java-shared", [java / ("java" + suffix), "-Xcheck:jni", "-Djava.library.path=" + str(library.parent), "-cp", classes, "StoreSharedSmoke", output / "java-shared"]),
                     ("java-shared-copy", [java / ("java" + suffix), "-Xcheck:jni", "-Dio.fengqudb.shared.copy=true", "-Djava.library.path=" + str(library.parent), "-cp", classes, "StoreSharedSmoke", output / "java-shared-copy"])]
        if args.csharp:
            managed = output / "csharp"
            report["commands"].append(run("csharp-build", ["dotnet", "build", root / "tests/csharp/StoreSharedSmoke.csproj", "-c", "Release", "-o", managed, "-p:UseSharedCompilation=false"]))
            commands.append(("csharp-shared", ["dotnet", managed / "StoreSharedSmoke.dll", output / "csharp-shared", library]))
        failures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
            futures = [executor.submit(run, *row) for row in commands]
            for result in futures:
                try:
                    report["commands"].append(result.result())
                except BaseException as error:
                    failures.append(str(error))
        if failures:
            raise RuntimeError("; ".join(failures))
        if manifest() != report["source_manifest"]:
            raise RuntimeError("Source changed during validation")
        report["ok"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS optional output reuse and lifecycle boundaries", flush=True)


if __name__ == "__main__":
    main()
