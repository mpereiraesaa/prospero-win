#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Controlled PE32/PE64 failure-ACK visibility and park cancellation proof."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--wine', type=Path, required=True)
p.add_argument('--prefix', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args(); root = Path(__file__).resolve().parents[2]
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
sources = ['tests/lab/d3d9_failure_ack.c', 'wine/ps5/pw_d3d9_bridge_wire.c',
           'wine/ps5/pw_d3d9_command_batch.c', 'wine/ps5/pw_d3d9_command_wire.c']
inputs = [root / s for s in sources] + [Path(__file__), root / 'wine/ps5/d3d9/pw_d3d9_session.c',
          root / 'wine/ps5/d3d9/pw_d3d9_failure_wait.h'] + list((root / 'wine/ps5').glob('*.h'))
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
r = dict(status='running', scope='Controlled real wire endpoints and production failure-wait helper; native COM dispatch and session ticket drops are modeled, not exercised.',
         inputs={str(f): sha(f) for f in inputs}, commands=[], artifacts={})
def save(): (out / 'receipt.json').write_text(json.dumps(r, indent=2) + '\n')
def run(command, env=None):
    e = dict(command=list(map(str, command))); r['commands'].append(e); save()
    result = subprocess.run(e['command'], cwd=root, env=env, capture_output=True, text=True, timeout=60)
    e.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr); save()
    assert result.returncode == 0
try:
    env = os.environ.copy(); env.update(WINEPREFIX=str(a.prefix.resolve()), WINEDEBUG='-all')
    for abi, cc in [('32', 'i686-w64-mingw32-gcc'), ('64', 'x86_64-w64-mingw32-gcc')]:
        target = out / (abi + '.exe')
        run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Iwine/ps5', '-Iwine/ps5/d3d9', *sources, '-o', target])
        r['artifacts'][str(target)] = sha(target); run([a.wine.resolve(), target], env)
    assert all(sha(Path(f)) == h for f, h in r['inputs'].items())
    r['status'] = 'pass'
except BaseException as e:
    r.update(status='timeout' if isinstance(e, subprocess.TimeoutExpired) else 'failed', error=repr(e)); raise
finally: save()
