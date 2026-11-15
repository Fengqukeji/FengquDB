"""Check first-build setuptools packaging and an isolated installed-style import."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--cli', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
work = args.output.resolve()
work.mkdir(parents=True, exist_ok=False)
models = work/'src/appmodels'
models.mkdir(parents=True)
(models/'__init__.py').write_text('', encoding='utf-8')
shutil.copy2(ROOT/'examples/entity-sources/python/student.py', models/'student.py')
shutil.copy2(ROOT/'integrations/python/build_py.py', work/'fqdb_build.py')
(work/'setup.py').write_text('''from setuptools import setup, find_packages
from fqdb_build import BuildFengquDBModels
setup(name="fqdb-build-probe", version="0.0.1", packages=find_packages("src"),
      package_dir={"":"src"},cmdclass={"build_py":BuildFengquDBModels})
''', encoding='utf-8')
env = dict(os.environ, FQDB_CODEGEN=str(args.cli.resolve()), FQDB_ENTITY_SOURCE='src')
checks = []
commands = [
    ('first-build', [sys.executable,'setup.py','build_py','--build-lib','installed']),
    ('manual-replay', [str(args.cli.resolve()),'project','--root',str(work),'--check']),
    ('isolated-import', [sys.executable,'-I','-c',
        'import sys;sys.path.insert(0,sys.argv[1]);'
        'from appmodels.student import Student;'
        'from fqdb_generated.appmodels.student.fq_codecs import StudentFqCodec as C;'
        'r=C.decode(C.encode(Student(7,"Alice",95)));'
        'assert isinstance(r.value,Student) and r.value.score==95;'
        'print("PASS first package build and isolated original-entity codec")',
        str(work/'installed')])]
try:
    assert not (work/'Generated').exists()
    for name,command in commands:
        r = subprocess.run(command,cwd=work,env=env,capture_output=True,text=True,
                           encoding='utf-8',errors='replace')
        (work/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        checks.append({'name':name,'exit_code':r.returncode})
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
    print('PASS setuptools first build, src layout, manual replay, isolated import')
finally:
    (work/'report.json').write_text(json.dumps({'ok':len(checks)==3 and all(x['exit_code']==0 for x in checks),'checks':checks},indent=2),encoding='utf-8')
