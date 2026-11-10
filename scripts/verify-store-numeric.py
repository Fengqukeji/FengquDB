"""Verify numeric persistence boundaries through production language bindings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--seed-executable", type=Path, required=True)
    parser.add_argument("--java-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--data-root", type=Path, help="Explicit native-filesystem fixture root")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    data = args.data_root.resolve() if args.data_root else output / "fixtures"
    data.mkdir(parents=True, exist_ok=False)
    library, seed = args.library.resolve(strict=True), args.seed_executable.resolve(strict=True)
    java = args.java_bin.resolve(strict=True)
    jni = library.parent / ("fengqudb_jni.dll" if os.name == "nt" else "libfengqudb_jni.so")
    files = [Path(__file__).resolve(), root / "tests/store_limits_tests.cpp",
             root / "tests/java/StoreLimitsSmoke.java", root / "tests/csharp/StoreLimits.cs",
             root / "tests/csharp/StoreLimits.csproj", root / "bindings/python/fengqudb.py",
             root / "bindings/java/store_jni.cpp", root / "bindings/java/io/fengqudb/Store.java"]
    for directory in (root / "src", root / "include", root / "bindings/csharp"):
        files += [p for p in directory.rglob("*") if p.is_file() and
                  not any(x in ("bin", "obj") for x in p.parts) and
                  p.suffix in (".cpp", ".hpp", ".h", ".cs", ".csproj")]
    def manifest():
        return {p.relative_to(root).as_posix(): digest(p) for p in sorted(set(files))}
    report = {"ok": False, "source_manifest": manifest(), "commands": [], "data_root": str(data),
              "library_sha256": digest(library), "jni_sha256": digest(jni), "seed_sha256": digest(seed)}
    def run(label, command):
        result = subprocess.run(list(map(str, command)), cwd=root, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=120)
        (output / (label + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        row = {"name": label, "command": list(map(str, command)), "exit_code": result.returncode}
        report["commands"].append(row)
        print(json.dumps(row), flush=True)
        if result.returncode:
            raise RuntimeError(label + " failed: " + result.stdout + result.stderr)
    try:
        suffix = ".exe" if os.name == "nt" else ""
        classes = output / "classes"; classes.mkdir()
        run("seed-high-version", [seed, "--seed", data / "main", (1 << 63) - 3, 1, 1])
        run("javac", [java / ("javac" + suffix), "-encoding", "UTF-8", "-d", classes,

                      root / "bindings/java/io/fengqudb/Store.java", root / "tests/java/StoreLimitsSmoke.java"])
        run("java", [java / ("java" + suffix), "-Xcheck:jni", "-Djava.library.path=" + str(library.parent),
                     "-cp", classes, "StoreLimitsSmoke", data])
        dotnet = shutil.which("dotnet")
        if dotnet:
            csharp = output / "csharp-bin"
            run("csharp-build", [dotnet, "build", root / "tests/csharp/StoreLimits.csproj", "-c", "Release",
                                 "--nologo", "-o", csharp])
            run("csharp", [dotnet, csharp / "StoreLimits.dll", data, library])
        else:
            report["csharp"] = "not run; no dotnet SDK on this platform"
        sys.path.insert(0, str(root / "bindings/python"))
        from fengqudb import Store, Error
        with Store.open_at(data, "main", library=library, create=False) as db:
            entry = db.get_entry(b"key")
            assert entry.version == (1 << 63) + (3 if dotnet else 2)
            assert entry.value == (b"D" if dotnet else b"C")
            assert db.add_i64(b"maximum", 0 if dotnet else (1 << 63) - 1).value == (1 << 63) - 1
            assert db.add_i64(b"minimum", 0 if dotnet else -(1 << 63)).value == -(1 << 63)
            before = db.info().version
            for key, delta in ((b"maximum", 1), (b"minimum", -1)):
                try: db.add_i64(key, delta)
                except Error as error: assert error.code == 20
                else: raise AssertionError("Signed counter overflow accepted")
            assert db.info().version == before
            with db.watch() as watch:
                result = db.compare_exchange(b"key", entry.version, b"E")
                assert result.matched
                change = watch.next_batch()
                assert change.version == result.version > (1 << 63)
                watch.ack(change.version)
            db.verify()
        with Store.open_at(data, "main", library=library, create=False) as db:
            assert db.get(b"key") == b"E"
        with Store.open_at(data, "copy", library=library, create=False) as db:
            assert db.get_entry(b"key").version == (1 << 63) + 2 and db.get(b"key") == b"C"
        report["python_unsigned_and_counters"] = True

        # Reproduce the original bug with the production C ABI. Only the CRC'd
        # sequence in an owned fixture's selected superblock is changed.
        with Store.open_at(data, "exhausted", library=library) as db:
            db.put_if_changed(b"old", b"confirmed"); db.checkpoint()
        file = next((data / "exhausted").glob("g*.fdb"))
        contents = bytearray(file.read_bytes())
        blocks = [(struct.unpack_from("<Q", contents, at + 8)[0], at) for at in (4096, 8192)
                  if contents[at:at + 8] == b"FQROOT04"]
        _, at = max(blocks)
        struct.pack_into("<Q", contents, at + 8, (1 << 64) - 1)
        struct.pack_into("<I", contents, at + 60, zlib.crc32(contents[at:at + 60]))
        file.write_bytes(contents)
        with Store.open_at(data, "exhausted", library=library, create=False) as db:
            try: db.put_if_changed(b"new", b"must-reject")
            except Error as error: assert error.code == 9
            else: raise AssertionError("Exhausted sequence accepted WAL")
            assert db.get(b"old") == b"confirmed" and db.get(b"new") is None
            db.checkpoint(); db.verify()
        assert file.read_bytes() == contents
        with Store.open_at(data, "exhausted", library=library, create=False) as db:
            db.compact(); db.put_if_changed(b"new", b"after-compaction"); db.verify()
        with Store.open_at(data, "exhausted", library=library, create=False) as db:
            assert db.get(b"new") == b"after-compaction" and db.get(b"old") == b"confirmed"
        report["exhausted_sequence_production_api"] = True
        if manifest() != report["source_manifest"]:
            raise RuntimeError("Source changed during numeric verification")
        report["ok"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS production unsigned-version interoperability and numeric exhaustion", flush=True)


if __name__ == "__main__":
    main()
