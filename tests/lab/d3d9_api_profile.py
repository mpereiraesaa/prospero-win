#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Separate-DLL profile origin proof, both PE ABIs, no D3D backend."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--header',type=Path,required=True);p.add_argument('--wine-build',type=Path,required=True);p.add_argument('--prefix',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
files=[Path(__file__),Path(__file__).with_suffix('.c'),root/'tools/generate_d3d9_api_observe.py',root/'tools/generate_d3d9_inventory.py',*[root/'wine/ps5/d3d9'/n for n in ('pw_d3d9_api_observe.c','pw_d3d9_api_observe.h','pw_d3d9_api_observe_generated.h')],a.header]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
r={'status':'running','scope':'separate DLL external/internal COM entry accounting; controlled transport counter, no D3D backend or console','sources':{str(f.resolve()):sha(f) for f in files},'commands':[]}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 e={'command':list(map(str,cmd))};r['commands'].append(e);save()
 try:
  with (out/(label+'.stdout')).open('w') as stdout,(out/(label+'.stderr')).open('w') as stderr:
   x=subprocess.run(e['command'],stdout=stdout,stderr=stderr,env=env,timeout=90)
 except subprocess.TimeoutExpired:r['status']='timeout';e['timeout_seconds']=90;save();raise
 e['exit_code']=x.returncode
 if x.returncode:r['status']='failed'
 save();assert not x.returncode,label
 return (out/(label+'.stdout')).read_text(),(out/(label+'.stderr')).read_text()
try:
 run(['python3',root/'tools/generate_d3d9_api_observe.py','--header',a.header,'--output',root/'wine/ps5/d3d9/pw_d3d9_api_observe_generated.h','--check'],'generator')
 for arch in ('i686','x86_64'):
  compiler=arch+'-w64-mingw32-gcc';flags=['-std=c11','-O2','-Wall','-Wextra','-Werror'];dll=out/(arch+'.dll');exe=out/(arch+'.exe')
  run([compiler,*flags,'-DPROFILE_DLL','-shared',Path(__file__).with_suffix('.c'),root/'wine/ps5/d3d9/pw_d3d9_api_observe.c','-o',dll],arch+'-dll')
  run([compiler,*flags,Path(__file__).with_suffix('.c'),'-o',exe],arch+'-exe')
  for profile,diagnostics in ((0,0),(1,0),(0,1),(1,1)):
   env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
   stdout,stderr=run([a.wine_build.resolve()/'loader/wine',exe,str(dll),str(profile),str(diagnostics)],f'{arch}-{profile}-{diagnostics}',env)
   assert f'entries={9 if profile else 0} transactions=3 callback=1 internal_pins_excluded=1 status=0' in stdout,stdout
   assert stderr.count('PW_D3D9_API_FAIL ')==(2 if diagnostics else 0),stderr
   assert stderr.count('PW_D3D9_API_PROFILE ')==(121 if profile else 0),stderr
   if profile:
    assert 'hr=80004005 attempt=1 startup=1 classification_valid=1 api_external_vtable_entries=9 interval_entries=9' in stderr,stderr
    assert 'hr=00000000 attempt=2 startup=0 classification_valid=1 api_external_vtable_entries=9 interval_entries=0' in stderr,stderr
    assert 'boundary=session_close epoch=3 sequence=0 object=0 generation=0' in stderr,stderr
    assert 'method=GetAvailableTextureMem cumulative=119 interval=118 interval_start_attempt=1 interval_end_attempt=120 startup=0' in stderr,stderr
    assert 'method=GetAvailableTextureMem cumulative=120 interval=1 interval_start_attempt=120 interval_end_attempt=120 startup=0' in stderr,stderr
    assert stderr.count('PW_D3D9_API_METHOD ')==11,stderr
 assert r['sources']=={str(f.resolve()):sha(f) for f in files}
 r.update(status='pass',artifacts={str(f):sha(f) for f in out.iterdir() if f.suffix in ('.exe','.dll')});save()
except Exception:
 if r['status']=='running':r['status']='failed';save()
 raise
print(out/'receipt.json')
