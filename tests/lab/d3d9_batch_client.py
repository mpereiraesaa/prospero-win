#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Controlled PE32 shipping batch admission/flush transport proof."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--wine-build',type=Path,required=True);p.add_argument('--prefix',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sources=[Path(__file__),Path(__file__).with_suffix('.c'),root/'wine/ps5/d3d9/pw_d3d9_session.c',*[root/'wine/ps5'/n for n in ['pw_d3d9_bridge_wire.c','pw_d3d9_objects.c','pw_d3d9_factory_wire.c','pw_d3d9_command_wire.c','pw_d3d9_getter_wire.c','pw_d3d9_command_batch.c','pw_d3d9_command_policy.c']],*sorted((root/'wine/ps5').rglob('*.h'))]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();r={'status':'running','scope':'Shipping PE32 session/codec with controlled peer acknowledgements, not native backend or console acceptance','sources':{str(p):sha(p) for p in sources},'commands':[]}
try:
 cmd=['i686-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds',str(Path(__file__).with_suffix('.c')),*[str(root/'wine/ps5'/n) for n in ['pw_d3d9_bridge_wire.c','pw_d3d9_objects.c','pw_d3d9_factory_wire.c','pw_d3d9_command_wire.c','pw_d3d9_getter_wire.c','pw_d3d9_command_batch.c','pw_d3d9_command_policy.c']],'-luuid','-ldxguid','-o',str(out/'client.exe')]
 env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
 for n,command in enumerate([cmd,[str(a.wine_build.resolve()/'loader/wine'),str(out/'client.exe')]]):
  with (out/(str(n)+'.log')).open('w') as log:x=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
  r['commands'].append({'command':command,'exit':x.returncode});assert x.returncode==0
 assert 'BATCH_CLIENT PASS' in (out/'1.log').read_text();assert all(sha(Path(p))==h for p,h in r['sources'].items());r.update(status='pass',binary_sha256=sha(out/'client.exe'))
except BaseException as e:r.update(status='failed',error=repr(e));raise
finally:(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
