#!/usr/bin/env python3
"""Exercise the shipped CLI and ordinary .NET rebuilds without native database I/O."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import xml.sax.saxutils


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    cli = args.cli.resolve()
    report = []
    env = dict(os.environ, DOTNET_CLI_UI_LANGUAGE='en')

    def run(label, command, expected=0):
        result = subprocess.run([str(part) for part in command], cwd=root, env=env,
                                capture_output=True, text=True, encoding='utf-8', errors='replace')
        (work / (label + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        report.append({'name': label, 'exit_code': result.returncode})
        if result.returncode != expected:
            raise RuntimeError(f'{label} returned {result.returncode}, expected {expected}\n{result.stdout}\n{result.stderr}')
        return result.stdout

    def generate(label, *options, expected=0):
        text = run(label, ['dotnet', cli, 'generate', *options], expected)
        return json.loads(text)

    models = work / '模型 Models'
    models.mkdir()
    entity = models / 'Student.cs'
    partial = models / 'Fields.cs'
    entity.write_text('''using FengquDB.Entities;
[FqEntity("student/")] public partial class Student {
    [FqKey,FqField(1)] public int Id;
    [FqField(2)] public string Name = "";
}
''', encoding='utf-8')
    partial.write_text('public partial class Student { [FengquDB.Entities.FqField(3)] public int Score; }', encoding='utf-8')
    runtime = xml.sax.saxutils.escape(str(root / 'bindings/csharp/FengquDB.csproj'))
    generator = xml.sax.saxutils.escape(str(root / 'bindings/csharp/Generator/FengquDB.Generator.csproj'))
    project = work / 'App.csproj'
    auto_reference = f'<ProjectReference Include="{generator}" OutputItemType="Analyzer" ReferenceOutputAssembly="false" />'
    project_text = f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable>
<EnableDefaultCompileItems>false</EnableDefaultCompileItems>
</PropertyGroup><ItemGroup>
<Compile Include="模型 Models/*.cs" /><Compile Include="Program.cs" /><Compile Include="Generated/*.cs" />
<ProjectReference Include="{runtime}" />{auto_reference}
</ItemGroup></Project>'''
    project.write_text(project_text, encoding='utf-8')
    (work / 'Program.cs').write_text('''using System;
var student = new Student { Id = 1, Name = "张三" };
byte[] bytes = new byte[student.EncodedSize()]; student.EncodeTo(bytes);
if (Student.Decode(bytes).Name != student.Name) throw new Exception("Roundtrip failed");
Console.WriteLine(typeof(Student).GetMethod("ReadBonus") == null ? "NO_BONUS" : "HAS_BONUS");
''', encoding='utf-8')
    build = ['dotnet', 'build', project, '-c', 'Release', '--nologo', '-p:NuGetAudit=false']
    app = work / 'bin/Release/net10.0/App.dll'
    run('auto-initial-build', build)
    if 'NO_BONUS' not in run('auto-initial-run', ['dotnet', app]):
        raise RuntimeError('Initial automatic output is incorrect')
    partial.write_text('public partial class Student { [FengquDB.Entities.FqField(4)] public long Bonus; }', encoding='utf-8')
    run('auto-incremental-build', build + ['--no-restore'])
    if 'HAS_BONUS' not in run('auto-incremental-run', ['dotnet', app]):
        raise RuntimeError('Ordinary dotnet build did not regenerate the changed partial class')
    target = work / 'Generated'
    result = generate('source-generate', '--source', models, '--output', target)
    if result['entities'] != 1 or not result['changed']:
        raise RuntimeError('CLI did not emit the entity')
    generated = target / 'FengquDB.Entities.g.cs'
    before = generated.read_bytes()
    timestamp = generated.stat().st_mtime_ns
    repeat = generate('source-repeat', '--source', models, '--output', target)
    if repeat['changed'] or generated.stat().st_mtime_ns != timestamp:
        raise RuntimeError('Identical output was rewritten')
    generate('source-check', '--source', models, '--output', target, '--check')
    generated.write_bytes(before.replace(b'\n', b'\r\n'))
    generate('windows-newline-regenerate', '--source', models, '--output', target)
    if generated.read_bytes() != before:
        raise RuntimeError('Windows line endings prevented deterministic regeneration')
    run('auto-with-disk-fallback-build', build + ['--no-restore'])
    manual_text = project_text.replace('<Nullable>enable</Nullable>',
        '<Nullable>enable</Nullable><DefineConstants>FQDB_MANUAL_ENTITIES</DefineConstants>')
    project.write_text(manual_text, encoding='utf-8')
    run('manual-with-analyzer-build', build + ['--no-restore'])
    partial.write_text('public partial class Student { [FengquDB.Entities.FqField(5)] public long Bonus; }', encoding='utf-8')
    stale = run('manual-stale-build', build + ['--no-restore'], expected=1)
    if 'FQDB002' not in stale:
        raise RuntimeError('Stale manual output was not diagnosed')
    generate('source-stale-check', '--source', models, '--output', target, '--check', expected=2)
    generate('project-force-generate', '--project', project, '--configuration', 'Release', '--output', target)
    if generated.read_bytes() == before:
        raise RuntimeError('Project mode reused stale generator output')
    run('manual-regenerated-build', build + ['--no-restore'])
    run('manual-regenerated-run', ['dotnet', app])
    project.write_text(manual_text.replace(auto_reference, ''), encoding='utf-8')
    run('manual-no-analyzer-build', build)
    run('manual-no-analyzer-run', ['dotnet', app])
    snapshot = generated.read_bytes()
    partial.write_text('public partial class Student { [FengquDB.Entities.FqField(1)] public long Bonus; }', encoding='utf-8')
    generate('invalid-preserves-output', '--source', models, '--output', target, expected=1)
    if generated.read_bytes() != snapshot:
        raise RuntimeError('Invalid input replaced a valid generated file')
    partial.write_text('public partial class Student { [FengquDB.Entities.FqField(5)] public long Bonus; }', encoding='utf-8')
    foreign = work / 'Foreign'
    foreign.mkdir()
    (foreign / generated.name).write_text('// User-owned source\n', encoding='utf-8')
    generate('foreign-output-protected', '--source', models, '--output', foreign, expected=1)
    empty = work / 'Empty'
    empty.mkdir()
    generate('removed-last-entity', '--source', empty, '--output', target)
    if 'partial class' in generated.read_text(encoding='utf-8'):
        raise RuntimeError('Deleted entity remained in the generated file')
    (work / 'report.json').write_text(json.dumps({'ok': True, 'checks': report}, indent=2), encoding='utf-8')
    print('PASS CLI source/project force generation, ordinary incremental builds, manual fallback, stale checks and atomic output protection')


if __name__ == '__main__':
    main()
