"""Private Linux build and business trials. Loaded only by the owned Docker runner."""
from pathlib import Path
import argparse,gzip,hashlib,json,os,runpy,shutil,subprocess,threading,time
root,lab,data=Path('/source'),Path('/lab'),Path('/data')
legacy=runpy.run_path(str(root/'tools/final_benchmark/runner.py'))
Services,limits=legacy['Services'],legacy['limits']

def run(args,log,env=None):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,env=env,timeout=240)
    log.write_text(p.stdout+p.stderr)
    if p.returncode:raise RuntimeError(str(args[0])+' failed: '+(p.stdout+p.stderr)[-3000:])
    return p.stdout

def cpu():
    return {k:int(v) for k,v in (row.split() for row in Path('/sys/fs/cgroup/cpu.stat').read_text().splitlines())}
def io():return Path('/sys/fs/cgroup/io.stat').read_text()
def runtime():
    bin=data/'bin';bin.mkdir(exist_ok=True)
    source=lab/'libfengqudb.so.0.3.2';shutil.copy2(source,bin/source.name)
    for name in ['libfengqudb.so.1','libfengqudb.so']:(bin/name).symlink_to(source.name)
    os.environ['LD_LIBRARY_PATH']=str(bin)+':/opt/bench/lib'

def build(out):
    runtime();dst=data/'compile';dst.mkdir()
    reference=root/'artifacts/reference/yyjson-0.12.0'
    origin=json.loads((reference/'origin.json').read_text())
    for item in origin['files']:assert hashlib.sha256((reference/item['file']).read_bytes()).hexdigest()==item['sha256']
    run(['gcc','-O3','-DNDEBUG','-c',reference/'yyjson.c','-o',dst/'json.o'],out/'json-build.log')
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','mysqlclient','hiredis'],text=True).split()
    src=root/'tools/business_benchmark'
    args=['g++','-std=c++17','-O3','-DNDEBUG','-Wall','-Wextra','-pthread','-I'+str(root/'include'),'-I'+str(reference),'-I/opt/bench/include',
          *[src/x for x in ['benchmark.cpp','fq.cpp','sqlite.cpp','mysql.cpp','redis.cpp']],dst/'json.o',data/'bin/libfengqudb.so.0.3.2','/opt/bench/lib/libsqlite3_benchmark.so',*flags,'-Wl,-rpath,/data/bin:/opt/bench/lib','-o',lab/'business_bench']
    run(args,out/'native-build.log')
    versions=run(['sh','-c','g++ --version && mysqld --version && redis-server --version && ldd /lab/business_bench'],out/'versions.log')
    return dict(ok=True,executable_sha256=hashlib.sha256((lab/'business_bench').read_bytes()).hexdigest(),versions=versions)

def native(services,case,args,timed=False):
    if not timed:
        result=json.loads(run(args,case/'command.log'));assert result['ok'];return result
    active,done=threading.Event(),threading.Event();observed=dict(peak_bytes=0,samples=0)
    def sample():
        while not done.wait(.02):
            if active.is_set():observed['peak_bytes']=max(observed['peak_bytes'],int(Path('/sys/fs/cgroup/memory.current').read_text()));observed['samples']+=1
    sampler=threading.Thread(target=sample);sampler.start()
    try:
        with (case/'measurement.stdout.json').open('w') as output:
            p=subprocess.Popen(list(map(str,args)),stdout=output,stderr=subprocess.PIPE,text=True)
            watchdog=threading.Timer(180,p.kill);watchdog.start();stderr=[]
            try:
                for line in p.stderr:
                    stderr.append(line)
                    if line.strip()=='BENCH_TIMED_BEGIN':observed.update(cpu_before=cpu(),io_before=io());active.set()
                    elif line.strip()=='BENCH_TIMED_END':active.clear();observed.update(cpu_after=cpu(),io_after=io())
                code=p.wait()
            finally:watchdog.cancel()
        (case/'measurement.stderr.log').write_text(''.join(stderr))
        if code:raise RuntimeError('native failed: '+''.join(stderr))
        result=json.loads((case/'measurement.stdout.json').read_text());assert result['ok'] and observed['samples']
        observed['cpu_usage_usec']=observed['cpu_after']['usage_usec']-observed['cpu_before']['usage_usec']
        observed['cpu_cores_used']=observed['cpu_usage_usec']/1e6/result['seconds'];result['observed']=observed;return result
    finally:done.set();sampler.join()

def main():
    p=argparse.ArgumentParser();p.add_argument('--job',required=True,type=Path);args=p.parse_args();job=json.loads(args.job.read_text());out=args.job.parent
    report=dict(ok=False,job=job,limits=limits(),trials=[]);services=None
    try:
        if job.get('build'):report.update(build(out));return
        if job.get('codec'):
            runtime();report['codecs']=[]
            for round_id in range(3):
                for body in (128,4096):
                    result=json.loads(run([lab/'business_bench','--action','codec','--body',body,'--records',4096],out/('codec-%s-%s.log'%(round_id,body))))
                    result['round']=round_id;report['codecs'].append(result)
            report['ok']=True;return
        runtime();base=job['engine'];mapped=base if base in ('mysql','redis','hybrid') else 'fqdb'
        services=Services(mapped,out,job.get('redis_io_threads',1));services.start(initial=True)
        trials=job['trials']
        for at,t in enumerate(trials):
            case=out/('t%04d'%at);case.mkdir();services.output=case
            if at:
                legacy['remove_fixture']('/data/client')
                if mapped in ('redis','hybrid'):
                    services.stop(crash=True,names={'redis'});legacy['remove_fixture']('/data/redis');services.start(initial=True,names={'redis'})
            if mapped in ('mysql','hybrid'):services.mysql('SET GLOBAL innodb_flush_log_at_trx_commit=2;')
            if mapped=='redis':services.command('redis-load-policy',['redis-cli','CONFIG','SET','appendfsync','no'])
            Path('/data/client').mkdir(exist_ok=True)
            cmd=[lab/'business_bench','--engine',base,'--mode',t['mode'],'--workload',t['workload'],'--records',t['records'],'--body',t['body'],'--threads',t['threads'],'--seconds',t['seconds'],'--data','/data/client']
            item=dict(ok=False,trial=t)
            try:
                item['prepare']=native(services,case,cmd+['--action','prepare'])
                if mapped in ('mysql','hybrid'):services.mysql('SET GLOBAL innodb_flush_log_at_trx_commit='+('1' if t['mode']=='sync' else '2')+';')
                if mapped=='redis':services.command('redis-measure-policy',['redis-cli','CONFIG','SET','appendfsync','always' if t['mode']=='sync' else 'everysec'])
                item['settings_before']=services.settings()
                item['measurement']=native(services,case,cmd+['--action','run'],True)
                restart=job.get('restart_each',False) or at==len(trials)-1
                item['server_restart']=restart
                item['private_disk_bytes']=sum(p.stat().st_size for p in data.rglob('*') if p.is_file() and '/bin/' not in str(p))
                if restart:services.stop(crash=True);services.start(initial=False)
                item['independent_verify']=native(services,case,cmd+['--action','verify'])
                oracle=Path('/data/client/expected.bin').read_bytes();item['oracle_sha256']=hashlib.sha256(oracle).hexdigest();(case/'expected.bin.gz').write_bytes(gzip.compress(oracle,mtime=0))
                item['ok']=True
            finally:
                (case/'report.json').write_text(json.dumps(item,indent=2));report['trials'].append(item)
                (out/'report.json').write_text(json.dumps(report,indent=2))
            print(json.dumps(dict(engine=base,trial=at,mode=t['mode'],workload=t['workload'],threads=t['threads'],body=t['body'],ops=item['measurement']['business_ops_per_second'])),flush=True)
        report['ok']=True
    except BaseException as e:report['error']=repr(e);raise
    finally:
        if services:services.stop()
        report['memory_events']=Path('/sys/fs/cgroup/memory.events').read_text();(out/'report.json').write_text(json.dumps(report,indent=2))
if __name__=='__main__':main()
