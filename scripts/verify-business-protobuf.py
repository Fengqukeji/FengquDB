"""Generate official business models, verify schema evolution, measure app costs.

No dependency is installed into the database core. Pass a Python environment
containing protobuf 7.36.2, a protobuf-java 4.36.2 jar and official protoc 36.2.
The C# example references Google.Protobuf 3.36.2 in its own business project.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(root):
    files = [Path(__file__).resolve(), root / "CMakeLists.txt", root / "tools/store_binding_benchmark.cpp", root / "examples/unity/TypedFengquSave.cs", root / "bindings/python/fengqudb.py", root / "bindings/java/store_jni.cpp",
             root / "bindings/java/io/fengqudb/Store.java"]
    for directory in (root / "src", root / "include", root / "bindings/csharp", root / "examples/protobuf"):
        files.extend(p for p in directory.rglob("*") if p.is_file() and
                     not any(x in ("bin", "obj", "__pycache__") for x in p.parts) and
                     p.suffix in (".cpp", ".hpp", ".h", ".cs", ".csproj", ".java", ".py", ".proto"))
    return {p.relative_to(root).as_posix(): digest(p) for p in sorted(set(files))}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protoc", type=Path, required=True)
    parser.add_argument("--java-bin", type=Path, required=True)
    parser.add_argument("--java-runtime", type=Path, required=True, help="protobuf-java jar")
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--native-benchmark", type=Path, help="optional same-semantics native benchmark executable")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=100000)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if args.iterations < 10000 or not 1 <= args.runs <= 10:
        parser.error("Use iterations >=10000 and 1..10 runs")
    root = Path(__file__).resolve().parents[1]
    for name in ("protoc", "java_runtime", "python", "library", "java_bin"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    if args.native_benchmark:
        args.native_benchmark = args.native_benchmark.resolve(strict=True)
    suffix = ".exe" if os.name == "nt" else ""
    java, javac = (args.java_bin / (name + suffix) for name in ("java", "javac"))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    generated = output / "generated"
    for name in ("csharp", "java", "python", "classes", "csharp-bin"):
        (generated / name).mkdir(parents=True)
    report = {"ok": False, "commands": [], "raw": [], "host": platform.platform(),
              "iterations": args.iterations, "runs": args.runs,
              "source_manifest": manifest(root),
              "protoc_sha256": digest(args.protoc), "jar_sha256": digest(args.java_runtime),
              "library_sha256": digest(args.library)}
    jni = args.library.parent / ("fengqudb_jni.dll" if os.name == "nt" else "libfengqudb_jni.so")
    report["jni_sha256"] = digest(jni)
    if args.native_benchmark:
        report["native_benchmark_sha256"] = digest(args.native_benchmark)
    environment = dict(os.environ, DOTNET_CLI_UI_LANGUAGE="en", PYTHONUTF8="1")

    def run(label, command):
        started = time.monotonic()
        result = subprocess.run(list(map(str, command)), cwd=root, env=environment,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        (output / (label + ".stdout.log")).write_text(result.stdout, encoding="utf-8")
        (output / (label + ".stderr.log")).write_text(result.stderr, encoding="utf-8")
        row = {"name": label, "command": list(map(str, command)), "exit_code": result.returncode,
               "elapsed_seconds": time.monotonic() - started}
        report["commands"].append(row)
        print(json.dumps({k: row[k] for k in ("name", "exit_code", "elapsed_seconds")}), flush=True)
        if result.returncode:
            raise RuntimeError(f"{label} failed; inspect {output / (label + '.stderr.log')}\n{result.stdout}\n{result.stderr}")
        return row, result.stdout

    def execute(label, command):
        row, text = run(label, command)
        return text

    try:
        report["versions"] = {
            "protoc": execute("protoc-version", [args.protoc, "--version"]).strip(),
            "python": execute("python-version", [args.python, "-c", "import sys,google.protobuf;from google.protobuf.internal import api_implementation;print(sys.version);print(api_implementation.Type());print(google.protobuf.__version__)"]).strip(),
            "dotnet": execute("dotnet-version", ["dotnet", "--version"]).strip(),
        }
        execute("java-version", [java, "-version"])
        report["versions"]["java"] = (output / "java-version.stderr.log").read_text(encoding="utf-8").strip()
        if report["versions"]["protoc"] != "libprotoc 36.2" or not report["versions"]["python"].endswith("7.36.2"):
            raise RuntimeError("Use pinned protoc 36.2 and Python protobuf 7.36.2 for this example")
        execute("generate", [args.protoc, "-I" + str(root / "examples/protobuf"),
                             "--csharp_out=" + str(generated / "csharp"),
                             "--java_out=" + str(generated / "java"),
                             "--python_out=" + str(generated / "python"),
                             "--descriptor_set_out=" + str(generated / "models.pb"),
                             "--include_imports", "player_v1.proto", "player_v2.proto"])
        unity_stub = generated / "UnityStubs.cs"
        unity_stub.write_text("namespace UnityEngine { public class MonoBehaviour {} public static class Application { public static string persistentDataPath { get; set; } = string.Empty; } }\n", encoding="utf-8")
        report["generated_manifest"] = {p.relative_to(generated).as_posix(): digest(p)
                                          for p in sorted(generated.rglob("*")) if p.is_file()}
        csharp_bin = generated / "csharp-bin"
        csharp_build = ["dotnet", "build", root / "examples/protobuf/csharp/PlayerExample.csproj",
                        "-c", "Release", "--nologo", "-o", csharp_bin,
                        "-p:GeneratedDir=" + str(generated / "csharp"), "-p:UnityStub=" + str(unity_stub)]
        java_build = [javac, "-encoding", "UTF-8", "-cp", args.java_runtime,
                      "-d", generated / "classes",
                      root / "bindings/java/io/fengqudb/Store.java",
                      *sorted((generated / "java").rglob("*.java")),
                      root / "examples/protobuf/java/PlayerStoreExample.java"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            builds = [executor.submit(run, "csharp-build", csharp_build), executor.submit(run, "java-build", java_build)]
            for future in builds:
                future.result()
        shared = output / "shared-\u8de8\u8bed\u8a00"
        fixture = output / "player-fixture.pb"
        py_base = [args.python, root / "examples/protobuf/python/player_store.py"]
        py_options = ["--generated", generated / "python", "--library", args.library]
        cs_base = ["dotnet", csharp_bin / "PlayerExample.dll"]
        java_base = [java, "-Xms64m", "-Xmx256m", "-Djava.library.path=" + str(args.library.parent),
                     "-cp", str(generated / "classes") + os.pathsep + str(args.java_runtime), "PlayerStoreExample"]
        execute("python-seed-v2", py_base + ["seed", *py_options, "--root", shared, "--fixture", fixture])
        execute("csharp-rewrite-v1", cs_base + ["rewrite", shared, args.library])
        execute("java-rewrite-v1", java_base + ["rewrite", shared])
        execute("csharp-inspect-v2", cs_base + ["inspect", shared, args.library])
        execute("csharp-unity-component", cs_base + ["unity-smoke", shared, args.library])
        execute("python-verify-v2", py_base + ["verify", *py_options, "--root", shared])
        report["fixture_bytes"] = fixture.stat().st_size
        report["fixture_sha256"] = digest(fixture)
        for iteration in range(args.runs):
            languages = ["python", "csharp", "java"]
            if args.native_benchmark:
                languages.insert(0, "native")
            order = languages if iteration % 2 == 0 else list(reversed(languages))
            for language in order:
                database = output / ("bench-" + language + "-" + str(iteration))
                if language == "native":
                    command = [args.native_benchmark, database, fixture, args.iterations]
                elif language == "python":
                    command = py_base + ["bench", *py_options, "--root", database, "--fixture", fixture,
                                         "--iterations", args.iterations]
                elif language == "csharp":
                    command = cs_base + ["bench", database, args.library, fixture, args.iterations]
                else:
                    command = java_base + ["bench", database, fixture, args.iterations]
                stdout = execute("bench-" + language + "-" + str(iteration), command)
                rows = [json.loads(line) for line in stdout.splitlines()]
                expected = ({"get_copy", "prepared_get_into", "get_many_32"} if language == "native" else
                            {"serialize", "parse", "get_copy", "prepared_get_into", "get_parse", "get_many_32"})
                if language == "python":
                    expected.add("prepared_bound_buffer")
                elif language == "java":
                    expected.add("prepared_direct_buffer")
                if len(rows) != len(expected) or {r["workload"] for r in rows} != expected:
                    raise RuntimeError("Benchmark workload coverage mismatch")
                report["raw"].extend(dict(row, run=iteration) for row in rows)
        medians = []
        for workload in sorted({r["workload"] for r in report["raw"]}):
            comparable = [r for r in report["raw"] if r["workload"] == workload]
            if len({(r["calls"], r["units_per_call"], r["checksum"]) for r in comparable}) != 1:
                raise RuntimeError("Cross-language workload units or checksums differ")
            for language in ("native", "python", "csharp", "java"):
                rows = [r for r in report["raw"] if r["workload"] == workload and r["language"] == language]
                if not rows:
                    continue
                if len(rows) != args.runs or len({r["checksum"] for r in rows}) != 1:
                    raise RuntimeError("Inconsistent measured workload results")
                medians.append({"workload": workload, "language": language,
                                "ops_per_second": statistics.median(r["ops_per_second"] for r in rows),
                                "p99_us": statistics.median(r["p99_us"] for r in rows),
                                "units_per_call": rows[0]["units_per_call"]})
        report["medians"] = medians
        if manifest(root) != report["source_manifest"]:
            raise RuntimeError("Source changed during business-model verification")
        report["ok"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS generated business Protobuf models, schema evolution, constraints and per-language cost measurements", flush=True)


if __name__ == "__main__":
    main()
