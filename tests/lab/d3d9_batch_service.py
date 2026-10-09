#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Controlled service batch execution, real registry/policy/codecs, no GPU."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for name in ('wine-source','wine-build','prefix','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
names=['tests/lab/d3d9_batch_service.c','wine/ps5/d3d9/pw_d3d9_service_methods.c','wine/ps5/pw_d3d9_objects.c','wine/ps5/pw_d3d9_command_batch.c','wine/ps5/pw_d3d9_command_policy.c','wine/ps5/pw_d3d9_command_wire.c','wine/ps5/pw_d3d9_getter_wire.c']
inputs=[root/n for n in names]+[Path(__file__)]+sorted((root/'wine/ps5').rglob('*.h'))
r={'status':'running','scope':'Actual batch service/registry/policy/codecs with controlled native dispatch; service is PE64 only. No production session enablement, real DXVK or console result.','sources':{str(f):sha(f) for f in inputs},'commands':[],'artifacts':{}}
def run(cmd,label,env=None):
 cmd=list(map(str,cmd));entry={'command':cmd};r['commands'].append(entry)
 with (out/(label+'.log')).open('w') as log:
  result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
 entry['exit']=result.returncode;assert result.returncode==0,label
try:
 flags=['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DPW_D3D9_ENABLE_BATCH']
 files=[root/n for n in names]
 for mode,extra in [('normal',[]),('sanitize',['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'])]:
  binary=out/mode
  run(['cc',*flags,*extra,'-D_WIN64','-D__WINESRC__','-I'+str(a.wine_build.resolve()/'include'),'-I'+str(a.wine_source.resolve()/'include'),*files,'-o',binary],mode+'-build')
  run([binary],mode);r['artifacts'][str(binary)]=sha(binary)
 binary=out/'service-control.exe'
 run(['x86_64-w64-mingw32-gcc',*flags,*files,'-o',binary],'pe64-build')
 env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
 run([a.wine_build.resolve()/'loader/wine',binary],'pe64',env);r['artifacts'][str(binary)]=sha(binary)
 assert all(sha(Path(f))==h for f,h in r['sources'].items())
 r['status']='pass'
except BaseException as e:r.update(status='failed',error=repr(e));raise
finally:(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
print(out/'receipt.json')
