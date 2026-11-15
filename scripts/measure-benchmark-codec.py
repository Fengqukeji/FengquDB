"""Measure the frozen driver's codecs after the database comparison has finished."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,required=True)
    args=parser.parse_args()
    root=args.input.resolve()
    report=json.loads((root/'report.json').read_text())
    assert report['ok'], 'Finish all database measurements first'
    results=[]
    for body in (128,4096):
        result=subprocess.run(['docker','run','--rm','--network','none','--cpus','2',
            '--cpuset-cpus','0,2','--memory','4g','--memory-swap','4g',
            '--entrypoint','/opt/public/business_bench',report['image_id'],
            '--action','codec','--records','4096','--body',str(body)],
            capture_output=True,text=True,encoding='utf-8',errors='replace',check=True)
        value=json.loads(result.stdout)
        assert value['ok'] and len(value['samples'])==4
        results.append(value)
    (root/'codec.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(results))


if __name__=='__main__':main()
