"""Merge this build_py command into the project's setuptools configuration.

This runs during a package build, never during database reads or writes.
The generated packages are registered after generation, including a first build
where Generated/python did not exist during setup.py package discovery.
"""
import os
from pathlib import Path
import subprocess
from setuptools.command.build_py import build_py
from setuptools import find_packages


class BuildFengquDBModels(build_py):
    def run(self):
        root = Path.cwd()
        source = (root/os.environ.get('FQDB_ENTITY_SOURCE', '.')).resolve()
        local = root/('fqdb-codegen.exe' if os.name == 'nt' else 'fqdb-codegen')
        compiler = os.environ.get('FQDB_CODEGEN') or (str(local) if local.is_file() else 'fqdb-codegen')
        subprocess.run([compiler, 'generate',
                        '--source', str(source), '--source-language', 'python',
                        '--output', str(root/'Generated'), '--schema-lock',
                        str(root/'FengquDB.schema.lock.json')], check=True)
        generated = root/'Generated/python'
        packages = list(self.packages or [])
        package_dir = dict(self.package_dir or {})
        for package in find_packages(where=str(generated)):
            directory = generated.joinpath(*package.split('.'))
            if package in packages:
                if Path(self.get_package_dir(package)).resolve() != directory.resolve():
                    raise RuntimeError('Generated package conflicts with a business package: '+package)
            else:
                packages.append(package)
            package_dir[package] = os.path.relpath(directory, root)
        self.packages = self.distribution.packages = packages
        self.package_dir = self.distribution.package_dir = package_dir
        super().run()
