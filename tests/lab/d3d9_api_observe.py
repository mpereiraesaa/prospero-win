#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile exact typed observer tables for both PE ABIs; optional non-GPU runs."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--header',type=Path,required=True);p.add_argument('--wine-build',type=Path);p.add_argument('--prefix',type=Path);a=p.parse_args()
if bool(a.wine_build)!=bool(a.prefix):p.error('--wine-build and --prefix must be paired')
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files=[Path(__file__),Path(__file__).with_suffix('.c'),root/'tools/generate_d3d9_api_observe.py',root/'tools/generate_d3d9_inventory.py',*[root/'wine/ps5/d3d9'/n for n in ('pw_d3d9_api_observe.c','pw_d3d9_api_observe.h','pw_d3d9_api_observe_generated.h')],a.header]
r={'status':'running','scope':'typed ABI/controlled callbacks; no GPU or console','sources':{str(f):sha(f) for f in files},'commands':[]}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 entry={'command':list(map(str,cmd))};r['commands'].append(entry);save()
 try:
  with (out/(label+'.stdout')).open('w') as stdout,(out/(label+'.stderr')).open('w') as stderr:
   x=subprocess.run(entry['command'],stdout=stdout,stderr=stderr,env=env,timeout=90)
 except subprocess.TimeoutExpired:
  r['status']='timeout';entry['timeout_seconds']=90;save();raise
 entry['exit_code']=x.returncode
 if x.returncode:r['status']='failed'
 save();assert not x.returncode,label
 return (out/(label+'.stdout')).read_text()
save();run(['python3',root/'tools/generate_d3d9_api_observe.py','--header',a.header,'--output',root/'wine/ps5/d3d9/pw_d3d9_api_observe_generated.h','--check'],'generator')
for arch in ('i686','x86_64'):
 for sink in ('controlled','stderr'):
  label=arch+'-'+sink;binary=out/(label+'.exe')
  run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',*(['-DPW_D3D9_API_OBSERVE_TEST'] if sink=='controlled' else []),Path(__file__).with_suffix('.c'),root/'wine/ps5/d3d9/pw_d3d9_api_observe.c','-o',binary],label+'-build')
  if a.wine_build:
   env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
   for enabled in (0,1):
    runlabel=label+'-'+str(enabled)
    text=run([a.wine_build.resolve()/'loader/wine',binary,*(['on'] if enabled else [])],runlabel,env)
    assert f'enabled={enabled} calls=12 logs={8 if enabled and sink=="controlled" else 0}' in text,text
    assert f'outputs={83 if enabled and sink=="controlled" else 0} bounded=1 snapshot=1 status=0' in text,text
    diagnostic=(out/(runlabel+'.stderr')).read_text()
    expected=enabled and sink=='stderr'
    assert diagnostic.count('PW_D3D9_API_FAIL ')==(6 if expected else 0),diagnostic
    assert diagnostic.count('PW_D3D9_API_RESULT ')==(2 if expected else 0),diagnostic
    assert diagnostic.count('PW_D3D9_API_OUTPUT ')==(83 if expected else 0),diagnostic
    if expected:
     assert diagnostic.count('method=GetAvailableTextureMem ')==64
     assert 'Pitch=-64,pBits=' in diagnostic and 'rect=00000001,00000002,00000003,00000004' in diagnostic
     assert 'a000004b' in diagnostic and 'pCaps=unreadable' in diagnostic
assert r['sources']=={str(f):sha(f) for f in files}
r.update(status='pass' if a.wine_build else 'compile-pass',artifacts={str(x):sha(x) for x in out.glob('*.exe')});save();print(str(out/'receipt.json'))
