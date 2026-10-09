#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""PE32 admission proof plus PE64 feature-disabled service compilation."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for name in ('wine-build','prefix','output'): p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
names=['pw_d3d9_bridge_wire.c','pw_d3d9_objects.c','pw_d3d9_factory_wire.c','pw_d3d9_command_wire.c','pw_d3d9_getter_wire.c','pw_d3d9_command_batch.c','pw_d3d9_command_policy.c','pw_d3d9_binding_plan.c']
fixtures=[('on',Path(__file__).with_suffix('.c'),'BINDING_CLIENT PASS'),('off',root/'tests/lab/d3d9_batch_client.c','BATCH_CLIENT PASS')]
sources=[Path(__file__),*[f for _,f,_ in fixtures],root/'wine/ps5/d3d9/pw_d3d9_session.c',*[root/'wine/ps5'/n for n in names],*sorted((root/'wine/ps5').rglob('*.h'))]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
r={'status':'running','scope':'PE32 shipping client session and codecs with controlled peer; feature-on private lifetime and feature-off scalar regression, plus PE64 feature-off service compilation. No native backend or console acceptance.','sources':{str(f):sha(f) for f in sources},'commands':[],'artifacts':{}}
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
def run(command,label):
 entry={'command':command,'log':label+'.log'};r['commands'].append(entry)
 try:
  with (out/entry['log']).open('w') as log:
   result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
  entry['exit']=result.returncode
  assert result.returncode==0,(label,result.returncode)
 except subprocess.TimeoutExpired:
  entry['status']='timeout';raise
 return (out/entry['log']).read_text()
try:
 for abi,cc in [('32','i686-w64-mingw32-gcc')]:
  for mode,fixture,marker in fixtures:
   label=abi+'-'+mode;binary=out/(label+'.exe')
   cmd=[cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds',str(fixture),*[str(root/'wine/ps5'/n) for n in names],'-luuid','-ldxguid','-o',str(binary)]
   run(cmd,label+'-compile');r['artifacts'][str(binary)]=sha(binary)
   assert marker in run([str(a.wine_build.resolve()/'loader/wine'),str(binary)],label+'-run')
 binary=out/'64-off.o'
 run(['x86_64-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds','-DPW_D3D9_ENABLE_METHODS','-DPW_D3D9_ENABLE_BATCH','-c',str(root/'wine/ps5/d3d9/pw_d3d9_session.c'),'-o',str(binary)],'64-off-compile')
 r['artifacts'][str(binary)]=sha(binary)
 assert all(sha(Path(f))==h for f,h in r['sources'].items());r['status']='pass'
except BaseException as e:r.update(status='failed',error=repr(e));raise
finally:(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
