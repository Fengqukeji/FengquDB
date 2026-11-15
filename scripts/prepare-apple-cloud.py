"""Create a bounded, hashed Apple build snapshot for a private GitHub runner."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SKIP = {'bin', 'obj', 'build', '__pycache__', '.git', '.agents'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--windows-library', type=Path,
                        help='Use a verified Windows core build instead of the core worktree default')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    core = args.core.resolve(strict=True)
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'artifacts') or output.exists():
        parser.error('Use a new directory under artifacts')
    output.mkdir(parents=True)
    stage = output / 'snapshot'
    source = stage / 'source'
    source.mkdir(parents=True)

    def copy_tree(base, relative):
        origin = base / relative
        if not origin.exists():
            raise FileNotFoundError(origin)
        for item in ([origin] if origin.is_file() else sorted(origin.rglob('*'))):
            rel = item.relative_to(base)
            if not item.is_file() or set(rel.parts) & SKIP:
                continue
            target = source / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target)

    for relative in ('src', 'include'):
        copy_tree(core, Path(relative))
    for relative in ('CMakeLists.txt', 'cmake', 'tools/codegen',
                     'third_party/json', 'bindings/csharp', 'integrations/csharp',
                     'licenses', 'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md',
                     'docs/languages/unity.md', 'docs/api/unity.md',
                     'docs/languages/csharp.md', 'docs/api/csharp.md',
                     'tests/apple', 'tests/codegen/package_verify.py'):
        copy_tree(ROOT, Path(relative))
    # New entity declarations belong to the current SDK rather than the core branch.
    copy_tree(ROOT, Path('include/fengqudb_entity_types.h'))
    for name in ('build-apple-sdk.sh', 'assemble-unity-sdk.py', 'package-client.py',
                 'unity_assets.py', 'fqdb-generate.bat', 'fqdb-generate.command'):
        copy_tree(ROOT, Path('scripts') / name)

    inputs = stage / 'inputs'
    inputs.mkdir()
    windows = (args.windows_library.resolve(strict=True) if args.windows_library else
               core / 'build/index-win/Release/fengqudb.dll')
    linux = core / 'build/index-portable/libfengqudb.so.2'
    tools = {
        'windows': ROOT / 'artifacts/language-sdk-win/FengquDB-csharp-win-x64/FengquDB/Tools/fqdb-codegen.exe',
        'linux': ROOT / 'artifacts/language-sdk-linux/FengquDB-csharp-linux-x64/FengquDB/Tools/fqdb-codegen',
    }
    for platform, library, host in (('windows', windows, 'win-x64'), ('linux', linux, 'linux-x64')):
        dest = output / ('input-' + platform)
        subprocess.run([sys.executable, str(ROOT / 'scripts/package-client.py'),
                        '--languages', 'csharp', '--platform', host,
                        '--library', str(library), '--codegen', str(tools[platform]),
                        '--output', str(dest)], check=True)
        (inputs / platform).mkdir()
        shutil.copy2(dest / ('FengquDB-csharp-' + host + '.zip'), inputs / platform)
    (inputs / 'android').mkdir()
    shutil.copy2(core / 'build/unity-android-arm64/libfengqudb.so', inputs / 'android/libfengqudb.so')
    files = [{'path': p.relative_to(stage).as_posix(), 'size': p.stat().st_size,
              'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in sorted(stage.rglob('*')) if p.is_file()]
    manifest = {'core_commit': subprocess.check_output(['git', '-C', str(core), 'rev-parse', 'HEAD'], text=True).strip(),
                'files': files}
    (stage / 'snapshot.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    archive = output / 'apple-source.zip'
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                package.write(path, path.relative_to(stage).as_posix())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / 'apple-source.sha256').write_text(digest + '  apple-source.zip\n', encoding='ascii')
    workflow = '''name: Apple SDK
on:
  workflow_dispatch:
permissions:
  contents: read
concurrency:
  group: apple-sdk
  cancel-in-progress: false
jobs:
  build:
    runs-on: macos-15
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@v4
        with:
          persist-credentials: false
      - name: Check and unpack fixed build inputs
        run: |
          echo "DIGEST  apple-source.zip" | shasum -a 256 -c -
          python3 - <<'PY'
          import hashlib, json, pathlib, zipfile
          root = pathlib.Path('work').resolve()
          with zipfile.ZipFile('apple-source.zip') as z:
              for name in z.namelist():
                  if not (root/name).resolve().is_relative_to(root):
                      raise ValueError('Unsafe archive path')
              z.extractall(root)
          for item in json.loads((root/'snapshot.json').read_text())['files']:
              data = (root/item['path']).read_bytes()
              assert len(data) == item['size']
              assert hashlib.sha256(data).hexdigest() == item['sha256']
          PY
      - uses: actions/setup-dotnet@v4
        with:
          dotnet-version: '10.0.x'
      - name: Build and probe Apple libraries
        working-directory: work/source
        run: sh scripts/build-apple-sdk.sh ../inputs/apple build/apple-sdk
      - name: Assemble complete Unity packages
        working-directory: work/source
        run: |
          python3 scripts/assemble-unity-sdk.py --inputs ../inputs --output artifacts/unity-sdk
          python3 tests/codegen/package_verify.py --packages artifacts/unity-sdk --output build/unity-verify --manifest-only
          sw_vers > ../inputs/apple/build-environment.txt
          xcodebuild -version >> ../inputs/apple/build-environment.txt
          xcrun clang++ --version >> ../inputs/apple/build-environment.txt
          find ../inputs/apple -type f \\( -name '*.a' -o -name '*.dylib' -o -name fqdb-codegen \\) -exec file {} \\; >> ../inputs/apple/build-environment.txt
          find ../inputs/apple -type f -exec shasum -a 256 {} \\; > artifacts/apple-sha256.txt
      - uses: actions/upload-artifact@v4
        with:
          name: FengquDB-Apple-Unity
          retention-days: 1
          if-no-files-found: error
          path: |
            work/source/artifacts/unity-sdk/*.zip
            work/source/artifacts/apple-sha256.txt
            work/inputs/apple/
            work/snapshot.json
'''.replace('DIGEST', digest)
    (output / 'apple-sdk.yml').write_text(workflow, encoding='utf-8', newline='\n')
    print(json.dumps({'archive': str(archive), 'bytes': archive.stat().st_size,
                      'sha256': digest, 'files': len(files)}, indent=2))


if __name__ == '__main__':
    main()
