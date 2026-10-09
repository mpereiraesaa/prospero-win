#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Actual PE32 staging allocations with controlled transport failure injection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
parser = argparse.ArgumentParser(description=__doc__)
for name in ('wine-build', 'prefix', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
files = ['tests/lab/d3d9_texture_client.c', 'wine/ps5/d3d9/pw_d3d9_texture_client.c',
         'wine/ps5/d3d9/pw_d3d9_texture_client.h', 'wine/ps5/pw_d3d9_texture_wire.h',
         'wine/ps5/d3d9/pw_d3d9_staging.c', 'wine/ps5/d3d9/pw_d3d9_staging.h', 'wine/ps5/pw_d3d9_texture_wire.c']
receipt = {'status': 'running', 'sources': {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in files},
           'commands': [], 'console_accessed': False, 'scope': 'Real PE32 low-address allocations and controlled transport faults, not a real backend or COM proxy proof.'}
def run(command, name, env=None):
    result = subprocess.run(list(map(str, command)), env=env, text=True, capture_output=True, timeout=120)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append({'command': list(map(str, command)), 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.returncode}')
    return result.stdout
try:
    run(['i686-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', root / files[0], root / files[1], root / files[4], root / files[6], '-o', out / 'client.exe'], 'compile')
    receipt['executable_sha256'] = hashlib.sha256((out / 'client.exe').read_bytes()).hexdigest()
    env = os.environ.copy()
    env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=')
    text = run([args.wine_build.resolve() / 'loader/wine', out / 'client.exe'], 'client', env)
    assert 'PW_TEXTURE_CLIENT pitch=520 low32=1 copied=1 readonly=1 failure_cleanup=1 budget=1' in text, text
    assert 'PW_TEXTURE_CLIENT pitch=-520 low32=1 copied=1 readonly=1 failure_cleanup=1 budget=1' in text, text
    assert 'PW_TEXTURE_CLIENT PASS' in text, text
    receipt['status'] = 'pass'
except Exception:
    receipt['status'] = 'failed'
    raise
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(out / 'receipt.json'))
