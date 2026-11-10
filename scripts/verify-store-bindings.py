"""Verify new Store bindings against a separately built native library.

The runner never installs into a released SDK. Successful smoke fixtures remove
only their own known files; failed fixtures and logs stay in the output directory.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--java-bin", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--segmented", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    library = args.library.resolve(strict=True)
    java = args.java_bin or (Path(shutil.which("javac")).parent if shutil.which("javac") else None)
    if java is None: parser.error("Pass --java-bin or add the JDK to PATH")
    suffix = ".exe" if os.name == "nt" else ""
    javac, java_runtime = java / ("javac" + suffix), java / ("java" + suffix)
    if not javac.is_file() or not java_runtime.is_file(): parser.error("The selected JDK bin directory is incomplete")
    output = (args.output or root / "build" / ("store-bindings-" + str(time.time_ns()))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    classes = output / "classes"
    classes.mkdir()
    report = {"ok": False, "library": str(library), "commands": []}
    env = dict(os.environ, DOTNET_CLI_UI_LANGUAGE="en",
               FQDB_TEST_PYTHON=sys.executable,
               FQDB_TEST_CLEANER=str(root/'tests/store_fixture_cleanup.py'),
               FQDB_TEST_OWNED_ROOT=str(output), FQDB_TEST_SEGMENTED='1' if args.segmented else '0')
    def run(label, command):
        result = subprocess.run([str(arg) for arg in command], cwd=root, env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        log = output / (label + ".log")
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        row = {"name": label, "exit_code": result.returncode, "log": str(log)}
        print(json.dumps(row), flush=True)
        if result.returncode: raise RuntimeError(f"{label} failed: {log}\n{result.stdout}\n{result.stderr}")
        return row
    python = [sys.executable, root / "tests" / "store_bindings_smoke.py"]
    managed_dll = root / "build" / "store-csharp-bin" / "net10.0" / "StoreSmoke.dll"
    java_command = [java_runtime, "-Djava.library.path=" + str(library.parent), "-cp", classes, "StoreSmoke"]
    try:
        builds = [
            ("csharp-build", ["dotnet", "build", root / "tests/csharp/StoreSmoke.csproj", "-c", "Release", "--nologo"]),
            ("java-build", [javac, "-encoding", "UTF-8", "-d", classes,
                            root / "bindings/java/io/fengqudb/Store.java", root / "tests/java/StoreSmoke.java"]),
        ]
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
            futures = [executor.submit(run, *build) for build in builds]
            for future in futures: report["commands"].append(future.result())
            checks = [
                ("python-smoke", python + [output / "python-db", library]),
                ("csharp-smoke", ["dotnet", managed_dll, output / "csharp-db", library]),
                ("java-smoke", java_command + [output / "java-db"]),
            ]
            futures = [executor.submit(run, *check) for check in checks]
            for future in futures: report["commands"].append(future.result())
        shared = output / "shared-跨语言"
        for label, command in [
            ("interop-python-seed", python + [shared, library, "seed"]),
            ("interop-java", java_command + [shared, "interop"]),
            ("interop-csharp", ["dotnet", managed_dll, shared, library, "interop"]),
            ("interop-python-verify", python + [shared, library, "verify"]),
        ]:
            report["commands"].append(run(label, command))
        report["ok"] = True
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS Store Python/C#/Java smoke and shared-directory Protobuf interoperability", flush=True)

if __name__ == "__main__": main()
