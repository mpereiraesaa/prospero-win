#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Controlled program readback through shipping COM/frontend and real wire codecs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--wine-build',type=Path,required=True)
p.add_argument('--prefix',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--expect-denied',action='store_true')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2];source=Path(__file__).with_suffix('.c')
r={'status':'running','scope':'Actual PE32/PE64 shipping program COM/frontend and wire codecs with controlled backend replies and denied WPM; no native backend acceptance','commands':[],'sources':{}}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for f in [source,Path(__file__),*[root/'wine/ps5'/n for n in ['d3d9/pw_d3d9_device_proxy.c','d3d9/pw_d3d9_program_proxy.c','pw_d3d9_program_wire.c','pw_d3d9_program_query.c']],*sorted((root/'wine/ps5').rglob('*.h'))]:r['sources'][str(f)]=sha(f)
try:
 for arch in ['i686','x86_64']:
  exe=out/(arch+'.exe');commands=[['%s-w64-mingw32-gcc'%arch,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds',str(source),*[str(root/'wine/ps5'/n) for n in ['d3d9/pw_d3d9_program_proxy.c','pw_d3d9_program_wire.c','pw_d3d9_program_query.c']],'-luser32','-luuid','-ldxguid','-o',str(exe)],[str(a.wine_build.resolve()/'loader/wine'),str(exe),*(['--expect-denied'] if a.expect_denied else [])]]
  for i,cmd in enumerate(commands):
   env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
   with (out/(arch+'-'+str(i)+'.log')).open('w') as log:result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
   r['commands'].append({'command':cmd,'exit':result.returncode})
   if result.returncode:raise RuntimeError(arch+' phase '+str(i)+' exit '+str(result.returncode))
  assert 'PROGRAM_COPY PASS' in (out/(arch+'-1.log')).read_text()
  r[arch]=sha(exe)
 for f,h in r['sources'].items():assert sha(Path(f))==h,f
 r['status']='pass'
except BaseException as e:
 r['status']='timeout' if isinstance(e,subprocess.TimeoutExpired) else 'failed';r['error']=str(e);raise
finally:(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
