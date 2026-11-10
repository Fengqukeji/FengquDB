"""Run resource-matched Store/SQLite/MySQL/Redis/hybrid jobs in Docker.

Each container includes the driver and all services for that engine, sharing two
physical CPU cores and 4 GiB with no swap. Database files use a new named Linux
volume. Only resources bearing this invocation's ownership label are removed.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--image", default="fqdb-final-bench:20261107")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("smoke", "matrix"), default="smoke")
    parser.add_argument("--engines", default="fqdb,sqlite,mysql,redis,hybrid")
    parser.add_argument("--modes", default="sync,buffered")
    parser.add_argument("--workloads", default="get_copy,update,patch8,put_many,get_many,mixed95,insert,delete")
    parser.add_argument("--threads", default="1,2,4,8,16,32")
    parser.add_argument("--cpuset", default="0,2", help="Two distinct physical cores on this host")
    parser.add_argument("--records", type=int, default=131072)
    parser.add_argument("--payload", type=int, default=256)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--fixed-calls", type=int, default=4096)
    parser.add_argument("--group", action="store_true", help="Reuse MySQL service startup across verified trials")
    parser.add_argument("--redis-io-threads", type=int, choices=(1,2), default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    token = uuid.uuid4().hex
    engine_names = args.engines.split(","); modes = args.modes.split(",")
    if not set(engine_names) <= {"fqdb", "sqlite", "mysql", "redis", "hybrid"} or not set(modes) <= {"sync", "buffered"}:
        parser.error("unknown engine/mode")
    report = {"ok": False, "owner": token, "jobs": [], "resources": [], "configuration": vars(args).copy()}
    report["configuration"]["output"] = str(output)

    def run(command, timeout=900):
        result = subprocess.run(list(map(str, command)), stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=timeout)
        if result.returncode:
            raise RuntimeError("Command failed: " + " ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
        return result.stdout

    def docker(*command):
        return run(["docker", *command])

    def inspect(kind, name):
        return json.loads(docker(kind, "inspect", name))[0] if kind == "volume" else json.loads(docker("inspect", name))[0]

    def check_owner(resource, kind):
        data = inspect(kind, resource)
        labels = data.get("Labels") if kind == "volume" else data["Config"].get("Labels")
        if not labels or labels.get("fqdb.benchmark.owner") != token:
            raise RuntimeError("Resource ownership mismatch: " + resource)
        return data

    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    def image_sources():
        files=[root/"CMakeLists.txt"]
        for directory in ("cmake","src","include","tools/final_benchmark"):
            files.extend(p for p in (root/directory).rglob("*") if p.is_file() and "__pycache__" not in p.parts and p.suffix!=".pyc")
        return {p.relative_to(root).as_posix():digest(p) for p in sorted(files)}

    try:
        image = json.loads(docker("image", "inspect", args.image))[0]
        image_id = image["Id"]; report["image"] = image
        manifest = json.loads(docker("run", "--rm", "--entrypoint", "cat", image_id, "/opt/bench/source-manifest.json"))
        if image_sources()!=manifest:
            raise RuntimeError("Image source set or hashes differ from current workspace")
        report["image_source_manifest"] = manifest
        report["driver_sha256"] = digest(Path(__file__).resolve())
        metadata = docker("run", "--rm", "--entrypoint", "sh", image_id, "-c",
                          "uname -a; g++ --version; mysqld --version; redis-server --version; ldd /opt/bench/bin/fqdb_final_bench; sha256sum /opt/bench/lib/*.so*")
        (output / "image-runtime.txt").write_text(metadata, encoding="utf-8")
        if args.suite == "smoke":
            jobs = [dict(engine=engine, mode=mode, workload="get_copy", threads=2, records=256,
                         payload=256, seconds=.25, fixed_calls=64, selftest=True, run=0)
                    for engine, mode in itertools.product(engine_names, modes)]
        else:
            jobs = [dict(engine=engine, mode=mode, workload=workload, threads=int(threads), records=args.records,
                         payload=args.payload, seconds=args.seconds, fixed_calls=args.fixed_calls, run=run_id)
                    for run_id in range(args.runs)
                    for mode, workload, threads, engine in itertools.product(modes, args.workloads.split(","), args.threads.split(","),
                                                                           engine_names if run_id % 2 == 0 else list(reversed(engine_names)))]
        report["planned_jobs"] = jobs
        for item in jobs:
            item["redis_io_threads"]=args.redis_io_threads
        if args.group:
            jobs=[{"engine":engine,"trials":[item for item in jobs if item["engine"]==engine],
                   "redis_io_threads":args.redis_io_threads,"restart_each":args.suite=="smoke"} for engine in engine_names]
        report["container_jobs"]=jobs
        save()
        for index, job in enumerate(jobs):
            name = f"fqdb-bench-{token[:12]}-j{index:04}"
            directory = output / f"j{index:04}"; directory.mkdir()
            (directory / "job.json").write_text(json.dumps(job, indent=2), encoding="utf-8")
            volume = name + "-data"
            docker("volume", "create", "--label", "fqdb.benchmark.owner=" + token, volume)
            resource = {"name": name, "volume": volume, "job": index, "removed": False}
            report["resources"].append(resource)
            print(json.dumps({"stage": "starting", "index": index, "total": len(jobs), "engine":job["engine"],"trials":len(job.get("trials",[job]))}), flush=True)
            container = docker("create", "--name", name, "--label", "fqdb.benchmark.owner=" + token,
                               "--init", "--network", "none", "--cpus", "2", "--cpuset-cpus", args.cpuset,
                               "--memory", "4g", "--memory-swap", "4g", "--pids-limit", "512",
                               "--mount", "type=volume,source=" + volume + ",target=/data",
                               "--mount", "type=bind,source=" + directory.as_posix() + ",target=/out",
                               image_id, "--job", "/out/job.json").strip()
            resource["container_id"] = container
            inspected = check_owner(container, "container")
            config = inspected["HostConfig"]
            if config["NanoCpus"] != 2000000000 or config["Memory"] != 4 << 30 or config["MemorySwap"] != 4 << 30 or config["CpusetCpus"] != args.cpuset:
                raise RuntimeError("Docker resource limits differ from requested values")
            (directory / "container-inspect.json").write_text(json.dumps(inspected, indent=2), encoding="utf-8")
            started = time.monotonic()
            process=subprocess.Popen(["docker","start","-a",container],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                                     text=True,encoding="utf-8",errors="replace")
            watchdog=threading.Timer(max(900,len(job.get("trials",[job]))*300),process.kill);watchdog.start()
            try:
                with (directory/"container.log").open("w",encoding="utf-8") as log:
                    for line in process.stdout:
                        log.write(line);log.flush()
                        if line.startswith('{'):
                            progress=json.loads(line)
                            if progress.get("stage")=="trial_passed":
                                print(json.dumps(dict(progress,engine=job["engine"],container=index)),flush=True)
                returncode=process.wait(timeout=10)
            finally:
                watchdog.cancel()
            state = check_owner(container, "container")["State"]
            resource["state"] = state
            native = json.loads((directory / "report.json").read_text()) if (directory / "report.json").is_file() else {}
            events = dict(line.split() for line in native.get("cgroup_memory_events", "").splitlines())
            if returncode or state["Running"] or state["ExitCode"] or state["OOMKilled"] or int(events.get("oom_kill", 0)) or not native.get("ok"):
                raise RuntimeError(f"Job {index} failed; inspect {directory}")
            elapsed=time.monotonic()-started
            for trial_index,trial in enumerate(native.get("trials",[native])):
                trial_path=directory/f"t{trial_index:04}"/"report.json" if "trials" in job else directory/"report.json"
                report["jobs"].append({"index":index,"trial":trial_index,"job":trial["job"],"report":str(trial_path),"measurement":trial["measurement"],"container_elapsed_seconds":elapsed})
            check_owner(container, "container"); docker("rm", container); resource["container_removed"] = True
            check_owner(volume, "volume"); docker("volume", "rm", volume)
            resource["removed"] = True
            if image_sources()!=manifest:
                raise RuntimeError("Image sources changed during comparison")
            save()
            print(json.dumps({"stage":"passed","index":index,"elapsed_seconds":elapsed,"trials":len(native.get("trials",[native]))}),flush=True)
        if digest(Path(__file__).resolve()) != report["driver_sha256"]:
            raise RuntimeError("Driver changed during comparison")
        report["ok"] = True
    except BaseException as error:
        report["error"] = str(error)
        for resource in report["resources"]:
            if resource.get("container_id") and not resource["removed"] and not resource.get("container_removed"):
                data = check_owner(resource["container_id"], "container")
                if data["State"]["Running"]:
                    docker("stop", "--time", "20", resource["container_id"])
                resource["retained_for_diagnosis"] = True
        raise
    finally:
        save()
    print("PASS resource-matched benchmark jobs and restart validation", flush=True)


if __name__ == "__main__":
    main()
