#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Record architecture, imports and digests for the offline WGL/Zink build."""
import hashlib,json,re,shutil,struct,subprocess,sys,tarfile
from pathlib import Path

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def pe_machine(path):
    data=path.read_bytes()
    if data[:2]!=b'MZ' or len(data)<64:
        raise ValueError('not a PE image: '+str(path))
    pos=struct.unpack_from('<I',data,60)[0]
    if data[pos:pos+4]!=b'PE\0\0' or pos+26>len(data):
        raise ValueError('bad PE header: '+str(path))
    machine=struct.unpack_from('<H',data,pos+4)[0]
    optional=struct.unpack_from('<H',data,pos+24)[0]
    if (machine,optional) not in [(0x14c,0x10b),(0x8664,0x20b)]:
        raise ValueError('unsupported PE architecture: '+str(path))
    return machine

def verify_source(source,repository,revision):
    # git archive applies export attributes (including Mesa's CSV CRLF rule).
    # Compare to its exact bytes, not canonical blobs or a mutable checkout.
    process=subprocess.Popen(['git','-C',str(repository),'archive',revision],stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdout,mode='r|') as archive:
            for entry in archive:
                path=source/entry.name
                if entry.isdir():continue
                if entry.issym():
                    if not path.is_symlink() or str(path.readlink())!=entry.linkname:
                        raise ValueError('source differs from pin: '+entry.name)
                    continue
                if not entry.isfile():raise ValueError('unsupported archive member')
                expected=hashlib.sha256(archive.extractfile(entry).read()).digest()
                if hashlib.sha256(path.read_bytes()).digest()!=expected:
                    raise ValueError('source differs from pin: '+entry.name)
    finally:
        process.stdout.close()
        code=process.wait()
    if code:raise ValueError('cannot export pinned source')

COMPILER_RUNTIMES = {'libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll'}

def imports(path,readobj):
    output=subprocess.check_output([str(readobj),'--coff-imports',str(path)],text=True)
    return re.findall(r'(?m)^\s*Name: (.+)$',output)

def copy_runtime(directory,compiler_bin,readobj):
    pending=list(sorted(directory.glob('*.dll')));seen=set()
    while pending:
        path=pending.pop()
        if path.name.lower() in seen:continue
        seen.add(path.name.lower())
        for dependency in imports(path,readobj):
            name=dependency.lower()
            if name not in COMPILER_RUNTIMES:continue
            target=directory/name
            if not target.exists():shutil.copy2(compiler_bin/name,target)
            if pe_machine(target)!=pe_machine(path):raise ValueError('mixed runtime architecture')
            pending.append(target)

def write_notices(source,target):
    # Retain copyright/permission blocks, not only SPDX licence templates.
    # Include all source components: this intentionally over-includes notices.
    patterns=[r"/\*.*?\*/",r"<!--.*?-->",r'""".*?"""',r"'''.*?'''",
              r"(?m:^(?://[^\n]*(?:\n|$))+)",r"(?m:^(?:#[^\n]*(?:\n|$))+)" ]
    comment=re.compile("|".join(patterns),re.S)
    blocks={}
    for path in sorted(source.rglob('*')):
        if not path.is_file() or path.is_symlink():continue
        data=path.read_bytes()
        if b'\0' in data[:4096]:continue
        text=data.decode('utf-8',errors='replace')
        for match in comment.finditer(text):
            block=match.group().strip()
            if not re.search(r'copyright|SPDX-License-Identifier',block,re.I):continue
            blocks.setdefault(block,[]).append(str(path.relative_to(source)))
    with target.open('w') as output:
        output.write('Mesa source copyright and licence notices (pinned source)\n')
        for block,paths in blocks.items():
            output.write('\nSource: '+', '.join(paths)+'\n'+block+'\n')

def manifest(work,revision,compiler_version,compiler_sha,script):
    artifacts={}
    for arch,machine in [('i386-windows',0x14c),('x86_64-windows',0x8664)]:
        directory=work/'artifacts'/arch
        for name in ['opengl32.dll','libgallium_wgl.dll']:
            if not (directory/name).is_file():raise ValueError('missing '+str(directory/name))
        for path in sorted(directory.glob('*.dll')):
            if pe_machine(path)!=machine:raise ValueError('mixed architecture: '+str(path))
            dependencies=imports(path,work/'toolchain/bin/llvm-readobj')
            for dependency in dependencies:
                if dependency.lower() in COMPILER_RUNTIMES and not (directory/dependency.lower()).is_file():
                    raise ValueError('missing compiler runtime '+dependency)
            artifacts[arch+'/'+path.name]={'sha256':sha(path),'machine':machine,'imports':dependencies}
    return {'mesa_commit':revision,'mesa_version':'26.2.0','llvm_mingw':compiler_version,
            'llvm_mingw_archive_sha256':compiler_sha,'build_script_sha256':sha(script),
            'driver':'zink','architectures':['i386-windows','x86_64-windows'],
            'console_validated':False,'files':artifacts,
            'raw_dll_sha256':{str(p.relative_to(work)):sha(p) for p in sorted(work.glob('build-*/src/gallium/targets/*/*.dll'))},
            'license_sha256':{str(p.relative_to(work/'artifacts')):sha(p) for p in sorted((work/'artifacts/LICENSES').rglob('*')) if p.is_file()}}

if __name__=='__main__':
    if sys.argv[1]=='--copy-runtime':
        copy_runtime(Path(sys.argv[2]),Path(sys.argv[3]),Path(sys.argv[4]));raise SystemExit(0)
    if sys.argv[1]=='--notices':
        write_notices(Path(sys.argv[2]),Path(sys.argv[3]));raise SystemExit(0)
    if sys.argv[1]=='--verify-source':
        verify_source(Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4]);raise SystemExit(0)
    work=Path(sys.argv[1]);result=manifest(work,*sys.argv[2:5],Path(sys.argv[5]))
    (work/'artifacts/manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Built PE32 and PE64 Mesa WGL/Zink; manifest:',work/'artifacts/manifest.json')
