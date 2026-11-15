"""Independent language SDKs: one copyable FengquDB folder in each ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import zipfile
from unity_assets import write_unity_assets

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'project\(FengquDB VERSION ([0-9]+\.[0-9]+\.[0-9]+)',
                    (ROOT/'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
LANGUAGES = ('c','cpp','csharp','unity','java','python','go','rust','node')
TARGETS = ('win-x64','linux-x64','android-arm64','ios-arm64','macos-arm64','macos-x64')
SKIP = {'bin','obj','build','target','node_modules','dist','__pycache__','.git','.agents','test','tests','bench','cmd','examples'}

def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)

def validate_host(path, platform):
    data = path.read_bytes()
    if platform == 'win-x64':
        if len(data)<64 or data[:2]!=b'MZ': raise ValueError('Expected Windows binary: '+str(path))
        offset=struct.unpack_from('<I',data,0x3c)[0]
        if offset+24>len(data) or data[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',data,offset+4)[0]!=0x8664: raise ValueError('Expected x64 PE binary')
    elif platform == 'linux-x64':
        if len(data)<20 or data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=62: raise ValueError('Expected x64 ELF binary')
    else:
        cpu=0x0100000c if platform.endswith('arm64') else 0x01000007
        if len(data)<8 or data[:4]!=bytes.fromhex('cffaedfe') or struct.unpack_from('<I',data,4)[0]!=cpu: raise ValueError('Expected matching Mach-O binary')

def write_cmake(sdk, platform, runtime):
    config='''get_filename_component(_fqdb_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET FengquDB::Core)
  add_library(FengquDB::Core SHARED IMPORTED)
  set_target_properties(FengquDB::Core PROPERTIES IMPORTED_LOCATION "${_fqdb_root}/Native/'''+runtime+'''" INTERFACE_INCLUDE_DIRECTORIES "${_fqdb_root}/include")
'''
    if platform.startswith('win'): config+='  set_target_properties(FengquDB::Core PROPERTIES IMPORTED_IMPLIB "${_fqdb_root}/Native/fengqudb.lib")\n'
    if platform.startswith('linux'): config+='  set_target_properties(FengquDB::Core PROPERTIES IMPORTED_SONAME "libfengqudb.so.2")\n'
    config+='''  add_library(FengquDB::Cpp INTERFACE IMPORTED)
  set_target_properties(FengquDB::Cpp PROPERTIES INTERFACE_LINK_LIBRARIES FengquDB::Core INTERFACE_COMPILE_FEATURES cxx_std_17)
endif()
set(FengquDB_CODEGEN_EXECUTABLE "${_fqdb_root}/Tools/fqdb-codegen")
if(CMAKE_HOST_WIN32)
  string(APPEND FengquDB_CODEGEN_EXECUTABLE ".exe")
endif()
include("${_fqdb_root}/cmake/FengquDBModels.cmake")
set(FengquDB_SHARED TRUE)
include("${_fqdb_root}/cmake/FengquDBRuntime.cmake")
'''
    target=sdk/'lib/cmake/FengquDB/FengquDBConfig.cmake';target.parent.mkdir(parents=True,exist_ok=True);target.write_text(config,encoding='utf-8')
    copy(ROOT/'cmake/FengquDBModels.cmake',sdk/'cmake/FengquDBModels.cmake')
    copy(ROOT/'cmake/FengquDBRuntime.cmake',sdk/'cmake/FengquDBRuntime.cmake')

def finish(sdk, archive, metadata):
    metadata['files']=[]
    for path in sorted(sdk.rglob('*')):
        if path.is_file() and path != sdk/'manifest.json':
            data=path.read_bytes();metadata['files'].append({'path':path.relative_to(sdk).as_posix(),'size':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    (sdk/'manifest.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    for page in sdk.glob('*.md'):
        for target in re.findall(r'\[[^\]]+\]\(([^)]+)\)',page.read_text(encoding='utf-8')):
            if '://' not in target and not target.startswith('#') and not (page.parent/target.split('#')[0]).is_file(): raise ValueError('Broken documentation link: '+target)
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=9) as package:
        for path in sorted(sdk.rglob('*')):
            if not path.is_file(): continue
            info=zipfile.ZipInfo.from_file(path,arcname='FengquDB/'+path.relative_to(sdk).as_posix());info.create_system=3
            info.external_attr=(0o100755 if path.name=='fqdb-codegen' or path.suffix in ('.sh','.command') else 0o100644)<<16
            package.writestr(info,path.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
    with zipfile.ZipFile(archive) as package:
        if package.testzip(): raise ValueError('Archive CRC failure')

def build(args, language, output):
    libraries={args.platform:args.library}
    for target in TARGETS:
        source=getattr(args,'unity_'+target.replace('-','_'))
        if source:libraries[target]=source.resolve(strict=True)
    missing=sorted({'android-arm64','ios-arm64'}-set(libraries)) if language=='unity' else []
    if missing and not args.unity_preview: raise ValueError('Unity release requires native libraries for '+', '.join(missing))
    name='FengquDB-'+language+'-'+args.platform+('-preview' if missing else '')
    archive=output/(name+'.zip');sdk=output/name/'FengquDB'
    if archive.exists() or sdk.parent.exists():raise ValueError('Output already exists: '+str(archive))
    sdk.mkdir(parents=True)
    metadata={'format':'fqdb-language-sdk-1','version':VERSION,'abi':2,'language':language,'host':args.platform,'preview':bool(missing),'missing_native_targets':missing}
    (sdk/'.fqdb-sdk').write_text(json.dumps(metadata),encoding='utf-8')
    tool_dir='Editor/Tools' if language=='unity' else 'Tools'
    copy(args.codegen,sdk/tool_dir/('fqdb-codegen.exe' if args.platform.startswith('win') else 'fqdb-codegen'))
    for script in ('fqdb-generate.bat','fqdb-generate.command'):copy(ROOT/'scripts'/script,sdk/script)
    runtime='fengqudb.dll' if args.platform.startswith('win') else 'libfengqudb.so' if args.platform.startswith('linux') else 'libfengqudb.dylib'
    if language=='unity':metadata['native_targets']=write_unity_assets(sdk,libraries,args.managed,args.analyzer,args.managed_ios)
    else:
        copy(args.library,sdk/'Native'/runtime)
        if args.platform.startswith('linux'):copy(args.library,sdk/'Native/libfengqudb.so.2')
        elif args.platform.startswith('macos'):copy(args.library,sdk/'Native/libfengqudb.2.dylib')
        metadata['native_library']='Native/'+runtime
    if language in ('c','cpp','go'):
        for path in (ROOT/'include').iterdir():
            if path.suffix in ('.h','.hpp') and not path.name.startswith('fengqudb_diagnostics'):copy(path,sdk/'include'/path.name)
    if language in ('c','cpp','go','rust') and args.platform.startswith('win'):copy(args.library.with_suffix('.lib'),sdk/'Native/fengqudb.lib')
    if language in ('c','cpp'):write_cmake(sdk,args.platform,runtime)
    elif language=='csharp':
        copy(args.managed,sdk/'FengquDB.Managed.dll');copy(args.analyzer,sdk/'FengquDB.Generator.dll')
        copy(ROOT/'integrations/csharp/FengquDB.targets',sdk/'FengquDB.targets')
    elif language=='java':
        copy(args.clients/'java/fengqudb.jar',sdk/'fengqudb.jar');copy(ROOT/'integrations/gradle/fengqudb-client.gradle',sdk/'fengqudb.gradle')
        jni='fengqudb_jni.dll' if args.platform.startswith('win') else 'libfengqudb_jni.so' if args.platform.startswith('linux') else 'libfengqudb_jni.dylib'
        copy(args.jni,sdk/'Native'/jni)
    elif language=='python':
        copy(args.clients/'python/fengqudb.zip',sdk/'fengqudb.zip')
        copy(ROOT/'integrations/python/__init__.py',sdk/'__init__.py')
    elif language=='node':
        shutil.copytree(args.clients/'node/dist/src',sdk/'dist/src');copy(args.node_addon,sdk/'Native/fengqudb_native.node')
        copy(ROOT/'integrations/node/fqdb-generate.cjs',sdk/'fqdb-generate.cjs')
        (sdk/'package.json').write_text(json.dumps({'name':'fengqudb','version':VERSION,'license':'Apache-2.0','main':'dist/src/index.js','types':'dist/src/index.d.ts'}),encoding='utf-8')
    elif language=='go':
        for path in (ROOT/'bindings/go').iterdir():
            if path.is_file() and path.suffix in ('.go','.h','.mod') and not path.name.endswith('_test.go'):copy(path,sdk/path.name)
        for filename in ('build.bat','build.sh','run.bat','run.sh'):copy(ROOT/'integrations/go'/filename,sdk/filename)
        copy(ROOT/'integrations/go/sdktool.go',sdk/'Tools/sdktool.go')
    elif language=='rust':
        for path in (ROOT/'bindings/rust').rglob('*'):
            rel=path.relative_to(ROOT/'bindings/rust')
            if not path.is_file() or any(part in SKIP for part in rel.parts) or path.suffix not in ('.rs','.toml'):continue
            if rel.as_posix()=='fengqudb/Cargo.toml':
                text=re.sub(r'(?ms)^\[dev-dependencies\].*?(?=^\[|\Z)','',path.read_text(encoding='utf-8'))
                text=re.sub(r'(?ms)^\[\[example\]\].*?(?=^\[|\Z)','',text)
                dest=sdk/'Crates'/rel;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(text,encoding='utf-8')
            else:copy(path,sdk/'Crates'/rel)
        copy(ROOT/'integrations/rust/build.rs',sdk/'generate.rs')
    for source_dir,filename in (('languages','接入.md'),('api','API.md')):
        text=(ROOT/'docs'/source_dir/(language+'.md')).read_text(encoding='utf-8')
        text=text.replace('../api/'+language+'.md','API.md').replace('../languages/'+language+'.md','接入.md')
        if missing and filename=='接入.md':text='开发预览：本包尚缺 iOS 原生库，暂不能发布 iOS。\n\n'+text
        (sdk/filename).write_text(text,encoding='utf-8')
    copy(ROOT/'third_party/json/LICENSE.MIT',sdk/'licenses/nlohmann-json.LICENSE.MIT')
    for name in ('LICENSE','NOTICE'):
        copy(ROOT/name,sdk/name)
    notices=(ROOT/'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
    notices=notices.replace('(LICENSE)','(../LICENSE)').replace('(NOTICE)','(../NOTICE)').replace('(licenses/','(')
    (sdk/'licenses/THIRD_PARTY_NOTICES.md').write_text(notices,encoding='utf-8')
    for path in (ROOT/'licenses').iterdir():
        if path.is_file():copy(path,sdk/'licenses'/path.name)
    finish(sdk,archive,metadata)
    return {'language':language,'folder':str(sdk),'archive':str(archive),'bytes':archive.stat().st_size,'preview':bool(missing)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--languages',default=','.join(LANGUAGES))
    p.add_argument('--platform',required=True,choices=('win-x64','linux-x64','macos-arm64','macos-x64'))
    for name in ('library','codegen'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--clients',type=Path)
    for name in ('jni','node-addon','managed-ios'):p.add_argument('--'+name,type=Path)
    p.add_argument('--managed',type=Path,default=ROOT/'bindings/csharp/bin/Release/netstandard2.1/FengquDB.Managed.dll')
    p.add_argument('--analyzer',type=Path,default=ROOT/'bindings/csharp/Generator/bin/Release/netstandard2.0/FengquDB.Generator.dll')
    for target in TARGETS:p.add_argument('--unity-'+target,type=Path)
    p.add_argument('--unity-preview',action='store_true',help='Allow an explicitly named development preview without all mobile binaries')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();selected=args.languages.split(',')
    if not selected or len(set(selected))!=len(selected) or not set(selected)<=set(LANGUAGES):p.error('Select distinct supported languages')
    for name in ('library','codegen'):setattr(args,name,getattr(args,name).resolve(strict=True))
    if set(selected)&{'csharp','unity'}:
        for name in ('managed','analyzer'):setattr(args,name,getattr(args,name).resolve(strict=True))
    if set(selected)&{'java','python','node'}:
        if not args.clients:p.error('--clients is required for Java, Python or Node')
        args.clients=args.clients.resolve(strict=True)
    for name in ('jni','node_addon','managed_ios'):
        if getattr(args,name):setattr(args,name,getattr(args,name).resolve(strict=True))
    output=args.output.resolve()
    if not output.is_relative_to((ROOT/'artifacts').resolve()) or output==(ROOT/'artifacts').resolve():p.error('Output must be a child of workspace artifacts')
    validate_host(args.library,args.platform);validate_host(args.codegen,args.platform)
    if 'java' in selected:
        if not args.jni:p.error('--jni is required for Java')
        validate_host(args.jni,args.platform)
    if 'node' in selected:
        if not args.node_addon:p.error('--node-addon is required for Node')
        validate_host(args.node_addon,args.platform)
    output.mkdir(parents=True,exist_ok=True)
    print(json.dumps([build(args,language,output) for language in selected],ensure_ascii=False))

if __name__=='__main__':main()
