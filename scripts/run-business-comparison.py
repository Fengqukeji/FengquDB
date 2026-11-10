"""Resource-matched business benchmarks; only labelled private Docker data are removed."""
import argparse,hashlib,json,subprocess,sys,uuid,itertools,shutil,time
from pathlib import Path

def main():
    sys.stdout.reconfigure(encoding='utf-8');p=argparse.ArgumentParser(__doc__)
    p.add_argument('--phase',choices=['build','codec','pilot','matrix'],default='pilot');p.add_argument('--output',type=Path,required=True)
    p.add_argument('--runtime',type=Path,default=Path('build/business-comparison-20261108/runtime'))
    p.add_argument('--redis-io-threads',type=int,choices=[1,2],default=1)
    p.add_argument('--representative',action='store_true',help='Use 1/8 clients for large profiles and 8 clients for small profiles')
    p.add_argument('--engines',default='fqdb,sqlite-rows,sqlite-json,sqlite-jsonb,sqlite-pb,mysql,redis,hybrid');p.add_argument('--workloads',default='C,projection,B,A,F,batch32,transfer')
    p.add_argument('--threads',default='1,8,32');p.add_argument('--bodies',default='128,4096');p.add_argument('--modes',default='buffered,sync');p.add_argument('--runs',type=int,default=1);p.add_argument('--seconds',type=float,default=2);p.add_argument('--records',type=int,default=16384)
    args=p.parse_args();root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);runtime=args.runtime.resolve();runtime.mkdir(parents=True,exist_ok=True)
    owner=uuid.uuid4().hex;label='fqdb.business.owner'
    # Public record resolves the exact image instead of relying on a mutable tag.
    image=json.loads((root/'build/store-only-20261108/sync-pilot-resource.json').read_text())['image_id']
    report=dict(ok=False,owner=owner,image=image,phase=args.phase,resources=[],jobs=[])
    def docker(*v):
        q=subprocess.run(['docker',*map(str,v)],capture_output=True,text=True,encoding='utf-8',errors='replace');q.check_returncode();return q.stdout
    def save():(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    def inspect(name,volume=False):return json.loads(docker('volume','inspect',name) if volume else docker('inspect',name))[0]
    def owned(name,volume=False):
        d=inspect(name,volume);labels=d.get('Labels') if volume else d['Config']['Labels'];assert labels.get(label)==owner;return d
    try:
        running=docker('ps','--format','{{.Names}}').splitlines();assert all(x=='buildx_buildkit_desktop-linux' for x in running),running
        source=root/'build/shared-read-20261108/libfengqudb.so.0.3.2';assert hashlib.sha256(source.read_bytes()).hexdigest()=='2f11cb668c212167f8adc202e31c1c69b456f44aec2de5d623e0317ad092042d'
        if args.phase=='build':shutil.copy2(source,runtime/source.name);jobs=[dict(build=True)]
        elif args.phase=='codec':jobs=[dict(codec=True)]
        else:
            assert (runtime/'business_bench').is_file()
            names=args.engines.split(',');assert set(names)<=set('fqdb,sqlite-rows,sqlite-json,sqlite-jsonb,sqlite-pb,mysql,redis,hybrid'.split(','))
            jobs=[]
            for round_id in range(args.runs):
                for engine in names if round_id%2==0 else list(reversed(names)):
                    trials=[]
                    if args.phase=='pilot':
                        cases=itertools.product(['buffered','sync'],args.workloads.split(','),[2],[128])
                    else:cases=itertools.product(args.modes.split(','),args.workloads.split(','),map(int,args.threads.split(',')),map(int,args.bodies.split(',')))
                    for mode,w,t,b in cases:
                        if args.representative and b==128 and t!=8:continue
                        if mode=='sync' and w in ('C','projection','batch32') and args.phase!='pilot':continue
                        trials.append(dict(engine=engine,mode=mode,workload=w,threads=t,body=b,records=256 if args.phase=='pilot' else args.records,seconds=.08 if args.phase=='pilot' else args.seconds,round=round_id))
                    jobs.append(dict(engine=engine,trials=trials,restart_each=args.phase=='pilot',redis_io_threads=args.redis_io_threads))
        files=[x for x in (root/'tools/business_benchmark').glob('*') if x.is_file() and x.name!='summarize.py']+[Path(__file__).resolve()]
        report['source_manifest']={x.relative_to(root).as_posix():hashlib.sha256(x.read_bytes()).hexdigest() for x in files};report['core_sha256']=hashlib.sha256(source.read_bytes()).hexdigest()
        if args.phase!='build':report['executable_sha256']=hashlib.sha256((runtime/'business_bench').read_bytes()).hexdigest()
        for index,job in enumerate(jobs):
            case=out/('j%03d'%index);case.mkdir();(case/'job.json').write_text(json.dumps(job,indent=2));name='fqdb-business-'+owner[:10]+'-'+str(index);volume=name+'-data'
            docker('volume','create','--label',label+'='+owner,volume);resource=dict(name=name,volume=volume,removed=False);report['resources'].append(resource);save()
            command=['create','--name',name,'--label',label+'='+owner,'--init','--network','none','--cpus','2','--cpuset-cpus','0,2','--memory','4g','--memory-swap','4g','--pids-limit','512',
                '--mount','type=volume,source='+volume+',target=/data','--mount','type=bind,source='+str(root)+',target=/source,readonly',
                '--mount','type=bind,source='+str(runtime)+',target=/lab','--mount','type=bind,source='+str(case)+',target=/out','--entrypoint','python3',image,'/source/tools/business_benchmark/container.py','--job','/out/job.json']
            docker(*command);state=owned(name);assert state['HostConfig']['NanoCpus']==2000000000 and state['HostConfig']['Memory']==4<<30
            print(json.dumps(dict(stage='starting',engine=job.get('engine','build'),job=index,trials=len(job.get('trials',[])))),flush=True)
            with (case/'container.log').open('w',encoding='utf-8') as log:
                q=subprocess.Popen(['docker','start','-a',name],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
                for line in q.stdout:log.write(line);log.flush();print(line,end='',flush=True)
                code=q.wait()
            state=owned(name);resource.update(exit_code=code,oom_killed=state['State']['OOMKilled']);save()
            result=json.loads((case/'report.json').read_text());report['jobs'].append(dict(path=case.name,engine=job.get('engine','build'),ok=result['ok']));save()
            assert code==0 and result['ok'] and not state['State']['Running'] and not state['State']['OOMKilled']
            owned(volume,True);docker('rm',name);docker('volume','rm',volume);resource['removed']=True;save()
        report['ok']=True
    except BaseException as e:report['error']=repr(e);raise
    finally:save()
if __name__=='__main__':main()
