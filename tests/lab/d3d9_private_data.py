#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Actual local COM private-data ownership on PE32 and PE64."""
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
files = ['tests/lab/d3d9_private_data.c', 'wine/ps5/d3d9/pw_d3d9_private_data.c',
         'wine/ps5/d3d9/pw_d3d9_private_data.h']
receipt = {'status': 'running', 'sources': {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in files},
           'commands': [], 'console_accessed': False, 'scope': 'Real PE32/PE64 local metadata and controlled IUnknown reentrancy; no native backend or wire transport.'}
def run(command, name, env=None):
    result = subprocess.run(list(map(str, command)), env=env, text=True, capture_output=True, timeout=120)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append({'command': list(map(str, command)), 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.returncode}')
    return result.stdout
try:
    env = os.environ.copy()
    env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=')
    for compiler in ['i686', 'x86_64']:
        target = out / (compiler + '.exe')
        run([compiler + '-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', root / files[0], root / files[1], '-o', target], 'compile-' + compiler)
        receipt[compiler + '_sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
        text = run([args.wine_build.resolve() / 'loader/wine', target], compiler, env)
        assert 'PW_PRIVATE_DATA PASS copied=1 local_unknown=1 reentrant=1 bounded=1 cleanup=1' in text, text
    receipt['status'] = 'pass'
except Exception:
    receipt['status'] = 'failed'
    raise
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(out / 'receipt.json'))
