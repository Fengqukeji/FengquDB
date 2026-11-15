"""Bounded production-ABI soak: paired-record atomicity during writes and backup."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings/python"))
from fengqudb import Store


def key(owner, slot, part):
    return f"pair/{owner}/{slot}/{part}".encode()


def validate(view, owner, slot):
    a = view.get(key(owner, slot, "a"))
    b = view.get(key(owner, slot, "b"))
    assert (a is None) == (b is None), "partial transaction observed"
    if a is not None:
        assert hashlib.sha256(a).digest() == b, "mismatched transaction pair"
        assert struct.unpack_from("<II", a) == (owner, slot)


def run(root, library, mode, segmented, operations):
    path = root / f"{mode}-{'segmented' if segmented else 'hybrid'}"
    owners, slots = 4, 16
    db = Store(path, library=library, segmented=segmented, cache_bytes=256 * 1024)
    db.configure_write(mode=mode)
    initial = db.snapshot()
    done = threading.Event()
    barrier = threading.Barrier(owners + 3)
    expected = [{} for _ in range(owners)]
    reads = [0, 0]
    started = time.monotonic()

    def writer(owner):
        barrier.wait(timeout=20)
        for sequence in range(operations):
            slot = sequence % slots
            with db.transaction() as tx:
                if sequence % 11 == 0:
                    tx.delete(key(owner, slot, "a"))
                    tx.delete(key(owner, slot, "b"))
                    expected[owner].pop(slot, None)
                else:
                    data = struct.pack("<III", owner, slot, sequence)
                    data += bytes([sequence % 251]) * (4096 if sequence % 7 == 0 else 128)
                    tx.put(key(owner, slot, "a"), data)
                    tx.put(key(owner, slot, "b"), hashlib.sha256(data).digest())
                    expected[owner][slot] = data
                tx.commit(force_reliable=sequence % 127 == 0)

    def reader(number):
        barrier.wait(timeout=20)
        while not done.is_set():
            with db.snapshot() as view:
                for owner in range(owners):
                    validate(view, owner, reads[number] % slots)
                reads[number] += 1

    try:
        with ThreadPoolExecutor(max_workers=owners + 2) as executor:
            writers = [executor.submit(writer, owner) for owner in range(owners)]
            readers = [executor.submit(reader, number) for number in range(2)]
            barrier.wait(timeout=20)
            try:
                # Real backup runs while the same Store accepts concurrent writes.
                db.backup(root / (path.name + "-backup"))
                for future in writers:
                    future.result(timeout=60)
            finally:
                done.set()
            for future in readers:
                future.result(timeout=20)
        assert all(reads), "concurrent readers did not execute"
        for owner in range(owners):
            for slot in range(slots):
                assert initial.get(key(owner, slot, "a")) is None, "old snapshot changed"
        initial.close()
        db.sync()
        info = db.info()
        assert info.version == info.durable_version, "sync left an unacknowledged suffix"
        db.verify()
    finally:
        initial.close()
        db.close()
    with Store(path, library=library, create=False) as recovered:
        for owner in range(owners):
            for slot in range(slots):
                assert recovered.get(key(owner, slot, "a")) == expected[owner].get(slot)
        recovered.verify()
    with Store(root / (path.name + "-backup"), library=library, create=False) as backup:
        with backup.snapshot() as view:
            for owner in range(owners):
                for slot in range(slots):
                    validate(view, owner, slot)
        backup.verify()
    return dict(mode=mode, segmented=segmented, write_transactions=owners * operations,
                reader_snapshots=sum(reads), seconds=time.monotonic() - started)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--operations", type=int, default=400)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 16 <= args.operations <= 4000:
        parser.error("operations must be 16..4000 per writer")
    with tempfile.TemporaryDirectory(prefix="fqdb-soak-") as temporary:
        results = [run(Path(temporary), str(args.library.resolve()), mode, segmented, args.operations)
                   for mode in ("performance", "reliable") for segmented in (False, True)]
    args.output.write_text(json.dumps(dict(ok=True, cases=results), indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results))
