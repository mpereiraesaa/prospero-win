#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prove that guest and native imm32 each resolve their own "Wine IME" class."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine-build', type=Path, required=True,
                    help='host Wine build with patch 0911 (and 0902-0905)')
parser.add_argument('--baseline-wine', type=Path,
                    help='same build without 0911; must show the cross-domain procedure')
parser.add_argument('--prefix', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_suffix('.c').resolve()
receipt = {'schema': 1, 'tests': [], 'artifacts': {}}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command, name, env=None):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, env=env, timeout=90)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['tests'].append({'name': name, 'exit': result.returncode})
    return result

for bits, target, extra in [('i686', 'client.exe', []),
                            ('x86_64', 'service.dll', ['-shared', '-static-libgcc'])]:
    build = run([bits + '-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-Wno-array-bounds', *extra, source, '-limm32', '-o', out / target],
                'build-' + bits)
    assert build.returncode == 0, build.stderr
    receipt['artifacts'][target] = digest(out / target)
env = os.environ.copy()
env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all',
           WINEDLLOVERRIDES='mscoree,mshtml=;winedbg.exe=d')
service = 'Z:' + str(out / 'service.dll').replace('/', chr(92))
row = re.compile(r'PW_WINE_IME mode=(\w+) status=0+ guest=(\d+) guest_proc=([0-9a-f]+) '
                 r'guest_imm32=([0-9a-f]+) native=(\d+) native_proc=([0-9a-f]+) '
                 r'native_imm32=([0-9a-f]+) guest_after=(\d+)')

def parse(result):
    match = row.search(result.stdout)
    return match.groups() if match else None

if args.baseline_wine:
    # Unfixed: the native lookup returns the guest procedure (outside native imm32).
    base = run([args.baseline_wine.resolve() / 'loader/wine', out / 'client.exe', service,
                'classinfo'], 'baseline-classinfo', env)
    fields = parse(base)
    assert base.returncode == 1 and fields and fields[4] == '1', (base.returncode, fields)
    native_proc, guest_imm32 = int(fields[5], 16), int(fields[3], 16)
    assert guest_imm32 <= native_proc < guest_imm32 + 0x1000000, fields
    receipt['baseline'] = {'native_flags': 1, 'native_proc': fields[5], 'guest_imm32': fields[3]}
    # Creating the window from the native thread then runs guest code in 64-bit mode.
    crash = run([args.baseline_wine.resolve() / 'loader/wine', out / 'client.exe', service,
                 'force'], 'baseline-force', env)
    assert crash.returncode != 0 and not parse(crash), crash.returncode
    receipt['baseline']['force_exit'] = crash.returncode
for mode in ('classinfo', 'create', 'force'):
    result = run([args.wine_build.resolve() / 'loader/wine', out / 'client.exe', service, mode],
                 mode, env)
    fields = parse(result)
    assert result.returncode == 0 and fields, (mode, result.returncode, result.stdout)
    receipt[mode] = dict(zip(('mode', 'guest', 'guest_proc', 'guest_imm32', 'native',
                              'native_proc', 'native_imm32', 'guest_after'), fields))
receipt['scope'] = ('guest imm32 registers first; native service thread resolves and creates '
                    '"Wine IME" with its own imm32 instance; guest class unaffected afterwards')
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
