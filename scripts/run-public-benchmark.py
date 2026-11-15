"""Freeze the current business driver and a verified Core, then run all five engines."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
ENGINES = ['mysql', 'redis', 'hybrid', 'sqlite-rows', 'fqdb']
WORKLOADS = ['C', 'save', 'insert', 'delete', 'patch', 'B', 'A', 'batch32', 'projection', 'F', 'transfer']
LABEL = 'fqdb.public-benchmark.owner'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                            encoding='utf-8', errors='replace')
    if result.returncode:
        raise RuntimeError('command failed: '+str(args[0])+'\n'+result.stdout[-3000:]+result.stderr[-3000:])
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--core-root', type=Path, required=True)
    parser.add_argument('--image', default='fqdb-public-bench:20261115')
    parser.add_argument('--base', default='fqdb-final-bench-base:20261107')
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--pilot', action='store_true')
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=1.5)
    parser.add_argument('--threads', default='1,8')
    parser.add_argument('--bodies', default='128,4096')
    parser.add_argument('--engines', default=','.join(ENGINES))
    parser.add_argument('--workloads', default=','.join(WORKLOADS))
    parser.add_argument('--reliable', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    core = args.core.resolve(strict=True)
    source = args.core_root.resolve(strict=True)
    owner = uuid.uuid4().hex
    report = {'ok': False, 'owner': owner, 'core_sha256': sha(core),
              'configuration': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              'jobs': [], 'resources': []}
    save = lambda: (output/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    save()
    context = output/'image-context'
    try:
        if args.build:
            for directory in ('include',):
                shutil.copytree(source/directory, context/directory)
            for directory in ('business_benchmark', 'public_benchmark'):
                shutil.copytree(ROOT/'tools'/directory, context/'tools'/directory,
                                ignore=shutil.ignore_patterns('__pycache__'))
            (context/'tools/final_benchmark').mkdir()
            shutil.copy2(ROOT/'tools/final_benchmark/runner.py', context/'tools/final_benchmark/runner.py')
            shutil.copytree(ROOT/'artifacts/reference/yyjson-0.12.0', context/'reference')
            (context/'native').mkdir()
            shutil.copy2(core, context/'native/libfengqudb.so.2')
            shutil.copy2(ROOT/'tools/public_benchmark/Dockerfile', context/'Dockerfile')
            base = json.loads(command('docker', 'image', 'inspect', args.base))[0]
            report['base_image_id'] = base['Id']
            result = subprocess.run(['docker', 'build', '--build-arg', 'BASE_IMAGE='+args.base,
                                      '-t', args.image, str(context)], capture_output=True, text=True,
                                     encoding='utf-8', errors='replace')
            (output/'image-build.log').write_text(result.stdout+result.stderr, encoding='utf-8')
            if result.returncode:
                raise RuntimeError('image build failed; see image-build.log')
            assert json.loads(command('docker', 'image', 'inspect', args.base))[0]['Id'] == base['Id'], 'base image changed during build'
        image = json.loads(command('docker', 'image', 'inspect', args.image))[0]
        report['image_id'] = image['Id']
        loaded_hash = command('docker', 'run', '--rm', '--entrypoint', 'sha256sum', image['Id'],
                              '/opt/public/lib/libfengqudb.so.2').split()[0]
        assert loaded_hash == report['core_sha256'], 'image does not contain the requested Core'
        report['versions'] = command('docker', 'run', '--rm', '--entrypoint', 'sh', image['Id'], '-c',
            'uname -a; g++ --version | head -1; mysqld --version; redis-server --version; '
            'lscpu | head -25; ldd /opt/public/business_bench')
        if args.build:
            report['source_manifest'] = {p.relative_to(context).as_posix(): sha(p)
                                        for p in sorted(context.rglob('*')) if p.is_file()}
            snapshot = output/'core-source'
            for directory in ('src', 'include', 'cmake', 'third_party'):
                shutil.copytree(source/directory, snapshot/directory,
                                ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
            shutil.copy2(source/'CMakeLists.txt', snapshot/'CMakeLists.txt')
            shutil.make_archive(str(output/'reproduction-inputs'), 'zip', output, 'image-context')
        selected = args.engines.split(',')
        workloads = args.workloads.split(',')
        assert set(selected) <= set(ENGINES) and set(workloads) <= set(WORKLOADS)
        assert args.runs >= 1 and args.seconds >= .05
        rounds = 1 if args.pilot else args.runs
        for round_id in range(rounds):
            order = selected if round_id % 2 == 0 else list(reversed(selected))
            for engine in order:
                cases = itertools.product(workloads, map(int, args.bodies.split(',')),
                                          map(int, args.threads.split(',')))
                trials = []
                for workload, body, threads in cases:
                    finite = workload in ('insert', 'delete')
                    records = 4096 if args.pilot or not finite or args.reliable else 65536
                    mode = 'sync' if args.reliable else 'performance' if engine == 'fqdb' else 'buffered'
                    trials.append({'workload': workload, 'body': body, 'threads': threads,
                        'records': records, 'seconds': .12 if args.pilot else args.seconds,
                        'mode': mode, 'round': round_id})
                if round_id % 2:
                    trials.reverse()
                index = len(report['jobs'])
                directory = output/f'j{index:03d}-{engine}'
                directory.mkdir()
                (directory/'job.json').write_text(json.dumps({'engine': engine, 'trials': trials}))
                name = 'fqdb-publish-'+owner[:10]+'-'+str(index)
                volume = name+'-data'
                command('docker', 'volume', 'create', '--label', LABEL+'='+owner, volume)
                resource = {'name': name, 'volume': volume, 'removed': False}
                report['resources'].append(resource)
                command('docker', 'create', '--name', name, '--label', LABEL+'='+owner, '--init',
                    '--network', 'none', '--cpus', '2', '--cpuset-cpus', '0,2', '--memory', '4g',
                    '--memory-swap', '4g', '--pids-limit', '512',
                    '--mount', 'type=volume,source='+volume+',target=/data',
                    '--mount', 'type=bind,source='+str(directory)+',target=/out',
                    image['Id'], '--job', '/out/job.json')
                save()
                print(json.dumps({'stage': 'starting', 'engine': engine, 'round': round_id,
                                  'trials': len(trials)}), flush=True)
                with (directory/'container.log').open('w', encoding='utf-8') as log:
                    process = subprocess.Popen(['docker', 'start', '-a', name], stdout=log,
                                               stderr=subprocess.STDOUT)
                    code = process.wait()
                state = json.loads(command('docker', 'inspect', name))[0]
                assert state['Config']['Labels'][LABEL] == owner
                assert code == 0 and not state['State']['OOMKilled'], 'job failed: '+str(directory)
                result = json.loads((directory/'report.json').read_text())
                assert result['ok'], 'invalid job result'
                report['jobs'].append({'path': directory.name, 'engine': engine,
                                       'round': round_id, 'trials': len(result['trials'])})
                volume_state = json.loads(command('docker', 'volume', 'inspect', volume))[0]
                assert volume_state['Labels'][LABEL] == owner and not state['State']['Running']
                command('docker', 'rm', name)
                command('docker', 'volume', 'rm', volume)
                resource['removed'] = True
                save()
        report['ok'] = True
    except BaseException as error:
        report['error'] = repr(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
