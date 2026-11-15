"""Exercise real default 8 MiB rotation, 64 MiB retention and seven-day expiry.

Generates actual public Store operation events. Never lowers retention limits or
calls clear(). All fixtures stay below the explicitly supplied fresh output root.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings/python"))
from fengqudb import Slice, Store


class Options(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("size", "abi", "level", "flags")] + [
        ("slow_us", C.c_uint64)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest():
    files = [Path(__file__).resolve(), ROOT / "bindings/python/fengqudb.py"]
    for directory in (ROOT / "src", ROOT / "include"):
        files.extend(p for p in directory.rglob("*") if p.suffix in (".cpp", ".hpp", ".h"))
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(set(files))}


class Diagnostics:
    def __init__(self, core_path, library, directory, reader=False):
        self.core = C.CDLL(str(core_path))
        self.core.fqdb_buffer_free.argtypes = [C.POINTER(Slice)]
        self.core.fqdb_buffer_free.restype = None
        self.lib = C.CDLL(str(library))
        pointer = C.c_void_p
        self.handle = pointer()
        for name, arguments in {
            "open": [C.c_char_p, C.POINTER(Options), C.POINTER(pointer)],
            "close": [pointer], "flush": [pointer, C.c_uint32],
            "status": [pointer, C.POINTER(Slice)],
            "query": [pointer, Slice, C.POINTER(Slice)],
        }.items():
            function = getattr(self.lib, "fqdb_diagnostics_" + name)
            function.argtypes, function.restype = arguments, C.c_int32
        # Record all successful commits; disk rotation/retention policy stays default.
        options = Options(C.sizeof(Options), 1, 0, int(reader), 0)
        self.check(self.lib.fqdb_diagnostics_open(str(directory).encode("utf-8"),
                                                C.byref(options), C.byref(self.handle)))

    @staticmethod
    def check(status):
        if status:
            raise RuntimeError("Diagnostic C API status " + str(status))

    def read(self, name, query=None):
        output = Slice()
        function = getattr(self.lib, "fqdb_diagnostics_" + name)
        if query is None:
            status = function(self.handle, C.byref(output))
        else:
            encoded = json.dumps(query).encode("utf-8")
            owner = C.create_string_buffer(encoded)
            status = function(self.handle, Slice(C.cast(owner, C.c_void_p), len(encoded)), C.byref(output))
        try:
            self.check(status)
            return json.loads(C.string_at(output.data, output.size))
        finally:
            self.core.fqdb_buffer_free(C.byref(output))

    def flush(self):
        self.check(self.lib.fqdb_diagnostics_flush(self.handle, 10000))

    def close(self):
        if self.handle:
            self.check(self.lib.fqdb_diagnostics_close(self.handle))
            self.handle = C.c_void_p()


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--diagnostics", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-events", type=int, default=350000)
    parser.add_argument("--segmented", action="store_true")
    args = parser.parse_args()
    core = args.library.resolve(strict=True)
    backend = args.diagnostics.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    logs = output / "log"
    logs.mkdir()
    foreign = logs / "foreign--1-2-3-4.jsonl"
    foreign.write_bytes(b"foreign log must survive\n")
    aged = logs / "fengqudb-1-2-3-4.jsonl"
    aged.write_bytes(b'{"event":"seeded expired legacy log"}\n')
    old_time = time.time() - 8 * 24 * 3600
    os.utime(aged, (old_time, old_time))
    report = {"ok": False, "source_manifest": manifest(), "core_sha256": digest(core),
              "diagnostics_sha256": digest(backend), "progress": [],
              "segmented": args.segmented,
              "note": "Real generated events and default byte/rotation limits; expired file uses an eight-day-old timestamp. Not an eight-day runtime test."}
    log = None
    started = time.monotonic()
    try:
        log = Diagnostics(core, backend, logs)
        initial = log.read("status")
        assert initial["max_bytes"] == 64 * 1024 * 1024 and initial["retention_days"] == 7
        report["initial_status"] = initial
        with Store.open_at(output / "db", "main", library=core, segmented=args.segmented) as db:
            db.put_if_changed(b"PRIVATE-LOG-STRESS-KEY", b"PRIVATE-LOG-STRESS-VALUE", force_reliable=True)
            primaries = list((output / "db/main").glob("g*.fdb"))
            assert len(primaries) == 1
            with primaries[0].open("rb") as primary:
                primary.seek(64)
                actual_segmented = primary.read(8) == b"FQSEG002"
            assert actual_segmented == args.segmented
            report["actual_segmented_header"] = actual_segmented
            log.flush()
            assert not aged.exists(), "Default seven-day retention did not remove expired owned log"
            prefix = "fengqudb--" + initial["writer_id"] + "-"
            first = logs / (prefix + "1.jsonl")
            assert first.is_file()
            report["first_log"] = first.name
            file_bytes = db.info().file_bytes
            attempts = 0
            while first.exists() and attempts < args.max_events:
                batch = min(1024, args.max_events - attempts)
                for _ in range(batch):
                    db.put_if_changed(b"PRIVATE-LOG-STRESS-KEY", b"PRIVATE-LOG-STRESS-VALUE")
                attempts += batch
                log.flush()
                status = log.read("status")
                assert status["dropped"] == status["cleared"] == status["io_errors"] == 0, status
                assert status["queued"] == status["inflight"] == 0, status
                if attempts % 32768 == 0 or not first.exists():
                    row = {"attempts": attempts, "written": status["written"],
                           "owned_files": len(list(logs.glob(prefix + "*.jsonl"))),
                           "first_log_exists": first.exists()}
                    report["progress"].append(row)
                    print(json.dumps(row), flush=True)
            assert not first.exists(), "Finite generated load did not cross the default retention limit"
            assert db.get(b"PRIVATE-LOG-STRESS-KEY") == b"PRIVATE-LOG-STRESS-VALUE"
            assert db.info().file_bytes == file_bytes, "Unchanged-value stress appended business WAL"
            db.sync()
        log.flush()
        status = log.read("status")
        assert status["submitted"] == status["written"] + status["dropped"] + status["cleared"]
        assert status["dropped"] == status["cleared"] == status["io_errors"] == status["callback_errors"] == 0
        assert status["high_water_queued"] <= 4096 and status["high_water_bytes"] <= 2 * 1024 * 1024
        assert status["retention_errors"] == status["protected_files"] == 0 and not status["directory_truncated"]
        log.close()
        log = None
        files = sorted(logs.glob(prefix + "*.jsonl"))
        assert len(files) >= 2 and all(p.stat().st_size <= 8 * 1024 * 1024 for p in files)
        total = sum(p.stat().st_size for p in files)
        assert total <= 64 * 1024 * 1024, "Default owned log bytes grew above the configured cap"
        assert foreign.read_bytes() == b"foreign log must survive\n"
        rows = 0
        for path in files:
            assert len(path.name) <= 53
            with path.open("rb") as stream:
                for line in stream:
                    assert b"PRIVATE-LOG-STRESS" not in line
                    json.loads(line)
                    rows += 1
        assert status["written"] > rows, "No actual generated events were reclaimed"
        log = Diagnostics(core, backend, logs, reader=True)
        assert log.read("status")["reader_only"]
        queried = log.read("query", {"limit": 5})
        assert queried["items"], "Read-only diagnosis could not query retained logs"
        log.close()
        log = None
        with Store.open_at(output / "db", "main", library=core, create=False,
                           segmented=args.segmented) as db:
            assert db.get(b"PRIVATE-LOG-STRESS-KEY") == b"PRIVATE-LOG-STRESS-VALUE"
            db.put_if_changed(b"after-log-stress", b"healthy")
        assert manifest() == report["source_manifest"], "Source changed during retention verification"
        report.update(ok=True, final_status=status, surviving_rows=rows, surviving_bytes=total,
                      files=[{"name": p.name, "bytes": p.stat().st_size} for p in files],
                      generated_noop_calls=attempts, elapsed_seconds=time.monotonic() - started)
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if log is not None:
            log.close()
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS default 64 MiB retention, 8 MiB rotation, seven-day expiry, valid JSONL, privacy and DB continuity", flush=True)


if __name__ == "__main__":
    main()
