#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Typed binding callbacks: actual PE32/64 proxy, controlled transport."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('wine-build','prefix','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
files=['tests/lab/d3d9_binding_adapter.c','wine/ps5/d3d9/pw_d3d9_device_methods.c','wine/ps5/pw_d3d9_command_wire.c','wine/ps5/pw_d3d9_getter_wire.c']
inputs=[root/f for f in files]+[root/'tests/lab/d3d9_device_methods.c',Path(__file__)]+sorted((root/'wine/ps5').rglob('*.h'))
r={'status':'running','sources':{str(f):sha(f) for f in inputs},'commands':[],'artifacts':{},'scope':'Controlled transport, actual device methods PE32/64; no GPU or production queue activation'}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,name,env=None):
 e={'command':list(map(str,cmd))};r['commands'].append(e);save()
 try:
  with (out/(name+'.log')).open('w') as log:x=subprocess.run(e['command'],stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
 except subprocess.TimeoutExpired:r['status']='timeout';e['timeout_seconds']=90;save();raise
 e['exit_code']=x.returncode;save();assert not x.returncode,name
try:
 for arch in ('i686','x86_64'):
  for enabled in (0,1):
   name=arch+'-'+str(enabled);binary=out/(name+'.exe');source=files[0]
   flags=['-DPW_D3D9_ENABLE_BINDING_TICKETS'] if enabled else []
   run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'wine/ps5/d3d9'),*flags,root/source,*[root/f for f in files[1:]],'-luuid','-ldxguid','-o',binary],name+'-build')
   env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
   run([a.wine_build.resolve()/'loader/wine',binary],name,env);r['artifacts'][str(binary)]=sha(binary)
 assert all(sha(Path(f))==h for f,h in r['sources'].items());r['status']='pass'
except BaseException as e:
 if r['status']=='running':r['status']='failed'
 r['error']=repr(e);raise
finally:save()
print(out/'receipt.json')
