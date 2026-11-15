"""Check that a killed codegen lock holder does not leave the project blocked."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
work=args.output.resolve();work.mkdir(parents=True,exist_ok=False)
(work/'model.py').write_text('# fqdb:entity prefix=counter/ key=id\nclass Counter:\n    id: int = 0\n    value: int = 0\n',encoding='utf-8')
cmd=[str(args.cli.resolve()),'project','--root',str(work)]
def run(expected):
    result=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8')
    if result.returncode!=expected:raise RuntimeError(result.stdout+result.stderr)
    return result
run(0)
code=r'''
import sys,os
if os.name=='nt':
 import ctypes as C
 api=C.WinDLL('kernel32',use_last_error=True);api.CreateFileW.argtypes=[C.c_wchar_p,C.c_uint32,C.c_uint32,C.c_void_p,C.c_uint32,C.c_uint32,C.c_void_p];api.CreateFileW.restype=C.c_void_p
 h=api.CreateFileW(sys.argv[1],0xc0010000,0,None,4,0x04000100,None)
 if h==C.c_void_p(-1).value:raise OSError(C.get_last_error())
else:
 import fcntl
 h=open(sys.argv[1],'a+b');fcntl.flock(h,fcntl.LOCK_EX|fcntl.LOCK_NB)
print('locked',flush=True)
sys.stdin.read()
'''
child=subprocess.Popen([sys.executable,'-u','-c',code,str(work/'.fqdb-codegen.lock')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
    if child.stdout.readline().strip()!='locked':raise RuntimeError(child.stderr.read())
    run(1)
finally:
    child.kill();child.wait(timeout=10)
run(0)
(work/'report.json').write_text(json.dumps({'ok':True,'checks':['exclusive lock','process kill releases lock','regeneration succeeds']}),encoding='utf-8')
print('PASS exclusive lock and crash recovery')
