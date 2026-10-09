#!/usr/bin/env python3
"""Run actual PE32/PE64 stateblock proxy lifetime and typed ABI checks."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('wine-build','prefix','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);a.prefix.resolve().mkdir(parents=True,exist_ok=True)
files=[root/'tests/lab/d3d9_stateblock_client.c',root/'wine/ps5/d3d9/pw_d3d9_stateblock_client.c',root/'wine/ps5/pw_d3d9_stateblock_wire.c',root/'wine/ps5/pw_d3d9_draw_shadow.c',root/'wine/ps5/pw_d3d9_transform_shadow.c']
inputs=[*files,Path(__file__),*sorted((root/'wine/ps5').rglob('*.h'))];frozen={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r={'status':'running','sources':frozen,'console_accessed':False,'production_session_integration':False,'commands':[]}
def run(cmd,label,env=None):
 x=subprocess.run(list(map(str,cmd)),text=True,capture_output=True,env=env,timeout=90);(out/(label+'.log')).write_text(x.stdout+x.stderr)
 r['commands'].append({'command':list(map(str,cmd)),'exit_code':x.returncode,'stdout':x.stdout,'stderr':x.stderr});(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');assert x.returncode==0,(label,x.stdout+x.stderr);return x
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'wine/ps5/d3d9')]
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
try:
 for bits,cc in [(32,'i686-w64-mingw32-gcc'),(64,'x86_64-w64-mingw32-gcc')]:
  for enabled in [0,1]:
   name='pe'+str(bits)+'-evidence'+str(enabled);exe=out/(name+'.exe');extra=['-DPW_D3D9_ENABLE_STATE_EVIDENCE'] if enabled else []
   run([cc,*flags,*extra,*files,'-ldxguid','-o',exe],'compile-'+name)
   assert 'PASS stateblock proxy:' in run([a.wine_build.resolve()/'loader/wine',exe],name,env).stdout
 assert frozen=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
 r.update(status='pass',sha256=frozen,scope='Actual PE32/PE64 COM ABI, canonical identity/cache concurrency, strong parent, failure and deferred release ownership with controlled callbacks; no native session/backend claim.')
except BaseException as e:
 r.update(status='failed',error=repr(e));raise
finally:
 (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
print('PASS '+str(out/'receipt.json'))
