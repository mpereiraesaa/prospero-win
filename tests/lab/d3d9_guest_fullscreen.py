#!/usr/bin/env python3
"""Real PE32/PE64 guest window transitions; no D3D backend or console."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
for name in ('wine-build','prefix','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
inputs=[root/'tests/lab/d3d9_guest_fullscreen.c',Path(__file__),root/'wine/ps5/d3d9/pw_d3d9_guest_fullscreen.h',root/'wine/ps5/d3d9/pw_d3d9_device_proxy.c']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
frozen={str(p):sha(p) for p in inputs};receipt={'status':'running','sources':frozen,'commands':[],'scope':'Real Win32 transitions and extent predicate; no complete production session or GPU/console proof.'}
def save():(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
def run(cmd,name,env=None):
 row={'command':list(map(str,cmd))};receipt['commands'].append(row);save()
 try:
  with (out/(name+'.log')).open('w') as log:r=subprocess.run(row['command'],stdout=log,stderr=subprocess.STDOUT,env=env,timeout=60)
  row['exit']=r.returncode;save();assert r.returncode==0,name
 except subprocess.TimeoutExpired:row['timeout']=60;raise
try:
 env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
 for abi,compiler in [('32','i686-w64-mingw32-gcc'),('64','x86_64-w64-mingw32-gcc')]:
  exe=out/('fixture'+abi+'.exe')
  run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(root/'wine/ps5/d3d9'),inputs[0],'-luser32','-luuid','-ldxguid','-o',exe],'compile'+abi)
  run([a.wine_build.resolve()/'loader/wine',exe],'run'+abi,env)
  assert (out/('run'+abi+'.log')).read_text().count('PW_GUEST_FULLSCREEN cycle=')==3
 assert frozen=={str(p):sha(p) for p in inputs}
 receipt['status']='pass'
except BaseException as e:
 receipt['status']='timeout' if isinstance(e,subprocess.TimeoutExpired) else 'failed';receipt['error']=repr(e);raise
finally:save()
print('PASS '+str(out/'receipt.json'))
