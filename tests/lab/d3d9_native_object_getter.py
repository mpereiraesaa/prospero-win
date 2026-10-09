#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile native object getter adapter and exercise real DXVK through host Wine."""
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
files = ['tests/lab/d3d9_native_object_getter.c', 'wine/ps5/d3d9/pw_d3d9_native_object_getter.c',
         'wine/ps5/d3d9/pw_d3d9_native_object_getter.h',
         'wine/ps5/pw_d3d9_object_getter.c', 'wine/ps5/pw_d3d9_object_getter.h']
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
receipt = {'status': 'running', 'sources': {name: digest(root / name) for name in files},
           'backend_sha256': digest(args.backend64), 'commands': [], 'console_accessed': False,
           'scope': 'Actual PE64 DXVK typed getters, null binding, exact failed HRESULT and retained bound buffers; no registry/proxy/console claim.'}
def run(command, name, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120, env=env)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append({'name': name, 'command': list(map(str, command)), 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.returncode}')
    return result.stdout
try:
    run(['x86_64-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-municode',
         root / files[0], root / files[1], root / files[3], '-ldxguid', '-luuid', '-o', out / 'native.exe'], 'compile')
    receipt['executable_sha256'] = digest(out / 'native.exe')
    env = os.environ.copy()
    env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n', DXVK_LOG_LEVEL='error')
    for cycle in range(3):
        text = run([args.wine_build.resolve() / 'loader/wine', out / 'native.exe',
                    'Z:' + str(args.backend64.resolve()).replace('/', chr(92))], f'cycle-{cycle}', env)
        rows = re.findall(r'PW_NATIVE_OBJECT method=(\d+) hr=([0-9a-f]+) kind=(\d)', text)
        assert [row[0] for row in rows] == ['18','38','40','64','64','88','93','108','101','105'], text
        assert rows[2][1:] == ('88760866','0'), text
        assert all(row[1] == '00000000' for i,row in enumerate(rows) if i != 2), text
        assert 'PW_NATIVE_OBJECT PASS' in text, text
    receipt['status'] = 'pass'
except Exception:
    receipt['status'] = 'failed'
    raise
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(out / 'receipt.json'))
