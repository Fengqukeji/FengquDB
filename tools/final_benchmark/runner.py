"""Run one isolated five-engine benchmark job inside its resource cgroup."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import shutil
import socket
import subprocess
import threading
import time

BIN = Path("/opt/bench/bin/fqdb_final_bench")


def read(path):
    return Path(path).read_text().strip()


def limits():
    values = {name: read("/sys/fs/cgroup/" + name) for name in
              ("cpu.max", "cpuset.cpus.effective", "memory.max", "memory.swap.max", "pids.max")}
    quota, period = values["cpu.max"].split()
    if quota == "max" or int(quota) != 2 * int(period) or values["memory.max"] != str(4 << 30) or values["memory.swap.max"] != "0":
        raise RuntimeError("Expected total 2 CPU / 4 GiB with no extra swap: " + str(values))
    return values


class Services:
    def __init__(self, engine, output, redis_io_threads=2):
        self.engine, self.output = engine, output
        if redis_io_threads not in (1, 2):
            raise ValueError("Redis I/O thread tuning must be 1 or 2")
        self.redis_io_threads = redis_io_threads
        self.children, self.logs = [], []

    def command(self, label, args, timeout=120):
        result = subprocess.run(list(map(str, args)), stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=timeout)
        (self.output / (label + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(label + ": " + result.stdout + result.stderr)
        return result.stdout

    def mysql(self, sql):
        return self.command("mysql-control", ["mysql", "--no-defaults", "--protocol=SOCKET",
                            "--socket=/data/m.sock", "-uroot", "--batch", "--raw", "--skip-column-names", "-e", sql])

    def spawn(self, name, args):
        log = (self.output / (name + "-process.log")).open("a")
        self.logs.append(log)
        process = subprocess.Popen(list(map(str, args)), stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.children.append((name, process))
        return process

    def wait(self, process, port):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Service exited during startup: " + str(process.returncode))
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return
            except OSError:
                time.sleep(.1)
        raise TimeoutError("Service not ready on port " + str(port))

    def wait_redis_ready(self, process):
        # Redis accepts TCP connections before an existing AOF finishes loading.
        # Readiness is outside all timed native calls, including crash recovery.
        started = time.monotonic()
        deadline = started + 90
        attempts, loading = 0, 0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Redis exited during dataset loading: " + str(process.returncode))
            try:
                result = subprocess.run(["/opt/bench/bin/redis-cli", "--raw", "PING"],
                                        stdin=subprocess.DEVNULL, capture_output=True,
                                        text=True, timeout=2)
                reply = (result.stdout + result.stderr).strip()
                attempts += 1
                loading += int("LOADING" in reply)
                (self.output / "redis-ready.json").write_text(json.dumps({
                    "attempts": attempts, "loading_replies": loading,
                    "elapsed_seconds": time.monotonic() - started, "reply": reply}))
                if result.returncode == 0 and reply == "PONG":
                    return
                if reply and result.returncode == 0 and "LOADING" not in reply:
                    raise RuntimeError("Redis startup PING failed: " + reply)
            except subprocess.TimeoutExpired:
                pass
            time.sleep(.05)
        raise TimeoutError("Redis did not finish dataset loading within 90 seconds")

    def start(self, initial, names=None):
        if self.engine in ("mysql", "hybrid") and (names is None or "mysql" in names):
            directory = Path("/data/mysql")
            if initial:
                directory.mkdir(exist_ok=False)
                self.command("mysql-initialize", ["mysqld", "--no-defaults", "--initialize-insecure", "--user=root",
                                                 "--datadir=" + str(directory), "--log-error=" + str(self.output / "mysql-init.log")], timeout=180)
            for path in ("/data/m.sock", "/data/m.sock.lock", "/data/m.pid"):
                Path(path).unlink(missing_ok=True)
            buffer = 1 << 30 if self.engine == "hybrid" else 2 << 30
            args = ["mysqld", "--no-defaults", "--user=root", "--datadir=/data/mysql", "--socket=/data/m.sock",
                    "--pid-file=/data/m.pid", "--bind-address=127.0.0.1", "--port=3306", "--mysqlx=OFF", "--skip-log-bin",
                    "--performance-schema=OFF", "--max-connections=256", "--max-allowed-packet=67108864",
                    "--innodb-buffer-pool-size=" + str(buffer), "--innodb-log-buffer-size=67108864",
                    "--innodb-redo-log-capacity=268435456", "--innodb-flush-method=O_DIRECT",
                    "--innodb-doublewrite=ON", "--innodb-flush-log-at-trx-commit=1",
                    "--log-error=" + str(self.output / "mysql.log")]
            self.wait(self.spawn("mysql", args), 3306)
            if initial:
                self.mysql("CREATE DATABASE bench; CREATE USER 'bench'@'127.0.0.1' IDENTIFIED BY 'bench'; GRANT ALL PRIVILEGES ON *.* TO 'bench'@'127.0.0.1';")
        if self.engine in ("redis", "hybrid") and (names is None or "redis" in names):
            directory = Path("/data/redis")
            if initial:
                directory.mkdir(exist_ok=False)
            config = ["bind 127.0.0.1", "port 6379", "protected-mode yes", "daemonize no", "save \"\"",
                      "dir /data/redis", "appendonly " + ("yes" if self.engine == "redis" else "no"),
                      "appendfsync always", "no-appendfsync-on-rewrite no", "auto-aof-rewrite-percentage 100",
                      "auto-aof-rewrite-min-size 64mb", "maxmemory " + ("1gb" if self.engine == "hybrid" else "3gb"),
                      "maxmemory-policy noeviction", "io-threads " + str(self.redis_io_threads), "logfile " + str(self.output / "redis.log")]
            conf = Path("/data/r.conf"); conf.write_text("\n".join(config) + "\n")
            process = self.spawn("redis", ["/opt/bench/bin/redis-server", conf])
            self.wait(process, 6379)
            self.wait_redis_ready(process)

    def stop(self, crash=False, names=None):
        children = [(name, process) for name, process in self.children if names is None or name in names]
        self.children = [(name, process) for name, process in self.children if names is not None and name not in names]
        for name, process in children:
            if process.poll() is None:
                if process.pid <= 1 or os.getpgid(process.pid) != process.pid:
                    raise RuntimeError("Unexpected service process group")
                os.killpg(process.pid, signal.SIGKILL if crash else signal.SIGTERM)
        for name, process in children:
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=10)
        for log in self.logs:
            log.close()
        self.logs.clear()

    def settings(self):
        result = {}
        if self.engine in ("mysql", "hybrid"):
            result["mysql"] = self.mysql("SELECT VERSION(),@@GLOBAL.innodb_flush_log_at_trx_commit,@@GLOBAL.log_bin,@@GLOBAL.innodb_buffer_pool_size,@@GLOBAL.innodb_doublewrite,@@GLOBAL.innodb_flush_method;")
        if self.engine in ("redis", "hybrid"):
            raw = self.command("redis-config", ["/opt/bench/bin/redis-cli", "--json", "CONFIG", "GET", "appendonly", "appendfsync", "maxmemory", "maxmemory-policy", "io-threads"])
            result["redis"] = json.loads(raw)
            result["redis_info"] = self.command("redis-info", ["/opt/bench/bin/redis-cli", "INFO", "persistence"])
        return result


def native(services, label, args, timed=False):
    if not timed:
        text = services.command(label, args, timeout=240)
        result = json.loads(text)
        if not result.get("ok"):
            raise RuntimeError(label + " returned false")
        return result
    active, finished = threading.Event(), threading.Event()
    observed = {"peak_bytes": 0, "samples": 0}
    def sample():
        try:
            while not finished.wait(.02):
                if active.is_set():
                    observed["peak_bytes"] = max(observed["peak_bytes"], int(read("/sys/fs/cgroup/memory.current")))
                    observed["samples"] += 1
        except BaseException as error:
            observed["error"] = str(error)
    sampler = threading.Thread(target=sample); sampler.start()
    try:
        with (services.output / (label + ".stdout.json")).open("w") as stdout:
            process = subprocess.Popen(list(map(str, args)), stdin=subprocess.DEVNULL, stdout=stdout,
                                       stderr=subprocess.PIPE, text=True)
            watchdog = threading.Timer(240, process.kill)
            watchdog.start()
            lines = []
            try:
                for line in process.stderr:
                    lines.append(line)
                    if line.strip() == "BENCH_TIMED_BEGIN":
                        active.set()
                        observed["samples"] += 1
                        observed["peak_bytes"] = max(observed["peak_bytes"], int(read("/sys/fs/cgroup/memory.current")))
                    elif line.strip() == "BENCH_TIMED_END":
                        active.clear()
                status = process.wait(timeout=10)
            finally:
                watchdog.cancel()
        (services.output / (label + ".stderr.log")).write_text("".join(lines))
        if status:
            raise RuntimeError(label + ": " + "".join(lines))
        result = json.loads((services.output / (label + ".stdout.json")).read_text())
        if not result.get("ok"):
            raise RuntimeError("Native result not verified")
        if observed.get("error") or not observed["samples"]:
            raise RuntimeError("Missing measurement memory observations: " + str(observed))
        result["observed_measurement_memory"] = observed
        return result
    finally:
        finished.set(); sampler.join(timeout=2)


def remove_fixture(path):
    path = Path(path)
    if path not in (Path("/data/client"), Path("/data/redis")) or path.resolve() != path:
        raise RuntimeError("Refuse fixture cleanup outside owned data directories")
    if path.exists():
        shutil.rmtree(path)


def verify_settings(settings, job):
    if job["engine"] in ("mysql", "hybrid"):
        values = settings["mysql"].strip().split("\t")
        if not values[0].startswith("8.4.11") or int(values[1]) != (1 if job["mode"] == "sync" else 2) or values[2] != "0" or values[4:] != ["ON", "O_DIRECT"]:
            raise RuntimeError("MySQL durability settings differ: " + repr(values))
    if job["engine"] in ("redis", "hybrid"):
        values = settings["redis"]
        if values["appendonly"] != ("yes" if job["engine"] == "redis" else "no") or values["maxmemory-policy"] != "noeviction":
            raise RuntimeError("Redis persistence/memory settings differ")
        if job["engine"] == "redis" and values["appendfsync"] != ("always" if job["mode"] == "sync" else "no"):
            raise RuntimeError("Redis durable acknowledgement settings differ")


def run_one(services, job, output, restart):
    report = {"ok": False, "job": job, "server_restart": restart}
    base = [BIN, "--engine", job["engine"], "--mode", job["mode"], "--workload", job["workload"],
            "--threads", job["threads"], "--records", job["records"], "--payload", job["payload"],
            "--seconds", job["seconds"], "--fixed-calls", job["fixed_calls"], "--data", "/data/client"]
    try:
        report["prepare"] = native(services, "prepare", base + ["--action", "prepare"])
        if job.get("selftest"):
            report["selftest"] = native(services, "selftest", base + ["--action", "selftest"])
        report["measurement"] = native(services, "run", base + ["--action", "run"], timed=True)
        report["settings"] = services.settings()
        verify_settings(report["settings"], job)
        if restart:
            services.stop(crash=True)
            services.start(initial=False)
        label = "restart-verify" if restart else "independent-verify"
        report[label] = native(services, label, base + ["--action", "verify"])
        data = Path("/data/client/expected.bin").read_bytes()
        report["oracle_sha256"] = hashlib.sha256(data).hexdigest()
        (output / "expected.bin.gz").write_bytes(gzip.compress(data, mtime=0))
        shutil.copyfile("/data/client/payload.pb", output / "payload.pb")
        report["ok"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        report["cgroup_memory_events"] = read("/sys/fs/cgroup/memory.events")
        (output / "report.json").write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--job", type=Path, required=True)
    args = parser.parse_args()
    job = json.loads(args.job.read_text())
    output = Path("/out")
    services = Services(job["engine"], output, job.get("redis_io_threads", 2))
    report = {"ok": False, "job": job, "limits": limits(), "platform": platform.platform(), "machine": platform.machine(), "trials": []}
    for filename in ("cpuinfo", "version", "mounts"):
        (output / (filename + ".txt")).write_text(read("/proc/" + filename))
    try:
        services.start(initial=True)
        trials = job.get("trials", [job])
        for index, trial in enumerate(trials):
            if trial["engine"] != job["engine"]:
                raise ValueError("A grouped container must use one engine")
            case = output / f"t{index:04}" if "trials" in job else output
            case.mkdir(exist_ok=True)
            services.output = case
            if index:
                remove_fixture("/data/client")
                if services.engine in ("mysql", "hybrid"):
                    services.mysql("DROP TABLE IF EXISTS bench.kv;")
                if services.engine in ("redis", "hybrid"):
                    services.stop(crash=True, names={"redis"})
                    remove_fixture("/data/redis")
                    services.start(initial=True, names={"redis"})
            restart = job.get("restart_each", True) or index == len(trials) - 1
            result = run_one(services, trial, case, restart)
            report["trials"].append(result)
            print(json.dumps({"stage": "trial_passed", "trial": index, "mode": trial["mode"], "workload": trial["workload"],
                              "threads": trial["threads"], "records_per_second": result["measurement"]["records_per_second"]}), flush=True)
        report["library_sha256"] = hashlib.sha256(Path("/opt/bench/lib/libfengqudb.so.0.3.2").read_bytes()).hexdigest()
        report["executable_sha256"] = hashlib.sha256(BIN.read_bytes()).hexdigest()
        report["packages_sha256"] = hashlib.sha256(Path("/opt/bench/packages.txt").read_bytes()).hexdigest()
        if len(trials) == 1:
            report["measurement"] = report["trials"][0]["measurement"]
        report["ok"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            services.stop()
        finally:
            report["cgroup_memory_events"] = read("/sys/fs/cgroup/memory.events")
            (output / "report.json").write_text(json.dumps(report, indent=2))
    print("PASS grouped resource-matched trials", flush=True)


if __name__ == "__main__":
    main()
