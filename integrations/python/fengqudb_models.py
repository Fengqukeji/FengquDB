"""Optional development-time entity generation, using the SDK's bundled tool."""
import os
from pathlib import Path
import subprocess
import sys


def generate(project, source='.'):
    """Generate once at application startup; skip with FQDB_CODEGEN_SKIP=1."""
    project = Path(project).resolve()
    if project.is_file():
        project = project.parent
    generated = project/'Generated'
    if os.environ.get('FQDB_CODEGEN_SKIP') != '1':
        # This module is imported from SDK/fengqudb.zip.
        sdk = Path(__file__).resolve().parents[1]
        compiler = sdk/'Tools'/('fqdb-codegen.exe' if os.name == 'nt' else 'fqdb-codegen')
        subprocess.run([str(compiler), 'generate', '--source', str(project/source),
                        '--source-language', 'python', '--schema-lock',
                        str(project/'FengquDB.schema.lock.json'), '--output', str(generated)], check=True)
    for path in (str(project/source), str(generated/'python')):
        if path not in sys.path:
            sys.path.insert(0, path)
    return generated
