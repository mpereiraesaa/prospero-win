#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validate and package both WGL/Zink architectures without enabling them."""
import argparse,hashlib,json,re,shutil,sys
from pathlib import Path
sys.dont_write_bytecode=True
from mesa_zink_manifest import pe_machine

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def validate(directory):
    manifest=json.loads((directory/'manifest.json').read_text())
    if manifest.get('driver')!='zink' or manifest.get('architectures')!=['i386-windows','x86_64-windows']:
        raise ValueError('requires both WGL/Zink architectures')
    if not re.fullmatch(r'[0-9a-f]{40}',manifest.get('mesa_commit','')) or not re.fullmatch(r'[0-9a-f]{64}',manifest.get('llvm_mingw_archive_sha256','')):
        raise ValueError('missing pinned source/compiler provenance')
    files=manifest.get('files',{});licenses=manifest.get('license_sha256',{})
    for arch,machine in [('i386-windows',0x14c),('x86_64-windows',0x8664)]:
        for name in ['opengl32.dll','libgallium_wgl.dll']:
            if arch+'/'+name not in files:raise ValueError('missing WGL artifact '+arch+'/'+name)
        for name,record in files.items():
            if not name.startswith(arch+'/'):continue
            path=directory/name
            if Path(name).parts!=(arch,Path(name).name) or path.suffix.lower()!='.dll':raise ValueError('unsafe artifact path')
            if digest(path)!=record['sha256'] or pe_machine(path)!=machine or record['machine']!=machine:
                raise ValueError('artifact hash/architecture mismatch: '+name)
            # Any compiler runtime imported by the supplied artifact must travel with it.
            for dependency in record.get('imports',[]):
                if dependency.lower() in ('libc++.dll','libunwind.dll','libwinpthread-1.dll') and arch+'/'+dependency.lower() not in files:
                    raise ValueError('missing runtime import '+dependency)
    if set(files)-{name for name in files if name.startswith(('i386-windows/','x86_64-windows/'))}:
        raise ValueError('unexpected artifact architecture')
    if not licenses:raise ValueError('missing component licences')
    for name,expected in licenses.items():
        if Path(name).is_absolute() or '..' in Path(name).parts or not name.startswith('LICENSES/'):
            raise ValueError('unsafe licence path')
        if digest(directory/name)!=expected:raise ValueError('licence hash mismatch: '+name)
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
