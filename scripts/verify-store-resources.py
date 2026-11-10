"""Real Linux ENOSPC, allocation failure, OOM kill and external SIGKILL checks.

Uses a separately built production-API probe in dedicated Docker containers.
Only this run's labelled containers/volumes are removed after passing checks.
Failed containers/volumes and all host-side logs remain available for inspection.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
import uuid


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest(root):
    paths = [root / "CMakeLists.txt", root / "tools/store_resource_probe.cpp", Path(__file__).resolve()]
    for directory in (root / "src", root / "include"):
        paths.extend(p for p in directory.rglob("*") if p.suffix in (".h", ".hpp", ".cpp"))
    return {str(p.relative_to(root)).replace("\\", "/"): sha256(p) for p in sorted(set(paths))}


class Run:
    def __init__(self, args, root, output):
        self.args, self.root, self.output = args, root, output
        self.identity = "fqdb-res-" + str(time.time_ns()) + "-" + uuid.uuid4().hex[:6]
        self.label = "fengqudb.resource-run"
        self.bundle = output / "bundle"
        self.bundle.mkdir()
        shutil.copy2(args.probe, self.bundle / "probe")
        shutil.copy2(args.library, self.bundle / "libfengqudb.so.1")
        self.report = {
            "ok": False, "identity": self.identity, "cases": [], "commands": [],
            "resources": [], "source_manifest": source_manifest(root),
            "probe_sha256": sha256(args.probe), "library_sha256": sha256(args.library),
            "limitations": "Actual bounded tmpfs ENOSPC and Linux process/container failures. "
                            "Not a hardware power-loss test or a throughput benchmark.",
        }
        self.image = self.command(["image", "inspect", args.image])[0]
        self.report["image"] = {k: self.image.get(k) for k in ("Id", "RepoDigests", "Architecture", "Os")}
        if self.image["Os"] != "linux" or self.image["Architecture"] != "amd64":
            raise RuntimeError("This runner requires a Linux amd64 probe and image")

    def save(self):
        (self.output / "report.json").write_text(json.dumps(self.report, indent=2), encoding="utf-8")

    def command(self, arguments, *, parse=True):
        command = ["docker", *arguments]
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=60)
        self.report["commands"].append({"command": command, "exit_code": result.returncode,
                                        "stdout": result.stdout, "stderr": result.stderr})
        result.check_returncode()
        return json.loads(result.stdout) if parse else result.stdout.strip()

    def inspect(self, name):
        info = self.command(["inspect", name])[0]
        if info["Config"]["Labels"].get(self.label) != self.identity:
            raise RuntimeError("Refusing to operate on a container not owned by this run")
        return info

    def remove_container(self, name):
        info = self.inspect(name)
        if info["State"]["Running"]:
            raise RuntimeError("Refusing to remove a running container")
        self.command(["rm", name], parse=False)

    def volume(self, suffix):
        name = self.identity + "-" + suffix
        self.command(["volume", "create", "--label", self.label + "=" + self.identity, name], parse=False)
        self.report["resources"].append({"kind": "volume", "name": name, "removed": False})
        self.save()
        return name

    def remove_volume(self, name):
        info = self.command(["volume", "inspect", name])[0]
        if info.get("Labels", {}).get(self.label) != self.identity:
            raise RuntimeError("Refusing to remove a volume not owned by this run")
        self.command(["volume", "rm", name], parse=False)
        next(r for r in self.report["resources"] if r["name"] == name)["removed"] = True

    def execute(self, label, mode, *, volume=None, memory="512m", extra=(), kill_after=None):
        name = self.identity + "-" + label
        mount = ("type=volume,source=" + volume + ",target=/data" if volume else
                 "type=tmpfs,destination=/data,tmpfs-size=16777216,tmpfs-mode=0700")
        arguments = ["create", "--name", name, "--label", self.label + "=" + self.identity,
                     "--network", "none", "--read-only", "--cap-drop", "ALL", "--security-opt",
                     "no-new-privileges", "--cpus", "2", "--memory", memory, "--memory-swap", memory,
                     "--pids-limit", "64", "--mount", "type=bind,source=" + str(self.bundle) +
                     ",target=/probe,readonly", "--mount", mount,
                     "--env", "LD_LIBRARY_PATH=/probe", self.image["Id"],
                     "/probe/probe", mode, "/data", *map(str, extra)]
        self.command(arguments, parse=False)
        resource = {"kind": "container", "name": name, "removed": False}
        self.report["resources"].append(resource)
        self.save()
        print(json.dumps({"event": "start", "case": label, "memory": memory}), flush=True)
        lines = queue.Queue()
        events, killed = [], False
        started = time.monotonic()
        with (self.output / (label + ".stdout.jsonl")).open("w", encoding="utf-8") as log, \
             (self.output / (label + ".stderr.log")).open("w", encoding="utf-8") as errors:
            process = subprocess.Popen(["docker", "start", "-a", name], stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=errors, text=True,
                                       encoding="utf-8", errors="replace")

            def read():
                try:
                    for line in process.stdout:
                        lines.put(line)
                finally:
                    lines.put(None)

            reader = threading.Thread(target=read, daemon=True)
            reader.start()
            try:
                while True:
                    if time.monotonic() - started > self.args.timeout:
                        raise TimeoutError("Resource probe exceeded the watchdog deadline")
                    try:
                        line = lines.get(timeout=1)
                    except queue.Empty:
                        continue
                    if line is None:
                        break
                    log.write(line)
                    log.flush()
                    event = json.loads(line)
                    events.append(event)
                    if (kill_after is not None and not killed and event["event"] == "begin"
                            and event["sequence"] >= kill_after):
                        info = self.inspect(name)
                        if not info["State"]["Running"]:
                            raise RuntimeError("Writer terminated before the planned external kill")
                        self.command(["kill", "--signal", "KILL", name], parse=False)
                        killed = True
                process.wait(timeout=15)
                reader.join(timeout=5)
            except BaseException:
                # A timeout is a test failure, never permission to restart it.
                if self.inspect(name)["State"]["Running"]:
                    self.command(["kill", "--signal", "KILL", name], parse=False)
                process.wait(timeout=15)
                raise
            finally:
                reader.join(timeout=5)
                process.stdout.close()
        info = self.inspect(name)
        (self.output / (label + ".inspect.json")).write_text(json.dumps(info, indent=2), encoding="utf-8")
        config = info["HostConfig"]
        result = {"name": label, "mode": mode, "state": info["State"], "events": events,
                  "external_kill_requested": killed, "docker_client_exit": process.returncode,
                  "limits": {k: config[k] for k in ("Memory", "MemorySwap", "NanoCpus", "PidsLimit")},
                  "elapsed_seconds": time.monotonic() - started, "container": name}
        self.report["cases"].append(result)
        self.save()
        if info["State"]["Running"] or info["State"]["Error"]:
            raise RuntimeError("Probe did not reach a clean terminal container state")
        if config["NanoCpus"] != 2000000000 or config["Memory"] != config["MemorySwap"]:
            raise RuntimeError("Requested CPU/memory constraints were not applied")
        if mode in ("kill", "oom"):
            acknowledgements = [e for e in events if e["event"] == "ack"]
            for n, item in enumerate(acknowledgements, 1):
                if item["sequence"] != n or item["version"] != n:
                    raise RuntimeError("Host acknowledgement journal is not a contiguous prefix")
            if not acknowledgements or info["State"]["ExitCode"] != 137:
                raise RuntimeError("Expected a killed writer after acknowledged commits")
            if mode == "oom" and not info["State"]["OOMKilled"]:
                raise RuntimeError("Writer was not killed by the actual cgroup memory limit")
            if mode == "kill" and (not killed or info["State"]["OOMKilled"]):
                raise RuntimeError("External SIGKILL case did not have the requested cause")
            result["acknowledged"] = acknowledgements[-1]["sequence"]
        elif (info["State"]["ExitCode"] != 0 or process.returncode != 0 or
              not any(e["event"] == "pass" for e in events)):
            raise RuntimeError("Native resource case failed: " + label)
        self.remove_container(name)
        resource["removed"] = True
        self.save()
        print(json.dumps({"event": "verified", "case": label, "exit_code": info["State"]["ExitCode"],
                          "oom_killed": info["State"]["OOMKilled"]}), flush=True)
        return result

    def run(self):
        selected = self.args.case or ["disk-full", "disk-compact", "allocation", "oom", "kill"]
        for case in selected:
            if case in ("disk-full", "disk-compact", "allocation"):
                self.execute(case, case)
            else:
                rounds = self.args.kill_rounds if case == "kill" else 1
                for n in range(rounds):
                    suffix = case + "-" + str(n)
                    volume = self.volume(suffix)
                    blob = 65536 if case == "oom" else 4 * 1024 * 1024
                    writer = self.execute(suffix, case, volume=volume,
                                          memory="96m" if case == "oom" else "256m",
                                          extra=(blob,), kill_after=(4 << n) if case == "kill" else None)
                    recovery = self.execute(suffix + "-recover", "recover", volume=volume,
                                            memory="256m", extra=(writer["acknowledged"], blob))
                    recovered = next(e for e in recovery["events"] if e["event"] == "pass")
                    if recovered["acknowledged"] != writer["acknowledged"]:
                        raise RuntimeError("Recovery did not validate the host's actual acknowledgement prefix")
                    self.remove_volume(volume)
        if source_manifest(self.root) != self.report["source_manifest"]:
            raise RuntimeError("Source changed during the resource verification run")
        self.report["ok"] = True


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="ubuntu:26.04")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--kill-rounds", type=int, default=3)
    parser.add_argument("--case", action="append", choices=["disk-full", "disk-compact", "allocation", "oom", "kill"])
    args = parser.parse_args()
    if args.timeout < 10 or not 1 <= args.kill_rounds <= 5:
        parser.error("Use timeout >=10 and 1..5 kill rounds")
    args.probe = args.probe.resolve(strict=True)
    args.library = args.library.resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if "," in str(output):
        parser.error("The Docker bind-mount output path cannot contain commas")
    output.mkdir(parents=True, exist_ok=False)
    run = Run(args, root, output)
    try:
        run.run()
    except BaseException as error:
        run.report["error"] = str(error)
        raise
    finally:
        run.save()
    print("PASS real filesystem ENOSPC, address-space exhaustion, cgroup OOM and external-kill recovery", flush=True)


if __name__ == "__main__":
    main()
