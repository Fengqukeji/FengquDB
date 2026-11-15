"""One owned, resource-limited publication benchmark job (no host services)."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import threading
import time

LEGACY = runpy.run_path('/source/tools/final_benchmark/runner.py')
Services = LEGACY['Services']
BIN = '/opt/public/business_bench'


def invoke(args, directory, measured=False):
    before = Path('/sys/fs/cgroup/cpu.stat').read_text()
    peak, samples = 0, 0
    active, finished = threading.Event(), threading.Event()

    def sample():
        nonlocal peak, samples
        while not finished.wait(.02):
            if active.is_set():
                peak = max(peak, int(Path('/sys/fs/cgroup/memory.current').read_text()))
                samples += 1

    sampler = threading.Thread(target=sample)
    if measured:
        sampler.start()
    try:
        with (directory/'stdout.json').open('w') as stdout:
            process = subprocess.Popen(list(map(str, args)), stdout=stdout,
                                       stderr=subprocess.PIPE, text=True)
            messages = []
            watchdog = threading.Timer(900, process.kill)
            watchdog.start()
            try:
                for line in process.stderr:
                    messages.append(line)
                    if line.strip() == 'BENCH_TIMED_BEGIN':
                        before = Path('/sys/fs/cgroup/cpu.stat').read_text()
                        peak = max(peak, int(Path('/sys/fs/cgroup/memory.current').read_text()))
                        samples += 1
                        active.set()
                    elif line.strip() == 'BENCH_TIMED_END':
                        active.clear()
                        after = Path('/sys/fs/cgroup/cpu.stat').read_text()
                code = process.wait()
            finally:
                watchdog.cancel()
        (directory/'stderr.log').write_text(''.join(messages))
        if code:
            raise RuntimeError(f'native exit {code}: '+''.join(messages)[-4000:])
        result = json.loads((directory/'stdout.json').read_text())
        assert result['ok']
        if measured:
            assert samples > 0
            cpu = lambda data: {k: int(v) for k, v in (row.split() for row in data.splitlines())}
            result['observed'] = {'peak_bytes': peak, 'samples': samples,
                'cpu_usage_usec': cpu(after)['usage_usec']-cpu(before)['usage_usec']}
        return result
    finally:
        finished.set()
        if measured:
            sampler.join()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--job', type=Path, required=True)
    args = parser.parse_args()
    job = json.loads(args.job.read_text())
    output = args.job.parent
    engine = job['engine']
    services = Services(engine if engine in ('mysql', 'redis', 'hybrid') else 'fqdb', output, 1)
    report = {'ok': False, 'job': job, 'limits': LEGACY['limits'](), 'trials': []}
    try:
        services.start(initial=True)
        for at, trial in enumerate(job['trials']):
            directory = output/f't{at:03d}'
            directory.mkdir()
            services.output = directory
            if at:
                LEGACY['remove_fixture']('/data/client')
                if engine in ('redis', 'hybrid'):
                    services.stop(names=['redis'])
                    LEGACY['remove_fixture']('/data/redis')
                    services.start(initial=True, names=['redis'])
            Path('/data/client').mkdir(exist_ok=True)
            if engine in ('mysql', 'hybrid'):
                services.mysql('SET GLOBAL innodb_flush_log_at_trx_commit=2;')
            if engine == 'redis':
                services.command('load-policy', ['redis-cli', 'CONFIG', 'SET', 'appendfsync', 'no'])
            command = [BIN, '--engine', engine, '--mode', trial['mode'], '--workload', trial['workload'],
                       '--records', trial['records'], '--body', trial['body'], '--threads', trial['threads'],
                       '--seconds', trial['seconds'], '--data', '/data/client', '--period', '50',
                       '--threshold', '1048576']
            item = {'ok': False, 'trial': trial}
            for action in ('prepare', 'run', 'verify'):
                target = directory/action
                target.mkdir()
                if action == 'run':
                    if engine in ('mysql', 'hybrid'):
                        services.mysql('SET GLOBAL innodb_flush_log_at_trx_commit='+
                                       ('1' if trial['mode'] == 'sync' else '2')+';')
                    if engine == 'redis':
                        services.command('run-policy', ['redis-cli', 'CONFIG', 'SET', 'appendfsync',
                                                        'always' if trial['mode'] == 'sync' else 'everysec'])
                    item['settings'] = services.settings()
                item[action] = invoke(command+['--action', action], target, action == 'run')
            oracle = Path('/data/client/expected.bin').read_bytes()
            item['oracle_sha256'] = hashlib.sha256(oracle).hexdigest()
            (directory/'oracle.bin.gz').write_bytes(gzip.compress(oracle, mtime=0))
            item['ok'] = True
            report['trials'].append(item)
            (output/'report.json').write_text(json.dumps(report, indent=2))
            print(json.dumps({'stage': 'trial_passed', 'engine': engine, 'case': trial,
                              'ops': item['run']['visible_ops_per_second']}), flush=True)
        report['ok'] = True
    except BaseException as error:
        report['error'] = repr(error)
        raise
    finally:
        services.stop()
        report['memory_events'] = Path('/sys/fs/cgroup/memory.events').read_text()
        (output/'report.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
