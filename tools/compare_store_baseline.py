"""Alternate identical Store benchmark executables with separate native libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", required=True, type=Path)
    parser.add_argument("--current", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    executables = {"previous": args.previous.resolve(), "current": args.current.resolve()}
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in executables.items()}
    if len(set(hashes.values())) != 1:
        parser.error("both executables must be byte-identical; stage each next to its native library")
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    raw = []
    for run in range(args.runs):
        order = ("previous", "current") if run % 2 == 0 else ("current", "previous")
        for engine in order:
            result = subprocess.run([str(executables[engine]), str(output / f"{engine}-{run}")],
                                    capture_output=True, text=True, encoding="utf-8")
            (output / f"{engine}-{run}.stdout.jsonl").write_text(result.stdout, encoding="utf-8")
            (output / f"{engine}-{run}.stderr.log").write_text(result.stderr, encoding="utf-8")
            result.check_returncode()
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            if not rows or len({row["workload"] for row in rows}) != len(rows):
                raise ValueError("empty or duplicate workload results")
            raw.extend(dict(row, engine=engine, run=run) for row in rows)
    summary = []
    for workload in sorted({row["workload"] for row in raw}):
        item = {"workload": workload}
        for engine in executables:
            rows = [row for row in raw if row["engine"] == engine and row["workload"] == workload]
            if len(rows) != args.runs or len({row["operations"] for row in rows}) != 1:
                raise ValueError("inconsistent workload coverage")
            item[engine] = {metric: statistics.median(row[metric] for row in rows)
                            for metric in ("ops_per_second", "p99_us", "peak_private_bytes")}
        item["throughput_ratio"] = item["current"]["ops_per_second"] / item["previous"]["ops_per_second"]
        summary.append(item)
    report = {"runs": args.runs, "host": platform.platform(), "executable_sha256": hashes,
              "note": "Same executable, alternating sequential runs, warm caches. Buffered writes exclude the final sync; each run reopens and verifies its data. Peak private memory is cumulative for the process. This is not a cross-database benchmark.",
              "raw": raw, "medians": summary}
    (output / "comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
