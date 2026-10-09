#!/usr/bin/env python3
"""Real PE32/PE64 UI callback and DXVK device/reset/present smoke proof."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine-build', type=Path, required=True)
parser.add_argument('--prefix', type=Path, required=True)
parser.add_argument('--backend64', type=Path, required=True)
parser.add_argument('--ui-only', action='store_true', help='Explicitly skip device calls to isolate callback routing')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_suffix('.c').resolve()
receipt = {'commands': [], 'console_accessed': False, 'device_calls_enabled': not args.ui_only, 'status': 'running'}
def run(command, name, environment=None):
    result = subprocess.run(list(map(str, command)), env=environment, capture_output=True, text=True, timeout=60)
    (output / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append(dict(command=list(map(str, command)), exit_code=result.returncode,
                                    stdout=result.stdout, stderr=result.stderr))
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if result.returncode:
        receipt['status'] = 'failed'
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        raise RuntimeError(f'{name} failed ({result.returncode}); see {output / (name + ".log")}')
    return result.stdout
for compiler, target, extra in [('i686', 'client.exe', []), ('x86_64', 'service.dll', ['-shared', '-static-libgcc'])]:
    run([compiler + '-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-array-bounds',
         *extra, source, '-luser32', '-o', output / target], 'compile-' + compiler)
environment = os.environ.copy()
environment.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG=os.environ.get('PW_WINDOW_WINEDEBUG', '-all'), WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',
                   DXVK_LOG_PATH=str(output), PW_BRIDGE_WINDOW_SESSION=uuid.uuid4().hex, PW_BRIDGE_WINDOW_DEVICE='0' if args.ui_only else '1', PW_BRIDGE_BACKEND64='Z:' + str(args.backend64.resolve()).replace('/', '\\'))
stdout = run([args.wine_build.resolve() / 'loader/wine', output / 'client.exe', 'Z:' + str(output / 'service.dll').replace('/', '\\')],
    'window', environment)
rows = re.findall(r'PW_BRIDGE_WINDOW iteration=(\d+) status=00000000 error=0 .*create=([0-9a-f]+) reset=([0-9a-f]+) present=([0-9a-f]+)', stdout)
assert [row[0] for row in rows] == ['0', '1', '2'], rows
assert all((value == 'deadbeef') == args.ui_only for row in rows for value in row[1:]), rows
receipt['status'] = 'pass'
receipt['device_calls_enabled'] = not args.ui_only
receipt['sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                     [source, output / 'client.exe', output / 'service.dll', args.backend64.resolve()]}
receipt['scope'] = ('Architecture-local WNDPROCs, scalar guest callbacks before/during/after, native child UI, guest/native exceptions and repeated teardown; ' + ('device calls explicitly skipped' if args.ui_only else 'real DXVK CreateDevice/Reset/Present') + '; no PS5 plane association or hardware input proof.')
(output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(output / 'receipt.json'))
