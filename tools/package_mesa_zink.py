#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validate and package both WGL/Zink architectures without enabling them."""
import argparse,hashlib,json,re,shutil,sys
from pathlib import Path
sys.dont_write_bytecode=True
from mesa_zink_manifest import pe_machine

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

MAX_MANIFEST = 1024 * 1024
MAX_DLL = 128 * 1024 * 1024
MAX_LICENSE = 16 * 1024 * 1024
MAX_DLLS_PER_ARCH = 32
MAX_IMPORTS = 256
MAX_LICENSES = 256

def token(value, pattern, what):
    if not isinstance(value, str) or not re.fullmatch(pattern, value, re.ASCII):
        raise ValueError('invalid ' + what)
    return value

def regular_input(directory, name, limit):
    path = directory
    for part in Path(name).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError('symlink package input: ' + name)
    if not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ValueError('missing or oversized package input: ' + name)
    return path

def validate(directory):
    manifest_path = regular_input(directory, 'manifest.json', MAX_MANIFEST)
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict) or manifest.get('driver') != 'zink' or manifest.get('architectures') != ['i386-windows', 'x86_64-windows']:
        raise ValueError('requires both WGL/Zink architectures')
    token(manifest.get('mesa_commit'), r'[0-9a-f]{40}', 'source provenance')
    token(manifest.get('llvm_mingw_archive_sha256'), r'[0-9a-f]{64}', 'compiler provenance')
    # This field is written to SOURCES.txt only after all copying succeeds.
    token(manifest.get('llvm_mingw'), r'[A-Za-z0-9][A-Za-z0-9._+-]{0,63}', 'compiler version')
    files = manifest.get('files')
    licenses = manifest.get('license_sha256')
    if not isinstance(files, dict) or not 4 <= len(files) <= 2 * MAX_DLLS_PER_ARCH:
        raise ValueError('invalid artifact count')
    if not isinstance(licenses, dict) or not 1 <= len(licenses) <= MAX_LICENSES:
        raise ValueError('invalid licence count')
    machines = {'i386-windows': 0x14c, 'x86_64-windows': 0x8664}
    seen = {arch: set() for arch in machines}
    for name, record in files.items():
        if not isinstance(name, str) or len(name) > 160:
            raise ValueError('invalid artifact path')
        parts = Path(name).parts
        if len(parts) != 2 or parts[0] not in machines:
            raise ValueError('unsafe artifact path')
        arch, basename = parts
        token(basename, r'[A-Za-z0-9_+.-]{1,123}\.[dD][lL][lL]', 'artifact basename')
        if basename.startswith('.') or basename.lower() in seen[arch]:
            raise ValueError('duplicate or unsafe artifact name')
        seen[arch].add(basename.lower())
        if len(seen[arch]) > MAX_DLLS_PER_ARCH or not isinstance(record, dict):
            raise ValueError('invalid artifact record')
        token(record.get('sha256'), r'[0-9a-f]{64}', 'artifact hash')
        if type(record.get('machine')) is not int or record['machine'] != machines[arch]:
            raise ValueError('artifact architecture mismatch: ' + name)
        dependencies = record.get('imports')
        if not isinstance(dependencies, list) or len(dependencies) > MAX_IMPORTS:
            raise ValueError('invalid import list')
        for dependency in dependencies:
            token(dependency, r'[A-Za-z0-9_+.-]{1,123}\.[dD][lL][lL]', 'import basename')
            if dependency.startswith('.'):
                raise ValueError('unsafe import basename')
            if dependency.lower() in ('libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll') and arch + '/' + dependency.lower() not in files:
                raise ValueError('missing runtime import ' + dependency)
        path = regular_input(directory, name, MAX_DLL)
        if digest(path) != record['sha256'] or pe_machine(path) != machines[arch]:
            raise ValueError('artifact hash/architecture mismatch: ' + name)
    for arch in machines:
        for basename in ['opengl32.dll', 'libgallium_wgl.dll']:
            if arch + '/' + basename not in files:
                raise ValueError('missing WGL artifact ' + arch + '/' + basename)
    for name, expected in licenses.items():
        if not isinstance(name, str) or len(name) > 256:
            raise ValueError('invalid licence path')
        parts = Path(name).parts
        if len(parts) < 2 or parts[0] != 'LICENSES' or any(part in ('.', '..') for part in parts):
            raise ValueError('unsafe licence path')
        token(expected, r'[0-9a-f]{64}', 'licence hash')
        if digest(regular_input(directory, name, MAX_LICENSE)) != expected:
            raise ValueError('licence hash mismatch: ' + name)
    return manifest

def package(directory,app):
    manifest=validate(directory) # Validate every input before copying any output.
    lib=app/'win/mesa-zink'
    for name in manifest['files']:
        target=lib/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(directory/name,target)
    for name in manifest['license_sha256']:
        target=app/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(directory/name,target)
    (app/'mesa-zink-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    with (app/'SOURCES.txt').open('a') as f:
        f.write('\nMesa WGL/Zink (PE32 and PE64)  https://github.com/mpereiraesaa/PS5_Mesa\n')
        f.write('  commit '+manifest['mesa_commit']+'\n')
        f.write('llvm-mingw '+manifest['llvm_mingw']+' archive SHA-256 '+manifest['llvm_mingw_archive_sha256']+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--app',type=Path);a=p.parse_args()
    if a.app:package(a.directory,a.app)
    else:validate(a.directory)
