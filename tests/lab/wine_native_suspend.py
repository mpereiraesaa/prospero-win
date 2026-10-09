#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the PE32 -> PE64 native-domain proof against a patched host Wine."""
import atexit
import sys
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
parser.add_argument('--dxvk64', type=Path)
parser.add_argument('--baseline-wine', type=Path)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_suffix('.c').resolve()
receipt = {'schema': 1, 'status':'running', 'tests': [], 'artifacts': {}, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}

def persist_terminal():
    if receipt['status'] == 'running':
        receipt['status'] = 'failed'
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

original_hook = sys.excepthook
def failure_hook(kind, error, traceback):
    receipt['status'] = 'timeout' if isinstance(error, subprocess.TimeoutExpired) else 'failed'
    receipt['error'] = {'type': kind.__name__, 'message': str(error)}
    original_hook(kind, error, traceback)
sys.excepthook = failure_hook
atexit.register(persist_terminal)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command, name, env=None, expected=0):
    try:
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, env=env, timeout=45)
    except subprocess.TimeoutExpired as error:
        (out / (name + '.log')).write_bytes((error.stdout or b'') + (error.stderr or b''))
        (out / 'failure.json').write_text(json.dumps({'status':'timeout','seconds':45}))
        raise
    receipt.setdefault('commands', []).append({'name': name, 'command': [str(x) for x in command], 'exit_code': result.returncode})
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
env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all,+seh',
           WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n')
env.pop('PW_TEST_DXVK64', None)
service_path = 'Z:' + str(out / 'service.dll').replace('/', chr(92))
command = [args.wine_build.resolve() / 'loader/wine', out / 'client.exe', service_path]
if args.baseline_wine:
    run([args.baseline_wine.resolve(), *command[1:]], 'baseline-rejects', env, 4)
for mode in ['native'] + (['dxvk'] if args.dxvk64 else []):
    if mode == 'dxvk':
        backend = args.dxvk64.resolve(strict=True)
        env['PW_TEST_DXVK64'] = 'Z:' + str(backend).replace('/', chr(92))
        receipt['artifacts']['dxvk64'] = digest(backend)
    output = run(command, mode, env)
    rows = re.findall(r'PW_NATIVE_DOMAIN iteration=(\d+) status=00000000 size=592 .* flags=(\d+)', output)
    suspend_rows=re.findall(r'PW_NATIVE_SUSPEND iteration=(\d+) rip=([0-9a-f]+) rsp=([0-9a-f]+)', (out / (mode+'.log')).read_text())
    assert len(suspend_rows)==80 and all(int(rip,16)>0xffffffff and int(rsp,16)>0xffffffff for _,rip,rsp in suspend_rows), suspend_rows
    receipt['suspensions']={'count':80,'high_rip_rsp':True,'rbx_r13_sentinels':True,'guest_suspend_before_after':True}
    expected = 63 if mode == 'dxvk' else 31
    assert rows == [(str(i), str(expected)) for i in range(10)], rows
    tokens = [(int(a), int(b)) for a, b in re.findall(r' token=(\d+) child_token=(\d+)', output)]
    assert len(tokens) == 10 and all(a > 0 and a == b for a,b in tokens), tokens
    assert all(tokens[i][0] < tokens[i+1][0] for i in range(9)), tokens
    receipt[mode] = {'iterations': 10, 'flags': expected, 'exit': 0, 'tokens': tokens}
receipt['status']='pass'
assert receipt['source_sha256']==hashlib.sha256(source.read_bytes()).hexdigest()
receipt['scope'] = ('Native PE64 suspension in MsgWait, explicit >4GiB stack and PC, RBX/R13 sentinels, PE32 suspend before/after; ''PE64 module load/unload, native child inheritance, PE32 guest threads before/after, '
                    'native session token inheritance/non-reuse, guest query rejection, CRT/Win32 TLS, >4GiB storage, vectored exceptions, malformed version/length, missing DLL recovery')
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
