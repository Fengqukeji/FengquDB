"""Assemble complete Unity ZIPs from the platform CI artifacts."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import zipfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    inputs = args.inputs.resolve()
    source = {}
    for name, host in (('windows', 'win-x64'), ('linux', 'linux-x64')):
        archive = inputs/name/('FengquDB-csharp-'+host+'.zip')
        destination = inputs/('extracted-'+name)
        destination.mkdir(exist_ok=False)
        with zipfile.ZipFile(archive) as package:
            for item in package.infolist():
                target = (destination/item.filename).resolve()
                if not target.is_relative_to(destination): raise ValueError('Unsafe artifact member')
            package.extractall(destination)
        source[name] = destination/'FengquDB'
    apple = inputs/'apple'
    producer = Path(__file__).with_name('package-client.py')
    common = [sys.executable, str(producer), '--languages', 'unity', '--clients', str(apple),
              '--managed', str(source['windows']/'FengquDB.Managed.dll'),
              '--analyzer', str(source['windows']/'FengquDB.Generator.dll'),
              '--managed-ios', str(apple/'managed-ios/FengquDB.Managed.dll'),
              '--unity-win-x64', str(source['windows']/'Native/fengqudb.dll'),
              '--unity-linux-x64', str(source['linux']/'Native/libfengqudb.so'),
              '--unity-android-arm64', str(inputs/'android/libfengqudb.so'),
              '--unity-ios-arm64', str(apple/'iphoneos/arm64/libfengqudb.a'),
              '--unity-macos-arm64', str(apple/'macos/arm64/libfengqudb.dylib'),
              '--unity-macos-x64', str(apple/'macos/x86_64/libfengqudb.dylib'),
              '--output', str(args.output)]
    subprocess.run(common+['--platform','win-x64','--library',str(source['windows']/'Native/fengqudb.dll'),
                          '--codegen',str(source['windows']/'Tools/fqdb-codegen.exe')], check=True)
    for arch,host,cpu in (('arm64','macos-arm64',0x0100000c),('x86_64','macos-x64',0x01000007)):
        mac_tool=apple/'Tools'/arch/'fqdb-codegen'
        header=mac_tool.read_bytes()[:8]
        if len(header)!=8 or header[:4]!=bytes.fromhex('cffaedfe') or struct.unpack_from('<I',header,4)[0]!=cpu:
            raise ValueError('Expected a matching native macOS host tool: '+str(mac_tool))
        subprocess.run(common+['--platform',host,'--library',str(apple/'macos'/arch/'libfengqudb.dylib'),
                              '--codegen',str(mac_tool)], check=True)


if __name__ == '__main__': main()
