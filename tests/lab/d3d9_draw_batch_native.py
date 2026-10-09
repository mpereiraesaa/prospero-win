#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Actual DXVK pixels through synchronous service and negotiated draw batch service."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
for name in ('wine-build', 'prefix', 'backend', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
expected_backend = '1d626ff743d93f78e1369c1f28daf5bb3b7c0832eef2876a6f352ec48f575710'
names = ['tests/lab/d3d9_draw_batch_native.c',
         'wine/ps5/d3d9/pw_d3d9_service_methods.c',
         'wine/ps5/d3d9/pw_d3d9_binding_leases.c',
         'wine/ps5/d3d9/pw_d3d9_native_command.c',
         'wine/ps5/d3d9/pw_d3d9_native_getter.c',
         'wine/ps5/d3d9/pw_d3d9_service_stateblock.c',
         'wine/ps5/d3d9/pw_d3d9_native_stateblock.c',
         'wine/ps5/pw_d3d9_stateblock_wire.c',
         'wine/ps5/pw_d3d9_getter_wire.c',
         'wine/ps5/pw_d3d9_objects.c',
         'wine/ps5/pw_d3d9_command_batch.c',
         'wine/ps5/pw_d3d9_binding_plan.c',
         'wine/ps5/pw_d3d9_command_policy.c',
         'wine/ps5/pw_d3d9_draw_shadow.c',
         'wine/ps5/pw_d3d9_command_wire.c']
inputs = [root / 'tests/lab/d3d9_service_stateblock.c'] + [root / n for n in names] + [Path(__file__)] + sorted((root / 'wine/ps5').rglob('*.h'))
r = {'status': 'running', 'scope': 'PE64 real DXVK service batch pixels versus synchronous service; test-only local device context/acquisition; no IPC, guest admission, PS5 window or console proof',
     'sources': {}, 'artifacts': {}, 'commands': []}
def save():
    (out / 'receipt.json').write_text(json.dumps(r, indent=2) + '\n')
def run(command, label, env=None):
    item = {'command': list(map(str, command))}
    r['commands'].append(item)
    save()
    try:
        with (out / (label + '.log')).open('w') as log:
            result = subprocess.run(item['command'], stdout=log, stderr=subprocess.STDOUT,
                                    env=env, timeout=180)
    except subprocess.TimeoutExpired:
        r['status'] = 'timeout'
        item['timeout_seconds'] = 180
        save()
        raise
    item['exit_code'] = result.returncode
    save()
    assert result.returncode == 0, label
try:
    assert sha(a.backend) == expected_backend, 'backend differs from pinned proof input'
    r['backend'] = {str(a.backend.resolve()): sha(a.backend)}
    r['sources'] = {str(f): sha(f) for f in inputs}
    r['source_head'] = subprocess.check_output(['git', '-C', root, 'rev-parse', 'HEAD'], text=True).strip()
    binary = out / 'draw-batch-native.exe'
    run(['x86_64-w64-mingw32-gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-municode', '-DPW_D3D9_ENABLE_BATCH', '-DPW_D3D9_ENABLE_BINDING_TICKETS',
         '-DPW_D3D9_ENABLE_DRAW_BATCH', *[root / n for n in names], '-o', binary,
         '-ld3d9', '-ldxguid', '-luuid'], 'build')
    r['artifacts'][str(binary)] = sha(binary)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(a.prefix.resolve()), WINEDEBUG='-all',
               WINEDLLOVERRIDES='mscoree,mshtml=', DXVK_LOG_PATH=str(out))
    recording = out / 'recording-native.exe'
    recording_sources = ['tests/lab/d3d9_service_stateblock.c',
                         'wine/ps5/d3d9/pw_d3d9_service_stateblock.c',
                         'wine/ps5/d3d9/pw_d3d9_native_stateblock.c',
                         'wine/ps5/pw_d3d9_stateblock_wire.c',
                         'wine/ps5/pw_d3d9_objects.c']
    run(['x86_64-w64-mingw32-gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-municode', '-DPW_D3D9_ENABLE_DRAW_BATCH',
         *[root / n for n in recording_sources], '-o', recording,
         '-ld3d9', '-ldxguid', '-luuid'], 'recording-build')
    r['artifacts'][str(recording)] = sha(recording)
    run([a.wine_build.resolve() / 'loader/wine', recording, a.backend.resolve()], 'recording', env)
    recording_rows = [line for line in (out / 'recording.log').read_text(errors='replace').splitlines()
                      if line.startswith('PW_NATIVE_RECORDING ')]
    assert len(recording_rows) == 3 and all('status=0' in row for row in recording_rows), recording_rows
    r['recording_rows'] = recording_rows
    run([a.wine_build.resolve() / 'loader/wine', binary, a.backend.resolve()], 'native', env)
    rows = [line for line in (out / 'native.log').read_text(errors='replace').splitlines()
            if line.startswith('PW_DRAW_BATCH_NATIVE ')]
    assert len(rows) == 3 and all('status=0' in row for row in rows), rows
    r['rows'] = rows
    assert all(sha(Path(f)) == h for f, h in r['sources'].items()), 'source changed during proof'
    r['status'] = 'pass'
except BaseException as exc:
    if r['status'] == 'running':
        r['status'] = 'failed'
    r['error'] = repr(exc)
    raise
finally:
    save()
print(out / 'receipt.json')
