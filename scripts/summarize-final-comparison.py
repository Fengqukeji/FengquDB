"""Export verified Docker comparison trials; never mix tuning and final samples."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    rows, inputs, images, sources = [], [], set(), set()
    for path in args.reports:
        path = path.resolve()
        report = json.loads(path.read_text(encoding="utf-8"))
        if not report.get("ok"):
            raise ValueError("Incomplete comparison: " + str(path))
        if any(not item.get("removed") for item in report["resources"]):
            raise ValueError("Comparison resources were not settled: " + str(path))
        images.add(report["image"]["Id"])
        sources.add(json.dumps(report["image_source_manifest"], sort_keys=True))
        inputs.append({"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                       "configuration": report["configuration"]})
        for item in report["jobs"]:
            trial_path = Path(item["report"])
            trial = json.loads(trial_path.read_text(encoding="utf-8"))
            job, value = item["job"], item["measurement"]
            if not trial.get("ok") or trial["measurement"] != value:
                raise ValueError("Unverified/mismatched trial: " + str(trial_path))
            for key in ("engine", "mode", "workload", "threads"):
                if job[key] != value[key]:
                    raise ValueError("Trial identity mismatch: " + str(trial_path))
            if not value["ok"] or value["seconds"] <= 0:
                raise ValueError("Invalid measurement: " + str(trial_path))
            events = dict(line.split() for line in trial["cgroup_memory_events"].splitlines())
            if int(events.get("oom_kill", 0)):
                raise ValueError("OOM in measured container: " + str(trial_path))
            rows.append({"suite": path.parent.name, "engine": job["engine"], "mode": job["mode"],
                         "workload": job["workload"], "payload_bytes": value["payload_bytes"],
                         "seed_records": value["seed_records"], "threads": job["threads"],
                         "run": job["run"], "records_per_second": value["records_per_second"],
                         "calls_per_second": value["calls_per_second"], "p50_us": value["p50_us"],
                         "p95_us": value["p95_us"], "p99_us": value["p99_us"],
                         "latency_samples": value["latency_samples"], "seconds": value["seconds"],
                         "calls": value["calls"], "units_per_call": value["units_per_call"],
                         "cpu_cores": value["cgroup_cpu_usec"] / (value["seconds"] * 1e6),
                         "throttled_usec": value["cgroup_throttled_usec"],
                         "memory_sample_peak_bytes": value["observed_measurement_memory"]["peak_bytes"],
                         "memory_lifetime_peak_bytes": value["cgroup_memory_peak_cumulative"],
                         "retries": value["transaction_retries"],
                         "server_restart": trial["server_restart"], "report": str(trial_path)})
    if len(images) != 1 or len(sources) != 1:
        raise ValueError("Do not combine different images or source manifests")
    # Repetitions at a given concurrency are reduced first. Only then choose
    # the largest median within each suite, workload and durability mode.
    grouped = {}
    for row in rows:
        identity = tuple(row[key] for key in ("suite", "engine", "mode", "workload", "payload_bytes",
                                              "seed_records", "threads"))
        grouped.setdefault(identity, []).append(row)
    medians = []
    for identity, samples in grouped.items():
        if len({row["run"] for row in samples}) != len(samples):
            raise ValueError("Duplicate run at the same concurrency: " + str(identity))
        median = dict(samples[0])
        for key in ("records_per_second", "calls_per_second", "p50_us", "p95_us", "p99_us",
                    "cpu_cores", "memory_sample_peak_bytes"):
            median[key] = statistics.median(row[key] for row in samples)
        median.update(samples=len(samples), min_rps=min(r["records_per_second"] for r in samples),
                      max_rps=max(r["records_per_second"] for r in samples))
        medians.append(median)
    peaks = {}
    for row in medians:
        identity = tuple(row[key] for key in ("suite", "engine", "mode", "workload", "payload_bytes",
                                              "seed_records"))
        if identity not in peaks or row["records_per_second"] > peaks[identity]["records_per_second"]:
            peaks[identity] = row
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / "trials.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.output / "summary.json").write_text(json.dumps({"inputs": inputs, "image": next(iter(images)),
        "trial_count": len(rows), "medians": medians, "peaks": list(peaks.values())}, indent=2), encoding="utf-8")
    lines = ["# Verified comparison samples", "",
             "Peak below means the largest median at a tested concurrency in that suite. "
             "It is not a theoretical or universal limit. Samples from different suites remain separate.", "",
             "Memory is the sampled cgroup total, including services and file cache; P99 is sampled per call. "
             "Batch throughput counts records; batch latency covers the whole call.", "",
             "| Suite | Engine | Mode | Workload | Bytes | Threads | Samples | Records/s | P99 us | CPU cores | Memory MiB |",
             "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for key, row in sorted(peaks.items()):
        lines.append(f"| {row['suite']} | {row['engine']} | {row['mode']} | {row['workload']} | "
                     f"{row['payload_bytes']} | {row['threads']} | {row['samples']} | "
                     f"{row['records_per_second']:,.0f} | {row['p99_us']:.2f} | {row['cpu_cores']:.2f} | "
                     f"{row['memory_sample_peak_bytes'] / 1048576:.1f} |")
    (args.output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"ok": True, "trials": len(rows), "groups": len(grouped), "peaks": len(peaks)}))


if __name__ == "__main__":
    main()
