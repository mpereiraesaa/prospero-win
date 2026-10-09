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
parser.add_argument('--driver-association', action='store_true', help='Require real PS5 driver guest IDs and native association APIs')
parser.add_argument('--fullscreen', action='store_true', help='Actual 1920x1080 FLIP/A8R8G8B8/D16 fullscreen with guest focus retained')
parser.add_argument('--ui-only', action='store_true', help='Explicitly skip device calls to isolate callback routing')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if args.fullscreen and (args.ui_only or args.driver_association):
    parser.error('fullscreen focus probe currently requires ordinary real DXVK mode')
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_suffix('.c').resolve()
receipt = {'commands': [], 'console_accessed': False, 'device_calls_enabled': not args.ui_only, 'status': 'running',
           'sha256': {str(source): hashlib.sha256(source.read_bytes()).hexdigest()}}
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
         *extra, *(['-DPW_BRIDGE_DRIVER_ASSOCIATION', '-I', str(source.parents[2] / 'wine/ps5')] if args.driver_association else []), source, '-luser32', '-o', output / target], 'compile-' + compiler)
    receipt['sha256'][str(output / target)] = hashlib.sha256((output / target).read_bytes()).hexdigest()
environment = os.environ.copy()
environment.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG=os.environ.get('PW_WINDOW_WINEDEBUG', '-all'), WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',
                   DXVK_LOG_PATH=str(output), PW_BRIDGE_WINDOW_SESSION=uuid.uuid4().hex, PW_BRIDGE_WINDOW_DEVICE='0' if args.ui_only else '1', PW_BRIDGE_WINDOW_FULLSCREEN='1' if args.fullscreen else '0', PW_BRIDGE_BACKEND64='Z:' + str(args.backend64.resolve()).replace('/', '\\'))
stdout = run([args.wine_build.resolve() / 'loader/wine', output / 'client.exe', 'Z:' + str(output / 'service.dll').replace('/', '\\')],
    'window', environment)
rows = re.findall(r'PW_BRIDGE_WINDOW iteration=(\d+) status=00000000 error=0 .*create=([0-9a-f]+) reset=([0-9a-f]+) present=([0-9a-f]+)', stdout)
assert [row[0] for row in rows] == ['0', '1', '2'], rows
assert all(value == ('deadbeef' if args.ui_only else '00000000') for row in rows for value in row[1:]), rows
assert len(re.findall(r'guest_builtins=3 native_builtins=2', stdout)) == 3, stdout
if args.driver_association:
    assert len(re.findall(r'PW_BRIDGE_WINDOW_IDS iteration=[0-2] guest=2 attach=1 mirror=2 detach=1 rejects=3', stdout)) == 3, stdout
if args.fullscreen:
    log = (output / 'window.log').read_text()
    assert len(re.findall(r'PW_FULLSCREEN foreground=\S+ guest=\S+ service=\S+ guest_foreground=1 service_iconic=0 service_rect=0,0,1920,1080 cooperative=00000000', log)) == 3, log
receipt['fullscreen_focus_enabled'] = args.fullscreen
receipt['driver_association_enabled'] = args.driver_association
receipt['status'] = 'pass'
receipt['device_calls_enabled'] = not args.ui_only
receipt['sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                     [source, output / 'client.exe', output / 'service.dll', args.backend64.resolve()]}
if args.driver_association:
    for header in ['pw_d3d9_window.h', 'pw_d3d9_window_driver.h']:
        path = source.parents[2] / 'wine/ps5' / header
        receipt['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
receipt['scope'] = ('Architecture-local WNDPROCs, scalar guest callbacks before/during/after, native child UI, guest/native exceptions and repeated teardown; ' + ('device calls explicitly skipped' if args.ui_only else 'real DXVK CreateDevice/Reset/Present') + ('; actual PS5-driver opaque guest registration, native association, checked geometry and teardown' if args.driver_association else '') + '; no Vulkan display-plane lease or hardware input proof.')
(output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(output / 'receipt.json'))
