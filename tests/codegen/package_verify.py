"""Verify each ZIP independently after copying its only folder into a project."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[2]

def manifest_check(sdk):
    m=json.loads((sdk/'manifest.json').read_text(encoding='utf-8'))
    assert sdk.name=='FengquDB' and m['format']=='fqdb-language-sdk-1'
    expected={x['path'] for x in m['files']}
    assert expected=={p.relative_to(sdk).as_posix() for p in sdk.rglob('*') if p.is_file() and p.name!='manifest.json'}
    archive=sdk.parent.parent/(sdk.parent.name+'.zip')
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert set(z.namelist())=={'FengquDB/'+x for x in expected}|{'FengquDB/manifest.json'}
        for item in m['files']:
            data=(sdk/item['path']).read_bytes()
            assert len(data)==item['size'] and hashlib.sha256(data).hexdigest()==item['sha256']
            assert z.read('FengquDB/'+item['path'])==data
    assert {p.name for p in sdk.glob('*.md')}=={'GettingStarted.md','API.md','接入.md','API.zh-CN.md'}
    assert m['documentation_languages']==['en','zh-CN']
    for p in sdk.glob('*.md'):
        text=p.read_text(encoding='utf-8')
        assert not any(x in text for x in ('旧包','ProjectReference','共同契约','unity/Assets'))
        for link in re.findall(r'\[[^\]]+\]\(([^)]+)\)',text):
            if '://' not in link and not link.startswith('#'):assert (p.parent/link.split('#')[0]).is_file()
    compiler=sdk/('Editor/Tools' if m['language']=='unity' else 'Tools')/('fqdb-codegen.exe' if m['host'].startswith('win') else 'fqdb-codegen')
    assert compiler.is_file() and (sdk/'fqdb-generate.bat').is_file() and (sdk/'fqdb-generate.command').is_file()
    if m['language']=='unity':
        assert (sdk/'FengquDB.Generator.dll').is_file()
        assert 'RoslynAnalyzer' in (sdk/'FengquDB.Generator.dll.meta').read_text()
        assert (sdk/'Runtime/iOS/FengquDB.Managed.dll').exists()==('ios-arm64' in m['native_targets'])
    else:assert (sdk/m['native_library']).is_file()
    return m

def main():
    p=argparse.ArgumentParser();g=p.add_mutually_exclusive_group(required=True)
    g.add_argument('--sdk',type=Path);g.add_argument('--packages',type=Path)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--manifest-only',action='store_true')
    p.add_argument('--cmake',default='cmake');p.add_argument('--java-bin',type=Path);p.add_argument('--dotnet',action='store_true')
    p.add_argument('--languages',help='Run integration only for these comma-separated languages; check every manifest')
    args=p.parse_args();work=args.output.resolve();work.mkdir(parents=True,exist_ok=False)
    sdks=[args.sdk.resolve()] if args.sdk else sorted(args.packages.resolve().glob('*/FengquDB'))
    assert sdks,'No independent SDKs found';report=[]
    def run(name,cmd,cwd,env):
        r=subprocess.run(list(map(str,cmd)),cwd=cwd,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
        (work/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        if r.returncode:raise RuntimeError(name+'\n'+r.stdout+r.stderr)
        return r.stdout
    for sdk in sdks:
        m=manifest_check(sdk);lang=m['language'];checks=['independent-zip-manifest-docs']
        if not args.manifest_only and (not args.languages or lang in args.languages.split(',')):
            project=work/lang;project.mkdir();local=project/'FengquDB'
            archive=sdk.parent.parent/(sdk.parent.name+'.zip')
            with zipfile.ZipFile(archive) as package:
                for item in package.infolist():
                    if not (project/item.filename).resolve().is_relative_to(project):
                        raise ValueError('Unsafe SDK archive member')
                package.extractall(project)
                if os.name!='nt':
                    for item in package.infolist():
                        if not item.is_dir():os.chmod(project/item.filename,(item.external_attr>>16)&0o777)
            # Consumers must run with their normal environment, without a test-only library path.
            env=dict(os.environ,CI='1',PYTHONUTF8='1',PYTHONIOENCODING='utf-8')
            tool=local/('Editor/Tools' if lang=='unity' else 'Tools')/('fqdb-codegen.exe' if os.name=='nt' else 'fqdb-codegen')
            assert run(lang+'-tool',[tool,'--version'],project,env).strip()==m['version']
            checks+=['extracted-release-zip','matching-tool-version']
            if lang=='csharp' and not args.dotnet:raise RuntimeError('C# consumer verification requires --dotnet')
            if lang=='java' and not args.java_bin:raise RuntimeError('Java consumer verification requires --java-bin')
            if lang=='go' and not shutil.which('go'):raise RuntimeError('Go compiler is required')
            if lang=='rust' and not shutil.which('cargo'):raise RuntimeError('Rust compiler is required')
            if lang=='unity':
                (project/'Assets/Plugins').mkdir(parents=True);(project/'ProjectSettings').mkdir()
                # The whole folder is the sole integration operation.
                local.rename(project/'Assets/Plugins/FengquDB');local=project/'Assets/Plugins/FengquDB'
                tool=local/'Editor/Tools'/tool.name
                student=next(b for b in re.findall(r'```csharp\n(.*?)\n```',(local/'接入.md').read_text(encoding='utf-8'),re.S) if 'public partial class Student' in b)
                (project/'Assets/Student.cs').write_text(student,encoding='utf-8')
                script=local/('fqdb-generate.bat' if os.name=='nt' else 'fqdb-generate.command')
                run('unity-nested-double-click',[os.environ.get('COMSPEC','cmd.exe'),'/d','/c',script] if os.name=='nt' else ['sh',script],project,env)
                assert (project/'Assets/Generated/csharp/global_entities/fq_codecs.cs').is_file()
                checks+=['plugins-contained-manual-tool']
            elif lang in ('c','cpp'):
                ext='h' if lang=='c' else 'hpp'
                model=(ROOT/'examples/entity-sources'/lang/('student.'+ext)).read_text(encoding='utf-8') if lang=='c' else re.findall(r'```cpp\n(.*?)\n```',(local/'接入.md').read_text(encoding='utf-8'),re.S)[0]
                (project/('student.'+ext)).write_text(model,encoding='utf-8')
                run(lang+'-generate',[tool,'project','--sdk',local],project,env)
                code='''#include <fengqudb_store.hpp>
#include "Generated/cpp/fq_codecs.hpp"
int main(){auto db=fengqudb::Store::open_at("save","main");using C=fqdb_generated::models::StudentFqCodec;Student s{7,"Alice",95};db.put(C::key(s),C::encode(s));auto b=db.get(C::key(s));return C::decode(b->view()).value.score==95?0:1;}
''' if lang=='cpp' else '''#include <fengqudb_store.h>
int main(void){fqdb_store*s=0;int r=fqdb_store_open_at("save","main",0,&s);fqdb_store_close(s);return r;}
'''
                (project/('main.'+('cpp' if lang=='cpp' else 'c'))).write_text(code,encoding='utf-8')
                generation='fengqudb_generate_models(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/Generated" TARGET models)\nadd_dependencies(probe models)\n' if lang=='cpp' else ''
                (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.15)\nproject(Probe LANGUAGES '+('CXX' if lang=='cpp' else 'C')+')\nfind_package(FengquDB CONFIG REQUIRED)\nadd_executable(probe main.'+('cpp' if lang=='cpp' else 'c')+')\nfengqudb_link(probe)\n'+generation,encoding='utf-8')
                run(lang+'-configure',[args.cmake,'-S','.','-B','build','-DCMAKE_PREFIX_PATH='+str(local)],project,env)
                run(lang+'-build',[args.cmake,'--build','build','--config','Release'],project,env)
                run(lang+'-run',[project/('build/Release/probe.exe' if os.name=='nt' else 'build/probe')],project,env);checks+=['compile-generated-code-and-native-call']
                executable=project/('build/Release/probe.exe' if os.name=='nt' else 'build/probe')
                published=project/'published';published.mkdir()
                shutil.copy2(executable,published/executable.name)
                for library in executable.parent.glob('*fengqudb*'):
                    if library.suffix in ('.dll','.so','.dylib') or '.so.' in library.name:
                        shutil.copy2(library,published/library.name)
                run(lang+'-published',[published/executable.name],work,env);checks+=['relocated-runtime-deployment']
            elif lang=='csharp' and args.dotnet:
                guide=(local/'接入.md').read_text(encoding='utf-8');blocks=re.findall(r'```csharp\n(.*?)\n```',guide,re.S)
                student=next(b for b in blocks if 'public partial class Student' in b)
                app=next(b for b in blocks if 'public static class AppDb' in b)
                app=app.replace('Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)','Directory.GetCurrentDirectory()')
                (project/'Student.cs').write_text(student,encoding='utf-8')
                (project/'AppDb.cs').write_text(app,encoding='utf-8')
                (project/'Program.cs').write_text('''using System;using System.IO;using System.Threading.Tasks;using FengquDB;
AppDb.Close();
if(Directory.Exists("MyApp"))throw new Exception("Closing an unused singleton opened a database");
var stores=new Store[8];Parallel.For(0,stores.Length,i=>stores[i]=AppDb.Main);
try {
 foreach(var s in stores)if(!ReferenceEquals(s,AppDb.Main))throw new Exception("Duplicate singleton");
 AppDb.Main.Put(new Student{Id=7,Name="Alice",Score=95});
 if(stores[1].GetEntity<Student>(7).Score!=95)throw new Exception();
 AppDb.Main.Put("score",95);if(AppDb.Main.Get<int>("score")!=95)throw new Exception();
}finally{AppDb.Close();}
using var reopened=new Store(Path.Combine(Directory.GetCurrentDirectory(),"MyApp","FengquDB","main"));
if(reopened.GetEntity<Student>(7).Score!=95)throw new Exception("Reopen failed");
Console.WriteLine("PASS shared singleton, close and reopen");
''',encoding='utf-8')
                (project/'Probe.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework></PropertyGroup><Import Project="FengquDB/FengquDB.targets"/></Project>',encoding='utf-8')
                run('csharp-build',['dotnet','build','-c','Release','--nologo'],project,env)
                run('csharp-run',['dotnet',project/'bin/Release/net10.0/Probe.dll'],project,env);checks+=['documented-singleton-auto-generation-close-reopen']
            elif lang=='python':
                blocks=re.findall(r'```python\n(.*?)\n```',(local/'接入.md').read_text(encoding='utf-8'),re.S)
                (project/'student.py').write_text(blocks[0],encoding='utf-8');(project/'main.py').write_text(blocks[1],encoding='utf-8')
                run('python-doc-example',[sys.executable,'main.py'],project,env);checks+=['documentation-startup-generation-roundtrip']
            elif lang=='node':
                blocks=re.findall(r'```js\n(.*?)\n```',(local/'接入.md').read_text(encoding='utf-8'),re.S)
                (project/'student.js').write_text(blocks[1],encoding='utf-8')
                app=blocks[2]+'\n'+blocks[0].replace('  db.sync();',blocks[3]+'\n  db.sync();')
                (project/'app.cjs').write_text(app,encoding='utf-8')
                run('node-generate',['node','FengquDB/fqdb-generate.cjs'],project,env)
                clean_env=dict(env,PATH=os.environ.get('PATH',''),LD_LIBRARY_PATH=os.environ.get('LD_LIBRARY_PATH',''))
                run('node-doc-example',['node','app.cjs'],project,clean_env);checks+=['documentation-prebuild-roundtrip']
            elif lang=='java' and args.java_bin:
                (project/'Probe.java').write_text('import io.fengqudb.Store;public class Probe{public static void main(String[]a){try(Store s=Store.openAt("save","main")){s.put(new byte[]{1},new byte[]{2},true);if(s.get(new byte[]{1})[0]!=2)throw new AssertionError();}}}',encoding='utf-8')
                run('java-compile',[args.java_bin/'javac','-cp',local/'fengqudb.jar','Probe.java'],project,env)
                run('java-run',[args.java_bin/'java','-cp',str(local/'fengqudb.jar')+os.pathsep+str(project),'Probe'],work,env);checks+=['prebuilt-jar-auto-native-path-roundtrip']
            elif lang=='go' and shutil.which('go'):
                (project/'go.mod').write_text('module consumer\ngo 1.21\nrequire fengqudb v0.0.0\nreplace fengqudb => ./FengquDB\n',encoding='utf-8')
                (project/'main.go').write_text('package main\nimport "fengqudb"\nfunc main(){s,e:=fengqudb.Open("db",nil);if e!=nil{panic(e)};defer s.Close();if e=s.Put([]byte("k"),[]byte("v"));e!=nil{panic(e)};v,e:=s.Get([]byte("k"));if e!=nil||string(v)!="v"{panic("read")};if e=s.Sync();e!=nil{panic(e)}}',encoding='utf-8')
                script=local/('run.bat' if os.name=='nt' else 'run.sh')
                run('go-client',[os.environ.get('COMSPEC','cmd.exe'),'/d','/c',script,'.'] if os.name=='nt' else ['sh',script,'.'],project,env)
                (project/'models').mkdir()
                (project/'models/student.go').write_text('package models\n// fqdb:entity prefix=student/ key=Id\ntype Student struct { Id int64; Name string; Score int32 }\n',encoding='utf-8')
                (project/'main.go').write_text('package main\nimport("fmt";"fengqudb";"consumer/models";codecs "consumer/Generated/go/models")\nfunc main(){s,e:=fengqudb.Open("db",nil);if e!=nil{panic(e)};defer s.Close();v:=models.Student{Id:7,Name:"Alice",Score:95};k,e:=codecs.StudentFqKey(&v);if e!=nil{panic(e)};b,e:=codecs.StudentFqEncode(&v);if e!=nil{panic(e)};if e=s.Put(k,b);e!=nil{panic(e)};fmt.Println(v.Name,v.Score);if e=s.Sync();e!=nil{panic(e)}}',encoding='utf-8')
                script=local/('build.bat' if os.name=='nt' else 'build.sh')
                run('go-build',[os.environ.get('COMSPEC','cmd.exe'),'/d','/c',script,'.'] if os.name=='nt' else ['sh',script,'.'],project,env)
                run('go-published',[project/('bin/consumer.exe' if os.name=='nt' else 'bin/consumer')],work,env);checks+=['sdk-run-build-deploy-native-call']
            elif lang=='rust' and shutil.which('cargo'):
                (project/'src').mkdir();(project/'Cargo.toml').write_text('[package]\nname="consumer"\nversion="0.1.0"\nedition="2021"\n[dependencies]\nfengqudb={path="FengquDB/Crates/fengqudb"}\n',encoding='utf-8')
                (project/'build.rs').write_text('include!("FengquDB/generate.rs");',encoding='utf-8')
                env['CARGO_TARGET_DIR']=str(project/'target')
                (project/'src/main.rs').write_text('use fengqudb::{Store,Options};fn main()->Result<(),Box<dyn std::error::Error>>{let db=Store::open("db",&Options::default())?;db.put(b"k",b"v")?;assert_eq!(db.get(b"k")?.unwrap(),b"v");db.sync()?;Ok(())}',encoding='utf-8')
                run('rust-client',['cargo','run','--offline'],project,env)
                guide=(local/'接入.md').read_text(encoding='utf-8')
                blocks=re.findall(r'```rust\n(.*?)\n```',guide,re.S)
                (project/'src/student.rs').write_text(next(b for b in blocks if 'pub struct Student' in b),encoding='utf-8')
                (project/'src/main.rs').write_text(next(b for b in blocks if 'fn main()' in b),encoding='utf-8')
                run('rust-generated',['cargo','run','--offline'],project,env)
                executable=project/('target/debug/consumer.exe' if os.name=='nt' else 'target/debug/consumer')
                published=project/'published';published.mkdir()
                shutil.copy2(executable,published/executable.name)
                runtime='fengqudb.dll' if os.name=='nt' else 'libfengqudb.so.2'
                shutil.copy2(executable.parent/runtime,published/runtime)
                run('rust-published',[published/executable.name],work,env);checks+=['cargo-hook-deploy-native-call','relocated-runtime-deployment']
        report.append({'language':lang,'ok':True,'checks':checks})
        (work/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    (work/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))

if __name__=='__main__':main()
