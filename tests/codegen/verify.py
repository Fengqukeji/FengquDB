"""Cross-language wire vectors, native CLI safety, and optional database round trips.

Run with --languages python,node,c,cpp,csharp,java,go,rust. Missing requested
compilers fail, rather than being counted as passing/skipped language tests.
The --prepare option writes fixtures/harnesses for an existing Linux toolchain.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]


def varint(v):
    b = bytearray()
    while v >= 128:
        b.append((v & 127) | 128)
        v >>= 7
    b.append(v)
    return bytes(b)


def vectors(work):
    def u(n, v): return varint(n * 8) + varint(v)
    def b(n, v): return varint(n * 8 + 2) + varint(len(v)) + v
    valid = (u(1, 0xffffffff) + b(2, '张三🙂'.encode()) + u(3, 190) + u(4, 0xffffffffffffffff)
             + u(5, 0xffffffff) + u(6, 0xffffffffffffffff) + u(7, 1)
             + varint(8*8+5) + struct.pack('<f', -1.5) + varint(9*8+1) + struct.pack('<d', 3.25)
             + b(10, b'\x00\xff') + u(536870911, 0xffffffff))
    unknown = u(20, 123) + b(21, b'future') + varint(22*8+3) + u(1, 9) + varint(22*8+4)
    (work/'valid.bin').write_bytes(valid)
    (work/'unknown.bin').write_bytes(valid+unknown)
    # Malformed varints, lengths, UTF-8, booleans, numeric overflow and groups.
    malformed = [b'\x80', b'\x00', b'\x08'+b'\xff'*10, b(2,b'\xff'), u(7,2),
                 u(1,0x100000000), b'\x12\x80\x01x', b'\xa3\x01', b'\xa3\x01\xac\x01',
                 u(5,0x100000000), varint(1 << 32), b'\x0c']
    for i, data in enumerate(malformed): (work/f'bad{i}.bin').write_bytes(data)
    (work/'text.bin').write_bytes(b(1,'键🙂'.encode())+u(2,0xffffffffffffffff))
    return len(malformed)


def harnesses(work, bad_count):
    c = r'''
#include "generated/c/fq_models.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned char data[1024],out[1024];
static size_t load(const char *p){FILE *f=fopen(p,"rb");size_t n;if(!f)exit(2);n=fread(data,1,sizeof(data),f);fclose(f);return n;}
int main(void){size_t i,n,w;game_models_Student v;game_models_TextStudent t;
const char *names[]={"valid.bin","unknown.bin"};
for(i=0;i<2;++i){n=load(names[i]);if(!game_models_Student_fq_decode(data,n,&v))return 3;
if(v.id!=INT32_MIN||v.score!=95||v.signed_value!=INT64_MIN||v.count!=UINT32_MAX||v.serial!=UINT64_MAX||!v.active||v.ratio!=-1.5f||v.average!=3.25||v.payload.size!=2||v.edge!=UINT32_MAX)return 4;
if(!game_models_Student_fq_encode(&v,out,n,&w)||w!=n||memcmp(data,out,n))return 5;
if(game_models_Student_fq_encode(&v,out,n-1,&w))return 6;
if(!game_models_Student_fq_key(&v,out,sizeof(out),&w)||w!=19||memcmp(out,"student/-2147483648",19))return 7;}
for(i=0;i<BAD_COUNT;++i){char p[32];snprintf(p,sizeof(p),"bad%u.bin",(unsigned)i);n=load(p);if(game_models_Student_fq_decode(data,n,&v))return 8;}
n=load("text.bin");if(!game_models_TextStudent_fq_decode(data,n,&t)||!game_models_TextStudent_fq_encode(&t,out,n,&w)||w!=n||memcmp(data,out,n))return 9;
{uint32_t seed=0x13579bdu;for(i=0;i<20000;++i){size_t j;seed=seed*1664525u+1013904223u;n=seed%256;for(j=0;j<n;++j){seed=seed*1664525u+1013904223u;data[j]=(uint8_t)(seed>>24);}v.id=77;if(!game_models_Student_fq_decode(data,n,&v)){if(v.id!=77)return 10;}else if(!game_models_Student_fq_encode(&v,out,sizeof(out),&w))return 11;}}
puts("PASS C wire, ranges, unknown fields, exact buffer, key");return 0;}
'''.replace('BAD_COUNT', str(bad_count))
    (work/'probe.c').write_text(c, encoding='utf-8')
    cpp = r'''
#include "generated/cpp/fq_models.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#ifdef FQ_TEST_DB
#include "fengqudb_store.hpp"
#endif
std::string load(const std::string&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing vector");return {std::istreambuf_iterator<char>(f),{}};}
int main(){using game::models::Student;for(auto p:{"valid.bin","unknown.bin"}){auto data=load(p);auto v=Student::fq_decode(data);
if(v.id!=INT32_MIN||v.name!=u8"张三🙂"||v.score!=95||v.signed_value!=INT64_MIN||v.serial!=UINT64_MAX||v.fq_encode()!=data||v.fq_key()!="student/-2147483648")return 2;
std::string buf(data.size(),'\0');if(v.fq_encode_into(buf.data(),buf.size())!=data.size()||buf!=data)return 3;}
for(int i=0;i<BAD_COUNT;++i){try{Student::fq_decode(load("bad"+std::to_string(i)+".bin"));return 4;}catch(const std::invalid_argument&){}}
#ifdef FQ_TEST_DB
{fengqudb::Store db("cpp-db");auto v=Student::fq_decode(load("valid.bin"));db.put(v.fq_key(),v.fq_encode());db.sync();auto got=db.get(v.fq_key());if(!got || Student::fq_decode(got->view()).score!=95)return 5;}
#endif
#ifdef FQ_TEST_DB
std::cout<<"PASS C++ wire and database\n";
#else
std::cout<<"PASS C++ wire\n";
#endif
}
'''.replace('BAD_COUNT', str(bad_count))
    (work/'probe.cpp').write_text(cpp, encoding='utf-8')
    (work/'probe.js').write_text(r'''
const fs=require('fs'),assert=require('assert'),{Student,TextStudent}=require('./generated/node/fq_models');
for(const p of ['valid.bin','unknown.bin']){const data=fs.readFileSync(p),v=Student.fqDecode(data);
assert.equal(v.id,-2147483648);assert.equal(v.name,'张三🙂');assert.equal(v.score,95);assert.equal(v.signed_value,-(1n<<63n));assert.equal(v.serial,(1n<<64n)-1n);assert.equal(v.ratio,-1.5);assert.equal(v.average,3.25);assert.equal(v.edge,0xffffffff);assert.deepEqual(Buffer.from(v.fqEncode()),data);
const out=new Uint8Array(data.length);assert.equal(v.fqEncodeInto(out),data.length);assert.deepEqual(Buffer.from(out),data);assert.throws(()=>v.fqEncodeInto(new Uint8Array(data.length-1)));assert.equal(v.fqKey().toString(),'student/-2147483648');}
for(let i=0;i<BAD_COUNT;++i)assert.throws(()=>Student.fqDecode(fs.readFileSync('bad'+i+'.bin')));
let v=TextStudent.fqDecode(fs.readFileSync('text.bin'));assert.equal(v.fqKey().toString(),'text/键🙂');assert.deepEqual(v.fqEncode(),fs.readFileSync('text.bin'));
v.id='\ufeffok';assert.equal(TextStudent.fqDecode(v.fqEncode()).id,'\ufeffok');v.id='\ud800';assert.throws(()=>v.fqEncode());
console.log('PASS Node/TypeScript wire, ranges, unknown fields and exact buffers');
'''.replace('BAD_COUNT',str(bad_count)),encoding='utf-8')
    (work/'Probe.java').write_text(r'''
import game.models.*;import java.nio.file.*;import java.util.*;
class Probe {static void check(boolean b){if(!b)throw new AssertionError();}
public static void main(String[] args)throws Exception {
for(String p:new String[]{"valid.bin","unknown.bin"}){byte[] data=Files.readAllBytes(Path.of(p));Student v=Student.fqDecode(data);
check(v.id==Integer.MIN_VALUE&&v.name.equals("张三🙂")&&v.score==95&&v.signed_value==Long.MIN_VALUE&&v.serial==-1L&&v.count==0xffffffffL&&v.ratio==-1.5f&&v.average==3.25&&v.edge==0xffffffffL);
check(Arrays.equals(v.fqEncode(),data));byte[] out=new byte[data.length];check(v.fqEncodeInto(out,0)==out.length&&Arrays.equals(out,data));check(new String(v.fqKey(),java.nio.charset.StandardCharsets.UTF_8).equals("student/-2147483648"));}
for(int i=0;i<BAD_COUNT;++i){try{Student.fqDecode(Files.readAllBytes(Path.of("bad"+i+".bin")));throw new AssertionError();}catch(IllegalArgumentException ok){}}
TextStudent v=TextStudent.fqDecode(Files.readAllBytes(Path.of("text.bin")));check(new String(v.fqKey(),java.nio.charset.StandardCharsets.UTF_8).equals("text/键🙂"));
System.out.println("PASS Java wire, ranges, unknown fields and buffers");}}
'''.replace('BAD_COUNT',str(bad_count)),encoding='utf-8')
    (work/'Program.cs').write_text(r'''
using System;using System.IO;using game.models;
class Program {static void Check(bool b){if(!b)throw new Exception("assertion");}
static void Main(){foreach(var path in new[]{"valid.bin","unknown.bin"}){var data=File.ReadAllBytes(path);var v=Student.Decode(data);
Check(v.id==int.MinValue&&v.name=="张三🙂"&&v.score==95&&v.signed_value==long.MinValue&&v.count==uint.MaxValue&&v.serial==ulong.MaxValue&&v.edge==uint.MaxValue&&v.ratio==-1.5f&&v.average==3.25);
Check(v.FqEncode().AsSpan().SequenceEqual(data));var b=new byte[data.Length];Check(v.EncodeTo(b)==b.Length&&b.AsSpan().SequenceEqual(data));}
for(int i=0;i<BAD_COUNT;++i){bool failed=false;try{Student.Decode(File.ReadAllBytes("bad"+i+".bin"));}catch(InvalidDataException){failed=true;}catch(OverflowException){failed=true;}catch(System.Text.DecoderFallbackException){failed=true;}Check(failed);}
var student=Student.Decode(File.ReadAllBytes("valid.bin"));var buffer=new byte[student.EncodedSize()];for(int j=0;j<1000;j++)student.EncodeTo(buffer);long before=GC.GetAllocatedBytesForCurrentThread();long begin=System.Diagnostics.Stopwatch.GetTimestamp();for(int j=0;j<200000;j++)student.EncodeTo(buffer);long end=System.Diagnostics.Stopwatch.GetTimestamp();long allocated=GC.GetAllocatedBytesForCurrentThread()-before;Check(allocated==0);Console.WriteLine("codec_ns="+((end-begin)*1e9/System.Diagnostics.Stopwatch.Frequency/200000).ToString("F1")+" allocated_bytes="+allocated);
using(var db=new FengquDB.Store("csharp-db")){db.Put(student,forceReliable:true);Check(db.GetEntity<Student>((long)student.id)!.score==95);db.Put("score",95);Check(db.Get<int>("score")==95);db.Put("name","张三");Check(db.Get<string>("name")=="张三");}
Console.WriteLine("PASS C# wire, typed entity and scalar database writes");}}
'''.replace('BAD_COUNT',str(bad_count)),encoding='utf-8')
    (work/'generated/go/fq_models_test.go').write_text(r'''
package models
import("testing";"os";"bytes";"fmt")
func TestModels(t *testing.T){for _,p:=range []string{"valid.bin","unknown.bin"}{data,e:=os.ReadFile("../../"+p);if e!=nil{t.Fatal(e)};v,e:=StudentFqDecode(data);if e!=nil{t.Fatal(e)};if v.Id!=-2147483648||v.Name!="张三🙂"||v.Score!=95||v.Signed_value!=-9223372036854775808||v.Count!=0xffffffff||v.Serial!=0xffffffffffffffff||v.Ratio!=-1.5||v.Average!=3.25||v.Edge!=0xffffffff{t.Fatal(v)};out,e:=v.FqEncodeInto(make([]byte,0,len(data)));if e!=nil||!bytes.Equal(data,out){t.Fatal(e)};k,e:=v.FqKey();if e!=nil||string(k)!="student/-2147483648"{t.Fatal(e)}}
for i:=0;i<BAD_COUNT;i++{b,e:=os.ReadFile(fmt.Sprintf("../../bad%d.bin",i));if e!=nil{t.Fatal(e)};if _,e=StudentFqDecode(b);e==nil{t.Fatal("accepted malformed",i)}}
b,e:=os.ReadFile("../../text.bin");if e!=nil{t.Fatal(e)};v,e:=TextStudentFqDecode(b);if e!=nil{t.Fatal(e)};k,e:=v.FqKey();if e!=nil||string(k)!="text/键🙂"{t.Fatal(e)}}
'''.replace('BAD_COUNT',str(bad_count)),encoding='utf-8')
    (work/'generated/go/go.mod').write_text('module generated-model-probe\n\ngo 1.21\n')
    (work/'probe.rs').write_text(r'''
#[path="generated/rust/fq_models.rs"]mod models;
use models::*;
fn main()->Result<(),Box<dyn std::error::Error>>{for p in ["valid.bin","unknown.bin"]{let data=std::fs::read(p)?;let v=Student::fq_decode(&data)?;assert_eq!(v.id,i32::MIN);assert_eq!(v.name,"张三🙂");assert_eq!(v.score,95);assert_eq!(v.signed_value,i64::MIN);assert_eq!(v.serial,u64::MAX);assert_eq!(v.edge,u32::MAX);assert_eq!(v.ratio,-1.5);assert_eq!(v.average,3.25);assert_eq!(v.fq_encode()?,data);assert_eq!(v.fq_key()?,b"student/-2147483648");}
for i in 0..BAD_COUNT{assert!(Student::fq_decode(&std::fs::read(format!("bad{}.bin",i))?).is_err());}
let v=TextStudent::fq_decode(&std::fs::read("text.bin")?)?;assert_eq!(v.fq_key()?,"text/键🙂".as_bytes());println!("PASS Rust wire, ranges, unknown fields and keys");Ok(())}
'''.replace('BAD_COUNT',str(bad_count)),encoding='utf-8')


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--cli',type=Path)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--languages',default='python,node')
    p.add_argument('--cmake',default='cmake')
    p.add_argument('--java-bin',type=Path)
    p.add_argument('--library',type=Path)
    p.add_argument('--prepare',action='store_true')
    p.add_argument('--reuse',action='store_true')
    args=p.parse_args()
    valid={'python','node','c','cpp','csharp','java','go','rust'}
    if not args.prepare and (not args.languages or not set(args.languages.split(','))<=valid):
        p.error('Select at least one supported language, or use --prepare for fixture preparation only')
    work=args.output.resolve();work.mkdir(parents=True,exist_ok=args.reuse)
    report={'ok':False,'checks':[]}
    env=dict(os.environ,DOTNET_CLI_UI_LANGUAGE='en')
    if args.library:
        env['PATH']=str(args.library.resolve().parent)+os.pathsep+env.get('PATH','')
        env['LD_LIBRARY_PATH']=str(args.library.resolve().parent)+os.pathsep+env.get('LD_LIBRARY_PATH','')
        env['FENGQUDB_LIBRARY']=str(args.library.resolve())
    def run(label,command,expected=0,cwd=work):
        r=subprocess.run([str(x) for x in command],cwd=cwd,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
        (work/(label+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        row={'name':label,'exit_code':r.returncode};report['checks'].append(row);print(json.dumps(row),flush=True)
        if r.returncode!=expected:raise RuntimeError(f'{label}: expected {expected}, got {r.returncode}\n{r.stdout}\n{r.stderr}')
        return r.stdout
    try:
        if args.cli:
            cli=args.cli.resolve();base=[cli,'generate','--schema',ROOT/'tests/codegen/models.json','--output',work/'generated']
            run('generate',base)
            times={x:x.stat().st_mtime_ns for x in (work/'generated').rglob('*') if x.is_file()}
            run('check-current',base+['--check']);run('generate-unchanged',base)
            assert all(x.stat().st_mtime_ns==t for x,t in times.items())
            run('check-missing',[cli,'generate','--schema',ROOT/'tests/codegen/models.json','--output',work/'missing','--check'],2)
            invalid=json.loads((ROOT/'tests/codegen/models.json').read_text());invalid['entities'][0]['fields'][1]['number']=1
            (work/'invalid.json').write_text(json.dumps(invalid));run('invalid-schema',[cli,'generate','--schema',work/'invalid.json','--output',work/'generated'],1)
            invalid['entities'][0]['fields'][1]['number']=2;invalid['entities'][0]['fields'][1]['type']='List<int>'
            (work/'invalid.json').write_text(json.dumps(invalid));run('unsupported-type',[cli,'generate','--schema',work/'invalid.json','--output',work/'generated'],1)
            protected=work/'protected/node';protected.mkdir(parents=True,exist_ok=True);(protected/'fq_models.js').write_text('// user file\n')
            run('protect-user-file',[cli,'generate','--schema',ROOT/'tests/codegen/models.json','--output',protected.parent],1)
            assert (protected/'fq_models.js').read_text()=='// user file\n'
            # A namespace change must remove stale Java classes, without touching
            # any unselected language or unrelated file.
            migration=work/'migration';schema=work/'migration.json'
            model=json.loads((ROOT/'examples/models/student.fqmodel.json').read_text())
            schema.write_text(json.dumps(model));cmd=[cli,'generate','--schema',schema,'--output',migration,'--lang','java']
            run('manifest-initial',cmd);(migration/'note.txt').write_text('keep')
            model['namespace']='game.changed';schema.write_text(json.dumps(model))
            run('manifest-stale',cmd+['--check'],2);run('manifest-update',cmd)
            assert not (migration/'java/game/models/Student.java').exists()
            assert (migration/'java/game/changed/Student.java').is_file() and (migration/'note.txt').read_text()=='keep'
            target=migration/'java/game/changed/Student.java';target.write_bytes(target.read_bytes().replace(b'\n',b'\r\n'))
            run('crlf-owned-output',cmd)
            schema.write_text('{"version":1,"version":1}')
            run('duplicate-json-property',cmd,1)
        count=vectors(work);harnesses(work,count)
        if args.prepare:
            report['ok']=True;return
        languages=set(args.languages.split(','))
        if 'python' in languages:
            spec=importlib.util.spec_from_file_location('fq_models',work/'generated/python/fq_models.py');mod=importlib.util.module_from_spec(spec);sys.modules[spec.name]=mod;spec.loader.exec_module(mod)
            for path in ['valid.bin','unknown.bin']:
                data=(work/path).read_bytes();v=mod.Student.fq_decode(data)
                assert v.id==-(1<<31) and v.name=='张三🙂' and v.score==95 and v.signed_value==-(1<<63) and v.count==0xffffffff and v.serial==0xffffffffffffffff and v.ratio==-1.5 and v.average==3.25 and v.edge==0xffffffff
                assert v.fq_encode()==data and v.fq_key()==b'student/-2147483648'
            for i in range(count):
                try:mod.Student.fq_decode((work/f'bad{i}.bin').read_bytes())
                except (ValueError,UnicodeError):pass
                else:raise AssertionError(f'Accepted bad{i}')
            v=mod.TextStudent.fq_decode((work/'text.bin').read_bytes());assert v.fq_key()=='text/键🙂'.encode()
            if args.library:
                sys.path.insert(0,str(ROOT/'bindings/python'));from fengqudb import Store
                with Store(work/'python-db',library=args.library) as db:
                    v=mod.Student.fq_decode((work/'valid.bin').read_bytes());db.put(v.fq_key(),v.fq_encode(),force_reliable=True)
                    assert mod.Student.fq_decode(db.get(v.fq_key())).score==95
            report['checks'].append({'name':'python-wire-and-database','exit_code':0})
        if 'node' in languages:run('node',['node','probe.js'])
        if languages & {'c','cpp'}:
            cm='cmake_minimum_required(VERSION 3.24)\nproject(ModelProbe LANGUAGES C CXX)\n'
            for lang,ext in [('c','c'),('cpp','cpp')]:
                if lang in languages:
                    cm+=f'add_executable(probe_{lang} probe.{ext})\nset_target_properties(probe_{lang} PROPERTIES CXX_STANDARD 17)\n'
                    if os.name=='nt':cm+=f'target_compile_options(probe_{lang} PRIVATE /utf-8)\n'
                    if lang=='cpp' and args.library:
                        cm+=f'target_compile_definitions(probe_cpp PRIVATE FQ_TEST_DB)\ntarget_include_directories(probe_cpp PRIVATE "{ROOT.as_posix()}/include")\n'
                        lib=args.library.resolve().with_suffix('.lib') if os.name=='nt' else args.library.resolve()
                        cm+=f'target_link_libraries(probe_cpp PRIVATE "{lib.as_posix()}")\n'
            (work/'CMakeLists.txt').write_text(cm)
            run('native-configure',[args.cmake,'-S',work,'-B',work/'build'])
            run('native-build',[args.cmake,'--build',work/'build','--config','Release','--parallel','2'])
            for lang in ['c','cpp']:
                if lang in languages:run(lang,[work/('build/Release' if os.name=='nt' else 'build')/('probe_'+lang+('.exe' if os.name=='nt' else ''))])
        if 'csharp' in languages:
            (work/'Probe.csproj').write_text(f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><OutputType>Exe</OutputType><Nullable>enable</Nullable><EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup><ItemGroup><Compile Include="Program.cs"/><Compile Include="generated/csharp/*.cs"/><ProjectReference Include="{ROOT.as_posix()}/bindings/csharp/FengquDB.csproj"/></ItemGroup></Project>')
            run('csharp-build',['dotnet','build','Probe.csproj','-c','Release','--nologo'])
            run('csharp',['dotnet',work/'bin/Release/net10.0/Probe.dll'])
            if args.cli:
                hook=work/'hook';hook.mkdir(exist_ok=True)
                schema=hook/'student.json';model=json.loads((ROOT/'examples/models/student.fqmodel.json').read_text());schema.write_text(json.dumps(model))
                project=f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><OutputType>Exe</OutputType>
<FqdbCodegenExecutable>{escape(str(cli))}</FqdbCodegenExecutable><FqdbModelSchema>{escape(str(schema))}</FqdbModelSchema></PropertyGroup>
<ItemGroup><ProjectReference Include="{ROOT.as_posix()}/bindings/csharp/FengquDB.csproj"/></ItemGroup>
<Import Project="{ROOT.as_posix()}/bindings/csharp/build/FengquDB.Models.targets"/></Project>'''
                (hook/'Hook.csproj').write_text(project)
                (hook/'Program.cs').write_text('var v=new game.models.Student {id=7};System.Console.WriteLine(v.id);')
                run('msbuild-model-initial',['dotnet','build','Hook.csproj','-c','Release','--nologo'],cwd=hook)
                model['entities'][0]['fields'].append({'name':'bonus','type':'uint64','number':4});schema.write_text(json.dumps(model))
                (hook/'Program.cs').write_text('var v=new game.models.Student {id=7,bonus=99};System.Console.WriteLine(game.models.Student.Decode(v.FqEncode()).bonus);')
                run('msbuild-model-change',['dotnet','build','Hook.csproj','-c','Release','--nologo'],cwd=hook)
                assert run('msbuild-model-run',['dotnet',hook/'bin/Release/net10.0/Hook.dll'],cwd=hook).strip()=='99'
        if 'java' in languages:
            javac=args.java_bin/'javac' if args.java_bin else 'javac';java=args.java_bin/'java' if args.java_bin else 'java'
            run('java-build',[javac,'-encoding','UTF-8','-d','classes',*sorted((work/'generated/java').rglob('*.java')),work/'Probe.java'])
            run('java',[java,'-cp','classes','Probe'])
        if 'go' in languages:run('go',['go','test','-count=1','./...'],cwd=work/'generated/go')
        if 'rust' in languages:
            run('rust-build',['rustc','--edition=2021','-O','probe.rs','-o','probe-rust'])
            run('rust',[work/('probe-rust.exe' if os.name=='nt' else 'probe-rust')])
        report['ok']=True
    finally:
        (work/('fixtures-report.json' if args.prepare else 'report.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')


if __name__=='__main__':main()
