#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the PE32 -> PE64 native-domain proof against a patched host Wine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine-build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--prefix', type=Path, required=True)
parser.add_argument('--backend64', type=Path)
parser.add_argument('--baseline-wine', type=Path)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_suffix('.c').resolve()
receipt = {'schema': 1, 'tests': [], 'artifacts': {}}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command, name, env=None, expected=0):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, env=env, timeout=90)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f'{name}: exit {result.returncode}; see {out / (name + ".log")}')
    receipt['tests'].append(name)
    return result.stdout

for bits, target, extra in [('i686', 'client.exe', []),
                            ('x86_64', 'service.dll', ['-shared', '-static-libgcc'])]:
    run([bits + '-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wno-array-bounds', *extra, source, '-o', out / target], 'build-' + bits)
    receipt['artifacts'][target] = digest(out / target)
env = os.environ.copy()
env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all',
           WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n')
env.pop('PW_BRIDGE_BACKEND64', None)
service_path = 'Z:' + str(out / 'service.dll').replace('/', chr(92))
command = [args.wine_build.resolve() / 'loader/wine', out / 'client.exe', service_path]
if args.baseline_wine:
    run([args.baseline_wine.resolve(), *command[1:]], 'baseline-rejects', env, 4)
for mode in ['native'] + (['dxvk'] if args.backend64 else []):
    if mode == 'dxvk':
        backend = args.backend64.resolve(strict=True)
        env['PW_BRIDGE_BACKEND64'] = 'Z:' + str(backend).replace('/', chr(92))
        receipt['artifacts']['backend64'] = digest(backend)
    output = run(command, mode, env)
    rows = re.findall(r'PW_NATIVE_DOMAIN iteration=(\d+) status=00000000 size=592 .* flags=(\d+)', output)
    expected = 63 if mode == 'dxvk' else 31
    assert rows == [(str(i), str(expected)) for i in range(10)], rows
    receipt[mode] = {'iterations': 10, 'flags': expected, 'exit': 0}
receipt['scope'] = ('PE64 module load/unload, native child inheritance, PE32 guest threads before/after, '
                    'CRT/Win32 TLS, >4GiB storage, vectored exceptions, malformed version/length')
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
