"""Validate source-first generation without installing any language toolchain."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--cli',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--languages',default='python,node')
    p.add_argument('--cmake',default='cmake')
    p.add_argument('--java-bin',type=Path)
    p.add_argument('--library',type=Path)
    args=p.parse_args();languages=args.languages.split(',')
    if not languages or not set(languages)<={'c','cpp','csharp','java','python','node','go','rust'}:p.error('Select supported languages')
    work=args.output.resolve();work.mkdir(parents=True,exist_ok=False);cli=args.cli.resolve()
    env=dict(os.environ,DOTNET_CLI_UI_LANGUAGE='en',CI='1')
    if args.library:
        env['PATH']=str(args.library.resolve().parent)+os.pathsep+env.get('PATH','')
        env['LD_LIBRARY_PATH']=str(args.library.resolve().parent)+os.pathsep+env.get('LD_LIBRARY_PATH','')
        env['FENGQUDB_LIBRARY']=str(args.library.resolve())
        env['FQDB_LIB_DIR']=str(args.library.resolve().parent)
    report={'ok':False,'checks':[]}
    def run(label,cmd,cwd,expected=0):
        r=subprocess.run([str(x) for x in cmd],cwd=cwd,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
        (work/(label+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        report['checks'].append({'name':label,'exit_code':r.returncode,'expected':expected});print(label,r.returncode,flush=True)
        if r.returncode!=expected:raise RuntimeError(label+'\n'+r.stdout+'\n'+r.stderr)
        return r.stdout
    try:
        for lang in languages:
            project=work/lang;shutil.copytree(ROOT/'examples/entity-sources'/lang,project)
            out=project/'Generated';command=[cli,'generate','--source',project,'--source-language',lang,'--output',out]
            run(lang+'-generate',command,project)
            run(lang+'-check',command+['--check'],project)
            run(lang+'-project-replay',[cli,'project','--root',project,'--check'],project)
            known=bytes.fromhex('080e120ae5bca0e4b889f09f998218be01');unknown=known+bytes.fromhex('a0062a')
            (project/'known.bin').write_bytes(known);(project/'unknown.bin').write_bytes(unknown)
            registry=json.loads((project/'FengquDB.schema.lock.json').read_text())
            assert len(registry['entities'])>=1
            if lang=='python':
                sys.path.insert(0,str(project));sys.path.insert(0,str(out/'python'))
                from fqdb_generated.student.fq_codecs import StudentFqCodec,CounterFqCodec
                import student
                assert StudentFqCodec.Fields.score==3
                for data in [known,unknown]:
                    row=StudentFqCodec.decode(data);assert isinstance(row.value,student.Student)
                    assert (row.value.id,row.value.name,row.value.score)==(7,'张三🙂',95)
                    assert row.fq_encode()==data and row.fq_key()==b'student/7'
                counter=student.Counter();counter.id=2;counter.title='counter';counter.total=-4
                assert CounterFqCodec.decode(CounterFqCodec.encode(counter)).value.total==-4
                if args.library:
                    sys.path.insert(0,str(ROOT/'bindings/python'));from fengqudb import Store
                    with Store(project/'db',library=args.library) as db:
                        db.put(StudentFqCodec.key(row.value),row.fq_encode(),force_reliable=True)
                        assert StudentFqCodec.decode(db.get(row.fq_key())).value.score==95
                # Reorder and add a field: numbers stay stable; removed ones stay reserved.
                original=(project/'student.py').read_text()
                changed=original.replace('    name: str = ""\n    score: int = 0','    score: int = 0\n    name: str = ""\n    level: int = 0')
                (project/'student.py').write_text(changed)
                run('python-source-stale',command+['--check'],project,2)
                run('python-source-update',command,project)
                after=json.loads((project/'FengquDB.schema.lock.json').read_text())
                key=next(k for k in registry['entities'] if k.endswith(':Student'))
                before_fields=registry['entities'][key]['fields'];after_fields=after['entities'][key]['fields']
                assert all(after_fields[k]==v for k,v in before_fields.items()) and after_fields['level']['number']==4
                (project/'student.py').write_text(changed.replace('score: int','score: str'))
                snapshot={x:x.read_bytes() for x in out.rglob('*') if x.is_file()}
                run('python-type-change-rejected',command,project,1)
                assert all(x.read_bytes()==v for x,v in snapshot.items())
                (project/'student.py').write_text(changed.replace('    name: str = ""\n',''))
                run('python-remove-field',command,project)
                model=json.loads(next((out/'model/python').glob('*.json')).read_text())
                student_schema=next(e for e in model['entities'] if e['name']=='Student');assert 2 in student_schema['reserved']
                (project/'student.py').write_text(changed.replace('    level: int = 0','    level = None'))
                run('python-ambiguous-rejected',command,project,1)
                (project/'student.py').write_text(original)
                run('python-restore-fields',command,project)
                lock=project/'FengquDB.schema.lock.json'
                backup=lock.with_suffix('.saved')
                snapshot={x:x.read_bytes() for x in out.rglob('*') if x.is_file()}
                lock.rename(backup)
                try:
                    run('python-missing-lock-rejected',command,project,1)
                    assert not lock.exists() and all(x.read_bytes()==v for x,v in snapshot.items())
                finally:backup.rename(lock)
                (out/'user-note.txt').write_text('keep')
                history=lock.read_bytes()
                (project/'student.py').write_text('# No remaining persisted entities.\n')
                run('python-remove-last-entity',command,project)
                assert not any(x.is_file() for x in (out/'python').rglob('*.py'))
                assert not any(x.is_file() for x in (out/'model/python').rglob('*'))
                assert lock.read_bytes()==history and (out/'user-note.txt').read_text()=='keep'
                run('python-empty-current',command+['--check'],project)
                mixed=work/'mixed';mixed.mkdir()
                (mixed/'one.py').write_text('# fqdb:entity prefix=one/ key=id\nclass One:\n    id: int = 0\n')
                (mixed/'two.js').write_text('// fqdb:entity prefix=two/ key=id\nclass Two { id = 0; }\nmodule.exports={Two};\n')
                mixed_out=mixed/'Generated'
                mixed_cmd=[cli,'generate','--source',mixed,'--output',mixed_out]
                run('mixed-initial',mixed_cmd,mixed)
                assert (mixed_out/'node/models/fq_codecs.js').is_file()
                (mixed/'two.js').write_text('// No JS entities remain.\n')
                run('mixed-remove-language',mixed_cmd,mixed)
                assert not any(x.is_file() for x in (mixed_out/'node').rglob('*'))
                assert not any(x.is_file() for x in (mixed_out/'model/node').rglob('*'))
                assert (mixed_out/'python/fqdb_generated/one/fq_codecs.py').is_file()
                sys.path.remove(str(project));sys.path.remove(str(out/'python'))
                report['checks'].append({'name':'python-original-entity-roundtrip','exit_code':0,'expected':0})
            elif lang=='node':
                sdk=project/'FengquDB';(sdk/'Tools').mkdir(parents=True)
                (sdk/'.fqdb-sdk').write_text(json.dumps({'format':'fqdb-language-sdk-1','language':'node'}))
                if os.name=='nt':
                    shutil.copy2(ROOT/'scripts/fqdb-generate.bat',sdk/'fqdb-generate.bat')
                    shutil.copy2(cli,sdk/'Tools/fqdb-codegen.exe')
                    run('windows-batch',['cmd.exe','/d','/c',str(sdk/'fqdb-generate.bat')],project)
                else:
                    shutil.copy2(ROOT/'scripts/fqdb-generate.command',sdk/'fqdb-generate.command')
                    env['FQDB_CODEGEN']=str(cli)
                    run('posix-command',['sh',sdk/'fqdb-generate.command'],project)
                (project/'probe.js').write_text("""const fs=require('fs'),assert=require('assert'),{Student}=require('./student');
const {StudentFqCodec:C}=require('./Generated/node/models/fq_codecs');
assert.equal(C.fields.score,3);for(const bad of [1.5,NaN,Number.MAX_SAFE_INTEGER+1]){const v=new Student();v.id=bad;assert.throws(()=>C.key(v));}
for(const p of ['known.bin','unknown.bin']){const b=fs.readFileSync(p),r=C.decode(b);assert(r.value instanceof Student);assert.equal(r.value.id,7);assert.equal(r.value.name,'张三🙂');assert.equal(r.value.score,95);assert.deepEqual(r.fqEncode(),b);assert.equal(r.fqKey().toString(),'student/7');}
console.log('PASS original JS entity');""",encoding='utf-8')
                run('node-original-entity',['node','probe.js'],project)
                shutil.copy2(ROOT/'integrations/node/fqdb-generate.cjs',project/'fqdb-generate.cjs')
                env['FQDB_CODEGEN']=str(cli)
                run('node-prebuild-helper',['node','fqdb-generate.cjs'],project)
                assert (project/'Generated/node/models/fq_codecs.js').is_file()
            elif lang=='java':
                (project/'Probe.java').write_text('''import example.*;import java.nio.file.*;import java.util.*;
class Probe{public static void main(String[]a)throws Exception{for(String p:new String[]{"known.bin","unknown.bin"}){byte[]b=Files.readAllBytes(Path.of(p));StudentFqCodec.Record r=StudentFqCodec.decode(b);if(r.value.getClass()!=Student.class||r.value.id!=7||r.value.getScore()!=95||!r.value.name.equals("张三🙂")||!Arrays.equals(r.fqEncode(),b))throw new AssertionError();}System.out.println("PASS original Java entity");}}''',encoding='utf-8')
                javac=args.java_bin/'javac' if args.java_bin else 'javac';java=args.java_bin/'java' if args.java_bin else 'java'
                run('java-compile',[javac,'-encoding','UTF-8','-d','classes',*sorted(project.rglob('*.java'))],project)
                run('java-original-entity',[java,'-cp','classes','Probe'],project)
            elif lang=='csharp':
                (project/'Probe.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework><FqdbEntitySources>$(MSBuildProjectDirectory)</FqdbEntitySources><FqdbCodegenExecutable>{cli}</FqdbCodegenExecutable><FqdbModelsOutput>$(MSBuildProjectDirectory)/Generated</FqdbModelsOutput></PropertyGroup><ItemGroup><ProjectReference Include="{ROOT.as_posix()}/bindings/csharp/FengquDB.csproj"/></ItemGroup><Import Project="{ROOT.as_posix()}/bindings/csharp/build/FengquDB.Models.targets"/></Project>''')
                (project/'Program.cs').write_text('''using System;using System.IO;using Example;
foreach(var p in new[]{"known.bin","unknown.bin"}){var b=File.ReadAllBytes(p);var r=StudentFqCodec.Decode(b);if(r.Value.GetType()!=typeof(Student)||r.Value.Id!=7||r.Value.Score!=95||r.Value.Name!="张三🙂"||!r.Encode().AsSpan().SequenceEqual(b))throw new Exception();}
var value=new Student{Id=7,Name="张三🙂",Score=95};var dst=new byte[64];for(int i=0;i<1000;i++)StudentFqCodec.EncodeInto(value,dst);long before=GC.GetAllocatedBytesForCurrentThread();for(int i=0;i<10000;i++)StudentFqCodec.EncodeInto(value,dst);if(GC.GetAllocatedBytesForCurrentThread()!=before)throw new Exception("codec allocated");Console.WriteLine("PASS original C# entity, zero-allocation caller buffer");''',encoding='utf-8')
                run('csharp-source-build',['dotnet','build','Probe.csproj','-c','Release','--nologo'],project)
                run('csharp-original-entity',['dotnet',project/'bin/Release/net10.0/Probe.dll'],project)
            elif lang in ('c','cpp'):
                if lang=='cpp':
                    header=project/'student.hpp';original=header.read_bytes()
                    header.write_bytes(original+b'\n/*')
                    run('cpp-unterminated-comment-rejected',command,project,1)
                    header.write_bytes(original)
                    probe='''#include "Generated/cpp/fq_codecs.hpp"
#include <fstream>
#include <iterator>
int main(){for(auto p:{"known.bin","unknown.bin"}){std::ifstream f(p,std::ios::binary);std::string b{std::istreambuf_iterator<char>(f),{}};using C=fqdb_generated::example::StudentFqCodec;auto r=C::decode(b);if(r.value.id!=7||r.value.score!=95||r.value.name!=u8"张三🙂"||C::encode(r)!=b||C::key(r.value)!="student/7")return 1;}}
'''
                else:probe='''#include "Generated/c/fq_codecs.h"
#include <stdio.h>
int main(void){unsigned char b[128],o[128];size_t n,w;FILE*f=fopen("unknown.bin","rb");models_StudentFqRecord r;if(!f)return 1;n=fread(b,1,sizeof(b),f);fclose(f);if(!models_StudentFqCodec_decode(b,n,&r)||r.value.id!=7||r.value.score!=95)return 2;if(!models_StudentFqCodec_encode(&r.value,r.unknown_source,o,sizeof(o),&w)||w!=n||memcmp(b,o,n))return 3;return 0;}
'''
                ext='cpp' if lang=='cpp' else 'c';(project/f'probe.{ext}').write_text(probe,encoding='utf-8')
                cm=f'''cmake_minimum_required(VERSION 3.24)
project(SourceProbe LANGUAGES C CXX)
include("{ROOT.as_posix()}/cmake/FengquDBModels.cmake")
add_executable(probe probe.{ext})
set_target_properties(probe PROPERTIES CXX_STANDARD 17)
target_include_directories(probe PRIVATE "{ROOT.as_posix()}/include")
'''
                if os.name=='nt':cm+='target_compile_options(probe PRIVATE /utf-8)\n'
                if lang=='cpp':cm+=f'fengqudb_generate_models(SOURCE "${{CMAKE_CURRENT_SOURCE_DIR}}" OUTPUT "${{CMAKE_CURRENT_SOURCE_DIR}}/Generated" COMPILER "{cli.as_posix()}" TARGET source_models)\nadd_dependencies(probe source_models)\n'
                (project/'CMakeLists.txt').write_text(cm)
                run(lang+'-configure',[args.cmake,'-S',project,'-B','build'],project)
                run(lang+'-compile',[args.cmake,'--build','build','--config','Release'],project)
                run(lang+'-original-entity',[project/('build/Release/probe.exe' if os.name=='nt' else 'build/probe')],project)
            elif lang=='go':
                (out/'go/models/probe_test.go').write_text('''package fqmodels
import("testing";"os";"bytes")
func TestOriginal(t *testing.T){for _,p:=range []string{"known.bin","unknown.bin"}{b,e:=os.ReadFile("../../../"+p);if e!=nil{t.Fatal(e)};r,e:=StudentFqDecode(b);if e!=nil||r.Value.Id!=7||r.Value.Score!=95||r.Value.Name!="张三🙂"{t.Fatal(e)};o,e:=r.FqEncode();if e!=nil||!bytes.Equal(o,b){t.Fatal(e)}}}
''',encoding='utf-8')
                run('go-original-entity',['go','test','./Generated/go/...'],project)
            elif lang=='rust':
                shutil.copy2(ROOT/'integrations/rust/build.rs',project/'build.rs')
                (project/'Cargo.toml').write_text('[package]\nname="source-model-probe"\nversion="0.1.0"\nedition="2021"\n')
                (project/'src/main.rs').write_text('''mod student;#[allow(dead_code)]mod codecs{include!(concat!(env!("FQDB_GENERATED_DIR"),"/rust/fq_codecs.rs"));}
fn main(){for p in ["known.bin","unknown.bin"]{let b=std::fs::read(p).unwrap();let r=codecs::student::StudentFqCodec::decode(&b).unwrap();assert_eq!(r.value.id,7);assert_eq!(r.value.name,"张三🙂");assert_eq!(r.value.score,95);assert_eq!(r.fq_encode().unwrap(),b);}}
''',encoding='utf-8')
                env['FQDB_CODEGEN']=str(cli)
                env['CARGO_TARGET_DIR']=str(project/'target')
                run('rust-source-build',['cargo','build','--release'],project)
                run('rust-original-entity',[project/('target/release/source-model-probe.exe' if os.name=='nt' else 'target/release/source-model-probe')],project)
        report['ok']=True
    finally:(work/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')


if __name__=='__main__':main()
