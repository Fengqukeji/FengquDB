"""Assemble Unity plug-ins with explicit platform import settings."""
import hashlib
from pathlib import Path
import shutil
import struct


def write_unity_assets(output, libraries, managed, analyzer, managed_ios=None):
    """Only advertise targets for which a real native binary was supplied."""
    assets = Path(output)
    paths = {
        'win-x64': 'Windows/x86_64/fengqudb.dll',
        'linux-x64': 'Linux/x86_64/libfengqudb.so',
        'android-arm64': 'Android/arm64-v8a/libfengqudb.so',
        'ios-arm64': 'iOS/libfengqudb.a',
        'macos-arm64': 'macOS/arm64/libfengqudb.dylib',
        'macos-x64': 'macOS/x86_64/libfengqudb.dylib',
    }
    if 'ios-arm64' in libraries and not managed_ios:
        raise ValueError('iOS native plug-in requires its __Internal managed assembly')
    for target, source in libraries.items():
        data = Path(source).read_bytes()
        if target in ('linux-x64', 'android-arm64'):
            machine = 62 if target == 'linux-x64' else 183
            if len(data) < 20 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != machine:
                raise ValueError('Wrong native architecture for ' + target)
        elif target == 'ios-arm64' and not data.startswith(b'!<arch>\n'):
            raise ValueError('Expected an iOS static archive')
        elif target == 'win-x64':
            if len(data) < 64 or data[:2] != b'MZ':
                raise ValueError('Expected a Windows native DLL')
            pe = struct.unpack_from('<I', data, 0x3c)[0]
            if pe + 24 > len(data) or data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0x8664:
                raise ValueError('Expected an x64 native DLL')
        elif target.startswith('macos'):
            if data[:4] != bytes.fromhex('cffaedfe'):
                raise ValueError('Expected a Mach-O native library')
            cpu = 0x0100000c if target.endswith('arm64') else 0x01000007
            if len(data) < 8 or struct.unpack_from('<I', data, 4)[0] != cpu:
                raise ValueError('Wrong macOS native architecture')

    def copy(source, relative, platforms, analyzer_asset=False):
        destination = assets / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        content = 'fileFormatVersion: 2\nguid: ' + hashlib.md5(('FengquDB.Unity/' + relative).encode()).hexdigest() + '\n'
        if analyzer_asset:
            content += 'labels:\n- RoslynAnalyzer\n'
        content += ('PluginImporter:\n  serializedVersion: 2\n  isPreloaded: 0\n'
                    '  isOverridable: 0\n  isExplicitlyReferenced: 0\n  validateReferences: 1\n  platformData:\n')
        for label, enabled, settings in platforms:
            first = label if ': ' in label else label + ':'
            content += f'  - first:\n      {first}\n    second:\n      enabled: {enabled}\n      settings: {settings}\n'
        content += '  userData: \n  assetBundleName: \n  assetBundleVariant: \n'
        destination.with_name(destination.name + '.meta').write_text(content, encoding='utf-8')

    copy(managed, 'Runtime/FengquDB.Managed.dll',
         [('Any', 1, '{Exclude iOS: 1}'), ('iPhone: iOS', 0, '{}')])
    # No asmdef ancestor: the source generator applies to business assemblies
    # throughout Assets; it is disabled as a runtime/editor plug-in.
    copy(analyzer, 'FengquDB.Generator.dll', [('Any', 0, '{}'), ('Editor: Editor', 0, '{}')], True)
    if 'ios-arm64' in libraries:
        copy(managed_ios, 'Runtime/iOS/FengquDB.Managed.dll',
             [('Any', 0, '{}'), ('Editor: Editor', 0, '{}'), ('iPhone: iOS', 1, '{}')])
    for target, source in libraries.items():
        config = [('Any', 0, '{}')]
        if target == 'android-arm64':
            config += [('Editor: Editor', 0, '{}'), ('Android: Android', 1, '{CPU: ARM64}')]
        elif target == 'ios-arm64':
            config += [('Editor: Editor', 0, '{}'), ('iPhone: iOS', 1, '{CPU: AnyCPU}')]
        else:
            os_name, standalone = ('Windows', 'Win64') if target.startswith('win') else ('Linux', 'Linux64') if target.startswith('linux') else ('OSX', 'OSXUniversal')
            cpu = 'ARM64' if target.endswith('arm64') else 'x86_64'
            config += [('Editor: Editor', 1, '{CPU: ' + cpu + ', OS: ' + os_name + '}'),
                       ('Standalone: ' + standalone, 1, '{CPU: ' + cpu + '}')]
        copy(source, 'Native/' + paths[target], config)
    return sorted(libraries)
