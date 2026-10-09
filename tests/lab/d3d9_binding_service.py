#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Negotiated binding service with actual registry/codecs/native dispatcher, mock COM."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for name in ('wine-source','wine-build','prefix','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=['tests/lab/d3d9_binding_service.c','wine/ps5/d3d9/pw_d3d9_service_methods.c','wine/ps5/pw_d3d9_getter_wire.c','wine/ps5/d3d9/pw_d3d9_binding_leases.c','wine/ps5/d3d9/pw_d3d9_native_command.c','wine/ps5/pw_d3d9_objects.c','wine/ps5/pw_d3d9_command_batch.c','wine/ps5/pw_d3d9_binding_plan.c','wine/ps5/pw_d3d9_command_policy.c','wine/ps5/pw_d3d9_command_wire.c']
inputs=[root/n for n in names]+[Path(__file__),root/'tests/lab/d3d9_binding_leases.c']+sorted((root/'wine/ps5').rglob('*.h'))
r={'status':'running','scope':'Real service batch, lease helper, registry, codecs, policy and native dispatcher with controlled COM objects; no DXVK, session activation or console','sources':{str(f):sha(f) for f in inputs},'commands':[],'artifacts':{}}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 entry={'command':list(map(str,cmd))};r['commands'].append(entry);save()
 try:
  with (out/(label+'.log')).open('w') as log:result=subprocess.run(entry['command'],stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
 except subprocess.TimeoutExpired:r['status']='timeout';entry['timeout_seconds']=90;save();raise
 entry['exit_code']=result.returncode;save();assert result.returncode==0,label
try:
 flags=['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DPW_D3D9_ENABLE_BATCH','-DPW_D3D9_ENABLE_BINDING_TICKETS'];files=[root/n for n in names]
 for mode,extra in [('normal',[]),('sanitize',['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'])]:
  binary=out/mode
  run(['cc',*flags,*extra,'-D_WIN64','-D__WINESRC__','-I'+str(a.wine_build.resolve()/'include'),'-I'+str(a.wine_source.resolve()/'include'),*files,'-o',binary],mode+'-build')
  run([binary],mode);r['artifacts'][str(binary)]=sha(binary)
 binary=out/'binding-control.exe';run(['x86_64-w64-mingw32-gcc',*flags,*files,'-o',binary],'pe64-build')
 env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
 run([a.wine_build.resolve()/'loader/wine',binary],'pe64',env);r['artifacts'][str(binary)]=sha(binary)
 assert all(sha(Path(f))==h for f,h in r['sources'].items());r['status']='pass'
except BaseException as e:
 if r['status']=='running':r['status']='failed'
 r['error']=repr(e);raise
finally:save()
print(out/'receipt.json')
