#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the PE32 COM factory proxy against the persistent PE64 DXVK service."""
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
receipt = {'schema': 1, 'sources': {}, 'artifacts': {}, 'tests': []}
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def run(command, label, env=None):
    p = subprocess.run([str(x) for x in command], env=env, capture_output=True,
                       text=True, timeout=120)
    (out / (label + '.log')).write_text(p.stdout + p.stderr)
    receipt['tests'].append({'name': label, 'exit': p.returncode})
    if p.returncode:
        raise RuntimeError(f'{label} failed: {p.returncode}')
    return p.stdout
sources = [root / 'wine/ps5' / name for name in (
    'd3d9/pw_d3d9_session.c', 'pw_d3d9_bridge_wire.c',
    'pw_d3d9_objects.c', 'pw_d3d9_factory_wire.c')]
proxy = root / 'wine/ps5/d3d9/pw_d3d9_proxy.c'
fixture = Path(__file__).with_suffix('.c')
for source in [fixture, proxy, *sources, *[p.with_suffix('.h') for p in sources]]:
    receipt['sources'][str(source.relative_to(root))] = digest(source)
try:
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-array-bounds']
    run(['i686-w64-mingw32-gcc', *flags, '-municode', fixture,
         '-luuid', '-ldxguid', '-o', out / 'client.exe'], 'build-client')
    run(['i686-w64-mingw32-gcc', *flags, '-shared', '-Wl,--kill-at', proxy, *sources,
         '-luuid', '-ldxguid', '-o', out / 'd3d9.dll'], 'build-proxy')
    run(['x86_64-w64-mingw32-gcc', *flags, '-shared', '-static-libgcc', *sources,
         '-luuid', '-o', out / 'service.dll'], 'build-service')
    for name in ('client.exe', 'd3d9.dll', 'service.dll'):
        receipt['artifacts'][name] = digest(out / name)
    backend = args.backend64.resolve(strict=True)
    receipt['artifacts']['backend64'] = digest(backend)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all',
               WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n')
    def windows(path):
        return 'Z:' + str(path).replace('/', chr(92))
    output = run([args.wine_build.resolve() / 'loader/wine', out / 'client.exe',
                  windows(out / 'd3d9.dll'), windows(out / 'service.dll'),
                  windows(backend)], 'proxy', env)
    rows = re.findall(r'PW_FACTORY_PROXY cycle=(\d+) identity=1 methods=11 unsupported=4 status=0', output)
    assert rows == ['0', '1', '2'], rows
    receipt['cycles'] = rows
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
