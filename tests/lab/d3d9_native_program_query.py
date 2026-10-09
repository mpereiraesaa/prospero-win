#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile native shader/declaration adapter and exercise real DXVK through host Wine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ('wine-build', 'prefix', 'backend64', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
files = ['tests/lab/d3d9_native_program_query.c', 'wine/ps5/d3d9/pw_d3d9_native_program.c',
         'wine/ps5/d3d9/pw_d3d9_native_program.h',
         'wine/ps5/pw_d3d9_program_wire.c', 'wine/ps5/pw_d3d9_program_wire.h',
         'wine/ps5/d3d9/pw_d3d9_native_program_query.c', 'wine/ps5/d3d9/pw_d3d9_native_program_query.h',
         'wine/ps5/pw_d3d9_program_query.c', 'wine/ps5/pw_d3d9_program_query.h']
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
receipt = {'status': 'running', 'sources': {name: digest(root / name) for name in files},
           'backend_sha256': digest(args.backend64), 'commands': [], 'console_accessed': False,
           'scope': 'Actual PE64 DXVK query vs direct zero/partial/full readback and implicit FVF declarations; no PE32 COM proxy or console game claim.'}
def run(command, name, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120, env=env)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append({'name': name, 'command': list(map(str, command)), 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.returncode}')
    return result.stdout
try:
    run(['x86_64-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-municode',
         root / files[0], root / files[1], root / files[3], root / files[5], root / files[7], '-ldxguid', '-luuid', '-o', out / 'native.exe'], 'compile')
    receipt['executable_sha256'] = digest(out / 'native.exe')
    env = os.environ.copy()
    env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n', DXVK_LOG_LEVEL='error')
    for cycle in range(3):
        text = run([args.wine_build.resolve() / 'loader/wine', out / 'native.exe',
                    'Z:' + str(args.backend64.resolve()).replace('/', chr(92))], f'cycle-{cycle}', env)
        assert re.findall(r'PW_NATIVE_PROGRAM kind=(\d) bytes=(\d+) pass=1', text) == [('7', '16'), ('8', '20'), ('9', '5000')], text
        assert 'PW_NATIVE_PROGRAM PASS' in text, text
    receipt['status'] = 'pass'
except Exception:
    receipt['status'] = 'failed'
    raise
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(out / 'receipt.json'))
