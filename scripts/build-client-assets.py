"""Build distributable language modules with the existing release toolchains."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--java-bin', type=Path, required=True)
    p.add_argument('--node-dist', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    classes = output/'java-classes'
    classes.mkdir()
    subprocess.run([str(args.java_bin/'javac'), '-encoding', 'UTF-8', '--release', '11',
                    '-d', str(classes), *map(str, (ROOT/'bindings/java/io/fengqudb').glob('*.java'))], check=True)
    java = output/'java'
    java.mkdir()
    subprocess.run([str(args.java_bin/'jar'), '--create', '--file', str(java/'fengqudb.jar'),
                    '-C', str(classes), '.'], check=True)
    python = output/'python'
    python.mkdir()
    with zipfile.ZipFile(python/'fengqudb.zip', 'x', zipfile.ZIP_DEFLATED) as archive:
        for source in (ROOT/'bindings/python').glob('*.py'):
            archive.write(source, source.name)
        archive.write(ROOT/'integrations/python/fengqudb_models.py', 'fengqudb_models.py')
    node = output/'node/dist/src'
    node.mkdir(parents=True)
    sources = list(args.node_dist.glob('*.js')) + list(args.node_dist.glob('*.d.ts'))
    if not sources or not (args.node_dist/'index.js').is_file():
        raise RuntimeError('Compile the Node wrapper before packaging')
    for source in sources:
        shutil.copy2(source, node/source.name)
    print(output)


if __name__ == '__main__':
    main()
