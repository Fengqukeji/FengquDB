"""Measure warm Python Store calls; excludes durable-write and restart timing."""
import argparse
import json
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings/python"))
from fengqudb import Store


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []

    def measure(name, calls, values, operation):
        for _ in range(100): operation()
        samples = []
        for _ in range(5):
            start = time.perf_counter_ns()
            for _ in range(calls): operation()
            samples.append((time.perf_counter_ns() - start) / (calls * values))
        median = statistics.median(samples)
        results.append(dict(workload=name, ns_per_value=median,
                            values_per_second=1e9 / median, samples_ns=samples))

    with Store.open_at(args.output, "main", library=args.library.resolve(), buffered=True) as db:
        keys = [f"key/{i}".encode() for i in range(32)]
        value = b"v" * 256
        db.put_many([(key, value) for key in keys])
        db.checkpoint()
        assert db.get_many(keys) == [value] * 32
        with db.prepare_key(keys[0]) as prepared:
            buffer = bytearray(256)
            measure("python_get_256B", 20000, 1, lambda: db.get(keys[0]))
            measure("python_prepared_get_256B", 20000, 1, prepared.get)
            measure("python_prepared_get_into_256B", 20000, 1, lambda: prepared.get_into(buffer))
            assert buffer == value
            measure("python_get_32_individual", 1000, 32, lambda: [db.get(key) for key in keys])
            measure("python_get_32_batch", 1000, 32, lambda: db.get_many(keys))
        db.verify()
    report = dict(note="Windows Python warm cache, 256-byte values; five sample medians; no durability throughput claim.",
                  results=results)
    (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__": main()
