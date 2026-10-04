#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile and inspect the bounded 32-bit Win32 fixture; never execute it."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='i686-w64-mingw32-gcc')
    parser.add_argument('--out', required=True, type=Path, help='artifact directory')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler: parser.error(f'compiler not found: {args.cc}')
    source = ROOT / 'tests/fixtures/wine_sync_ordinary.c'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'wine-sync-ordinary.exe'
    command = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-ident',
               '-nostdlib', '-Wl,--subsystem,console', '-Wl,--entry,_mainCRTStartup@0',
               '-Wl,--no-insert-timestamp', str(source), '-o', str(exe), '-lkernel32']
    subprocess.run(command, check=True)
    data = exe.read_bytes()
    if data[:2] != b'MZ': raise RuntimeError('not a PE image')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0': raise RuntimeError('invalid PE signature')
    machine = struct.unpack_from('<H', data, pe + 4)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    entry = struct.unpack_from('<I', data, optional + 16)[0]
    subsystem = struct.unpack_from('<H', data, optional + 68)[0]
    if (machine, magic, subsystem) != (0x14c, 0x10b, 3):
        raise RuntimeError('fixture must be x86 PE32 console')
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    section_table = optional + struct.unpack_from('<H', data, pe + 20)[0]
    entry_is_executable = False
    for i in range(sections):
        section = section_table + 40 * i
        size, address = struct.unpack_from('<II', data, section + 8)
        characteristics = struct.unpack_from('<I', data, section + 36)[0]
        if address <= entry < address + size and characteristics & 0x20000000:
            entry_is_executable = True
    if not entry or not entry_is_executable: raise RuntimeError('entry point is not executable')
    objdump = shutil.which('i686-w64-mingw32-objdump')
    if not objdump: parser.error('i686-w64-mingw32-objdump is required for import inspection')
    imports = subprocess.check_output([objdump, '-p', str(exe)], text=True)
    dlls = [line.split('DLL Name:', 1)[1].strip() for line in imports.splitlines() if 'DLL Name:' in line]
    if [dll.lower() for dll in dlls] != ['kernel32.dll']:
        raise RuntimeError(f'unexpected CRT/runtime imports: {dlls}')
    (out / 'pe-inspection.txt').write_text(imports)
    manifest = {
        'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'artifact': str(exe), 'sha256': hashlib.sha256(data).hexdigest(),
        'compiler_version': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
        'command': command, 'machine': machine, 'optional_header_magic': magic,
        'subsystem': subsystem, 'imports': dlls, 'case_groups': 12,
        'entry_rva': entry, 'entry_in_executable_section': entry_is_executable,
        'runtime_executed': False,
        'scope': 'Compilation, PE32 header and kernel32-only import checks. No semantic/runtime acceptance.',
        'report_file': 'pw-sync-ordinary.log in the application working directory',
    }
    (out / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built PE32 console fixture: {exe}')
    print(f'SHA256 {manifest["sha256"]}; runtime not executed')


if __name__ == '__main__':
    main()
